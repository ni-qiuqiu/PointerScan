//! BFS 指针链扫描器
//! 
//! 核心扫描逻辑实现：
//! - 多级 BFS 指针链扫描
//! - tmpfile + mmap 避免内存爆炸
//! - 多线程并行处理

use std::fs::File;
use std::io::{BufWriter, Write};
use std::time::Instant;

use rayon::prelude::*;

use crate::buffer_pool::SharedBufferPool;
use crate::error::{Result, ScanError};
use crate::mapqueue::MapQueue;
use crate::memory::VmAreaData;
use crate::pointer::*;
use crate::process::{ProcessMemory, ReadMode};

/// 指针扫描结果
pub struct PointerScanResult {
    /// 指针数据列表
    pub pointers: Vec<PointerData>,
}

/// BFS 指针链扫描器
pub struct ChainScanner {
    /// 进程内存访问器
    process: ProcessMemory,
    /// 全局指针数据（按地址排序）
    global_pointers: Vec<PointerData>,
}

impl ChainScanner {
    /// 创建扫描器
    pub fn new(pid: i32) -> Result<Self> {
        let process = ProcessMemory::new(pid)?;
        Ok(Self {
            process,
            global_pointers: Vec::new(),
        })
    }

    /// 创建指定读取模式的扫描器
    pub fn with_mode(pid: i32, mode: ReadMode) -> Result<Self> {
        let process = ProcessMemory::with_mode(pid, mode)?;
        Ok(Self {
            process,
            global_pointers: Vec::new(),
        })
    }

    /// 通过进程名创建扫描器
    pub fn from_name(name: &str) -> Result<Self> {
        let pid = ProcessMemory::find_pid(name)?;
        Self::new(pid)
    }

    /// 通过进程名创建指定读取模式的扫描器
    pub fn from_name_with_mode(name: &str, mode: ReadMode) -> Result<Self> {
        let pid = ProcessMemory::find_pid(name)?;
        Self::with_mode(pid, mode)
    }

    /// 获取潜在指针数据
    /// 
    /// 扫描指定范围内的内存，找出所有可能是指针的值
    /// 原项目逻辑：
    /// 1. 检查指针值是否在扫描区域范围内
    /// 2. 检查指针值是否落在某个有效的内存区域内
    pub fn get_pointers(
        &mut self,
        _start: u64,
        _end: u64,
        ranges: i32,
        block_size: usize,
    ) -> Result<usize> {
        self.process.set_scan_ranges(ranges);
        
        let scan_areas = self.process.scan_areas().to_vec();
        let pid = self.process.pid();
        let read_mode = self.process.read_mode();
        
        // 获取扫描区域的地址范围（原项目逻辑）
        let min_addr = scan_areas.iter().map(|v| v.start).min().unwrap_or(0);
        let max_addr = scan_areas.iter().map(|v| v.end).max().unwrap_or(0);
        let addr_range = max_addr - min_addr;
        
        println!("Scan area range: 0x{:x} - 0x{:x}", min_addr, max_addr);
        
        // 创建缓冲区池
        let thread_count = rayon::current_num_threads();
        let buffer_count = thread_count + 2;
        let pool = SharedBufferPool::new(buffer_count, block_size);
        
        println!("Scanning {} memory areas with {} threads", 
                 scan_areas.len(), thread_count);

        // 收集所有指针数据
        // 过滤条件与 C++ 一致：可读或可写（prot & PROT_READ || prot & PROT_WRITE）
        let results: Vec<Vec<PointerData>> = scan_areas
            .par_iter()
            .filter(|vma| vma.prot.read || vma.prot.write)
            .flat_map(|vma| {
                let mut blocks = Vec::new();
                let mut addr = vma.start;
                while addr < vma.end {
                    let size = (vma.end - addr).min(block_size as u64) as usize;
                    blocks.push((addr, size));
                    addr += size as u64;
                }
                blocks
            })
            .filter_map(|(addr, size)| {
                let mut guard = pool.acquire_guard().ok()?;
                let buf = guard.as_mut_slice();
                
                // 读取内存块
                let pm = ProcessMemory::with_mode(pid, read_mode).ok()?;
                let read_size = pm.read(addr, &mut buf[..size]).ok()?;
                
                // 扫描指针
                let mut pointers = Vec::new();
                let ptr_size = std::mem::size_of::<u64>();
                
                for offset in (0..read_size).step_by(ptr_size) {
                    if offset + ptr_size > read_size {
                        break;
                    }
                    
                    let value = u64::from_le_bytes(
                        buf[offset..offset + ptr_size].try_into().unwrap()
                    ) & 0xffffffffffff; // 取低48位（原项目逻辑）
                    
                    // 原项目逻辑：检查值是否在扫描区域范围内
                    if value < min_addr || (value - min_addr) > addr_range {
                        continue;
                    }
                    
                    // 原项目逻辑：检查值是否落在某个有效的内存区域内
                    // 二分查找：找到第一个 end >= value 的区域
                    let lower = scan_areas.partition_point(|v| v.end < value);
                    if lower >= scan_areas.len() || value < scan_areas[lower].start {
                        continue;
                    }
                    
                    pointers.push(PointerData::new(addr + offset as u64, value));
                }
                
                Some(pointers)
            })
            .collect();

        // 合并结果
        self.global_pointers.clear();
        for mut result in results {
            self.global_pointers.append(&mut result);
        }

        // 按 address 排序（原项目逻辑）
        self.global_pointers.sort_by_key(|p| p.address);

        println!("Found {} potential pointers", self.global_pointers.len());
        Ok(self.global_pointers.len())
    }

    /// BFS 指针链扫描
    /// 
    /// # Arguments
    /// * `targets` - 目标地址列表
    /// * `depth` - 扫描深度
    /// * `offset` - 最大偏移量
    /// * `output` - 输出文件
    pub fn scan_pointer_chain(
        &mut self,
        targets: &[u64],
        depth: usize,
        offset: u64,
        output: &mut File,
    ) -> Result<usize> {
        if targets.is_empty() {
            return Ok(0);
        }

        let timer = Instant::now();
        
        // 初始化数据结构
        // dirs: 每层的指针目录（使用 MapQueue 避免内存爆炸）
        let mut dirs: Vec<MapQueue<PointerDir>> = (0..=depth)
            .map(|_| MapQueue::new())
            .collect();
        
        // ranges: 找到的静态模块指针范围
        let mut ranges: Vec<PointerRange> = Vec::new();
        let mut first_range_idx = 0;
        let mut total_count = 0usize;

        // 阶段 1: 多级指针链扫描（BFS 展开）
        for level in 0..=depth {
            println!("\nCurrent level: {}", level);

            if level > 0 {
                // 搜索上一层指针的引用
                let curr = self.search_pointer(&dirs[level - 1], offset)?;
                println!("{}: search {} pointers", level, curr.len());

                if curr.is_empty() {
                    break;
                }

                // 过滤指针范围
                self.filter_pointer_ranges(&mut dirs, &mut ranges, curr, level as i32)?;

                // 创建层间索引 - 使用 split_at_mut 避免借用冲突
                let (left, right) = dirs.split_at_mut(level);
                let prev = &left[level - 1];
                let curr = &mut right[0];
                Self::create_assoc_dir_index_static(prev, curr, offset)?;
            } else {
                // Level 0: 转换目标地址为指针数据
                let curr: Vec<PointerData> = targets
                    .iter()
                    .map(|&addr| PointerData::new(addr, 0))
                    .collect();

                // 过滤指针范围
                self.filter_pointer_ranges(&mut dirs, &mut ranges, curr, 0)?;
                first_range_idx = ranges.len();
            }
        }

        // 阶段 2: 补充静态模块索引
        for idx in first_range_idx..ranges.len() {
            let level = ranges[idx].level;
            if level > 0 {
                self.create_assoc_range_index(
                    &dirs[level as usize - 1],
                    &mut ranges[idx].results,
                    offset,
                )?;
            }
        }

        if ranges.is_empty() {
            return Ok(0);
        }

        println!(
            "\nSearch and associate finish, spend: {:.3}s",
            timer.elapsed().as_secs_f64()
        );

        // 阶段 3: 构建指针目录树
        let chain_info = self.build_pointer_dirs_tree(&dirs, &mut ranges)?;
        if chain_info.is_empty() {
            return Ok(0);
        }

        // 阶段 4: 统计每个模块的指针链数量
        for range in &ranges {
            let mut module_count = 0usize;
            let level_count = &chain_info.counts[range.level as usize];

            for dir in range.results.iter() {
                module_count += level_count[dir.end as usize] - level_count[dir.start as usize];
            }

            total_count += module_count;
            println!(
                "Find {} chains from {} {}[{}]",
                module_count, range.level, range.vma.name, range.vma.count
            );
        }

        // 阶段 5: 输出到文件
        self.write_to_file(&chain_info, &ranges, output)?;

        println!(
            "\nFinish write into file, total spend: {:.3}s",
            timer.elapsed().as_secs_f64()
        );

        Ok(total_count)
    }

    /// 在全局指针数据中搜索指向上一层的指针
    /// 
    /// 原项目逻辑 (filter_pointer_from_fmmap):
    /// - input (prev_dirs) 按 address 排序
    /// - 遍历 pcoll (global_pointers)，对每个指针的 value 在 input 中二分查找
    /// - 找到第一个 address >= value 的位置 lower
    /// - 检查: target_addr >= value 且 (target_addr - value) <= offset
    /// - 即: value 在 [target_addr - offset, target_addr] 范围内
    fn search_pointer(
        &self,
        prev_dirs: &MapQueue<PointerDir>,
        offset: u64,
    ) -> Result<Vec<PointerData>> {
        let mut results = Vec::new();
        
        if prev_dirs.is_empty() {
            return Ok(results);
        }

        let prev_slice = prev_dirs.as_slice();
        let prev_len = prev_slice.len();

        // 遍历全局指针数据，找到 value 指向 prev_dirs 中 address 的指针
        for p in &self.global_pointers {
            let value = p.value;
            
            // 二分查找：找到第一个 address >= value 的位置
            let lower = prev_slice.partition_point(|d| d.address < value);
            
            // 检查是否越界
            if lower >= prev_len {
                continue;
            }
            
            let target_addr = prev_slice[lower].address;
            
            // 原项目逻辑: target_addr >= value 且 (target_addr - value) <= offset
            // 即 value 在 [target_addr - offset, target_addr] 范围内
            if target_addr >= value && (target_addr - value) <= offset {
                results.push(*p);
            }
        }

        // 按 address 排序（原项目逻辑）
        results.sort_by_key(|p| p.address);

        Ok(results)
    }

    /// 过滤指针范围：静态区域的加入 ranges，其他加入 dirs
    fn filter_pointer_ranges(
        &self,
        dirs: &mut Vec<MapQueue<PointerDir>>,
        ranges: &mut Vec<PointerRange>,
        curr: Vec<PointerData>,
        level: i32,
    ) -> Result<()> {
        let mut matched_addrs: Vec<u64> = Vec::new();

        let static_modules = self.process.static_modules();

        for vma in static_modules {
            if vma.filter {
                continue;
            }

            // 找出在该模块范围内的指针（按 address 检查）
            let module_pointers: Vec<&PointerData> = curr
                .iter()
                .filter(|p| p.address >= vma.start && p.address < vma.end)
                .collect();

            if module_pointers.is_empty() {
                continue;
            }

            // 创建结果 MapQueue
            let mut results = MapQueue::with_capacity(module_pointers.len())?;
            for p in &module_pointers {
                results.push(PointerDir::from_data(p))?;
                matched_addrs.push(p.address);
            }

            println!("{}[{}]: {} pointers", vma.name, vma.count, module_pointers.len());

            ranges.push(PointerRange::new(
                level,
                VmAreaData::from_static(vma),
                results,
            ));
        }

        // 未匹配的加入 dirs（按 address 排序）
        matched_addrs.sort();
        for p in curr {
            if matched_addrs.binary_search(&p.address).is_err() {
                dirs[level as usize].push(PointerDir::from_data(&p))?;
            }
        }

        Ok(())
    }

    /// 创建层间索引关系（静态版本，避免借用冲突）
    /// 
    /// 原项目 associate_data_index 逻辑：
    /// - prev 按 address 排序
    /// - 对 curr 中每个 dir，根据其 value 在 prev 中查找匹配的 address 范围
    /// - start = 第一个 address >= value 的索引 (get_addr_by_bin_gt: address < value)
    /// - end = 第一个 address > value + offset 的索引 (get_addr_by_bin_lt: address <= value + offset)
    /// - 结果: [start, end) 包含所有 address 在 [value, value + offset] 范围内的项
    fn create_assoc_dir_index_static(
        prev: &MapQueue<PointerDir>,
        curr: &mut MapQueue<PointerDir>,
        offset: u64,
    ) -> Result<()> {
        let prev_slice = prev.as_slice();

        for dir in curr.as_mut_slice() {
            let value = dir.value;

            // start: 第一个 address >= value 的位置
            let start = prev_slice.partition_point(|p| p.address < value);
            // end: 第一个 address > value + offset 的位置（用 saturating_add 防溢出）
            let end = prev_slice.partition_point(|p| p.address <= value.saturating_add(offset));

            dir.start = start as u32;
            dir.end = end as u32;
        }

        Ok(())
    }

    /// 为 range 结果创建索引
    ///
    /// 与 create_assoc_dir_index_static 相同的逻辑
    fn create_assoc_range_index(
        &self,
        prev: &MapQueue<PointerDir>,
        results: &mut MapQueue<PointerDir>,
        offset: u64,
    ) -> Result<()> {
        let prev_slice = prev.as_slice();

        for dir in results.as_mut_slice() {
            let value = dir.value;
            // start: 第一个 address >= value 的位置
            let start = prev_slice.partition_point(|p| p.address < value);
            // end: 第一个 address > value + offset 的位置（用 saturating_add 防溢出）
            let end = prev_slice.partition_point(|p| p.address <= value.saturating_add(offset));
            dir.start = start as u32;
            dir.end = end as u32;
        }

        Ok(())
    }

    /// 构建指针目录树
    fn build_pointer_dirs_tree(
        &self,
        dirs: &[MapQueue<PointerDir>],
        ranges: &mut [PointerRange],
    ) -> Result<ChainInfo> {
        if ranges.is_empty() {
            return Ok(ChainInfo::new(Vec::new(), Vec::new()));
        }

        let max_level = ranges.iter().map(|r| r.level).max().unwrap_or(0) as usize;

        // 初始化 counts 和 contents
        let mut counts: Vec<MapQueue<usize>> = (0..=max_level)
            .map(|_| MapQueue::new())
            .collect();
        
        let mut contents: Vec<MapQueue<*const PointerDir>> = (0..=max_level)
            .map(|_| MapQueue::new())
            .collect();

        // 构建 contents：只收集每层 dirs 的指针目录指针
        // 注意：不能混入 ranges 条目，因为 dir.start/end 索引是相对于 dirs[level-1] 的
        for level in (0..=max_level).rev() {
            for dir in dirs[level].iter() {
                contents[level].push(dir as *const PointerDir)?;
            }
        }

        // 统计每层的累计计数
        counts[0].push(0)?;
        counts[0].push(1)?;

        for level in 1..=max_level {
            // 先收集上一层的数据到临时变量
            let prev_count_data: Vec<usize> = counts[level - 1].as_slice().to_vec();
            let prev_content_len = contents[level - 1].len();
            
            let mut cumulative = 0usize;
            counts[level].push(cumulative)?;

            for i in 0..prev_content_len {
                let dir = unsafe { &*contents[level - 1][i] };
                cumulative += prev_count_data[dir.end as usize] - prev_count_data[dir.start as usize];
                counts[level].push(cumulative)?;
            }
        }

        Ok(ChainInfo::new(counts, contents))
    }

    /// 写入二进制文件
    fn write_to_file(
        &self,
        chain_info: &ChainInfo,
        ranges: &[PointerRange],
        output: &mut File,
    ) -> Result<()> {
        let mut writer = BufWriter::new(output);

        // 写入文件头
        let mut header = ChainHeader::default();
        header.module_count = ranges.len() as i32;
        header.level = chain_info.contents.len() as i32 - 1;

        let header_bytes = unsafe {
            std::slice::from_raw_parts(
                &header as *const _ as *const u8,
                std::mem::size_of::<ChainHeader>(),
            )
        };
        writer.write_all(header_bytes).map_err(ScanError::Io)?;

        // 写入每个模块的符号和数据
        for range in ranges {
            let mut sym = ChainSymbol::default();
            sym.start = range.vma.start;
            sym.range = range.vma.range as i32;
            sym.count = range.vma.count;
            sym.level = range.level;
            sym.pointer_count = range.results.len() as i32;
            sym.set_name(&range.vma.name);

            let sym_bytes = unsafe {
                std::slice::from_raw_parts(
                    &sym as *const _ as *const u8,
                    std::mem::size_of::<ChainSymbol>(),
                )
            };
            writer.write_all(sym_bytes).map_err(ScanError::Io)?;

            // 写入指针数据
            for dir in range.results.iter() {
                let dir_bytes = unsafe {
                    std::slice::from_raw_parts(
                        dir as *const _ as *const u8,
                        std::mem::size_of::<PointerDir>(),
                    )
                };
                writer.write_all(dir_bytes).map_err(ScanError::Io)?;
            }
        }

        // 写入每层的内容
        for (level, content) in chain_info.contents.iter().enumerate() {
            if level >= chain_info.contents.len() - 1 {
                break;
            }

            let llen = ChainLevelLen {
                module_count: 0,
                count: content.len() as u32,
                level: level as i32,
            };

            let llen_bytes = unsafe {
                std::slice::from_raw_parts(
                    &llen as *const _ as *const u8,
                    std::mem::size_of::<ChainLevelLen>(),
                )
            };
            writer.write_all(llen_bytes).map_err(ScanError::Io)?;

            for &ptr in content.iter() {
                let dir = unsafe { &*ptr };
                let dir_bytes = unsafe {
                    std::slice::from_raw_parts(
                        dir as *const _ as *const u8,
                        std::mem::size_of::<PointerDir>(),
                    )
                };
                writer.write_all(dir_bytes).map_err(ScanError::Io)?;
            }
        }

        writer.flush().map_err(ScanError::Io)?;
        Ok(())
    }

    /// 输出为文本格式
    /// 
    /// 原项目格式: 模块名[编号] + 0x偏移 -> + 0x偏移 -> + 0x偏移 ...
    pub fn write_to_text(
        &self,
        chain_info: &ChainInfo,
        ranges: &[PointerRange],
        output: &mut File,
    ) -> Result<usize> {
        let mut writer = BufWriter::new(output);
        let mut total_chains = 0usize;

        for range in ranges {
            println!("Writing chains for {}[{}] at level {}, count: {}", 
                     range.vma.name, range.vma.count, range.level, range.results.len());
            
            for dir in range.results.iter() {
                // 构建起始部分: 模块名[编号] + 0x偏移
                let base_offset = dir.address - range.vma.start;
                let prefix = format!("{}[{}] + 0x{:X}", range.vma.name, range.vma.count, base_offset);
                
                let chains = self.write_chain_recursive_text(
                    &mut writer,
                    chain_info,
                    dir,
                    range.level as usize,
                    prefix,
                )?;
                total_chains += chains;
            }
        }

        writer.flush().map_err(ScanError::Io)?;
        println!("Total chains written to txt: {}", total_chains);
        Ok(total_chains)
    }

    /// 递归输出指针链（文本格式）
    /// 
    /// 原项目逻辑：
    /// - level == 0 时输出完整链
    /// - 否则遍历 [dir.start, dir.end) 范围内的子节点
    /// - 偏移计算: child.address - dir.value
    fn write_chain_recursive_text<W: Write>(
        &self,
        writer: &mut W,
        chain_info: &ChainInfo,
        dir: &PointerDir,
        level: usize,
        prefix: String,
    ) -> Result<usize> {
        if level == 0 {
            // 到达最底层，输出完整链
            writeln!(writer, "{}", prefix).map_err(ScanError::Io)?;
            return Ok(1);
        }

        let mut count = 0;
        let content = &chain_info.contents[level - 1];

        for i in dir.start..dir.end {
            let child = unsafe { &*content[i as usize] };
            // 原项目格式: -> + 0x偏移
            let offset = child.address.wrapping_sub(dir.value);
            let new_prefix = format!("{} -> + 0x{:X}", prefix, offset);
            
            count += self.write_chain_recursive_text(
                writer,
                chain_info,
                child,
                level - 1,
                new_prefix,
            )?;
        }

        Ok(count)
    }

    /// BFS 指针链扫描并直接输出到文本文件
    /// 
    /// 与 scan_pointer_chain 类似，但直接输出文本格式
    pub fn scan_pointer_chain_to_txt(
        &mut self,
        targets: &[u64],
        depth: usize,
        offset: u64,
        output: &mut File,
    ) -> Result<usize> {
        if targets.is_empty() {
            return Ok(0);
        }

        let timer = Instant::now();
        
        // 初始化数据结构
        let mut dirs: Vec<MapQueue<PointerDir>> = (0..=depth)
            .map(|_| MapQueue::new())
            .collect();
        
        let mut ranges: Vec<PointerRange> = Vec::new();
        let mut first_range_idx = 0;

        // 阶段 1: 多级指针链扫描（BFS 展开）
        for level in 0..=depth {
            println!("\nCurrent level: {}", level);

            if level > 0 {
                let curr = self.search_pointer(&dirs[level - 1], offset)?;
                println!("{}: search {} pointers", level, curr.len());

                if curr.is_empty() {
                    break;
                }

                self.filter_pointer_ranges(&mut dirs, &mut ranges, curr, level as i32)?;

                let (left, right) = dirs.split_at_mut(level);
                let prev = &left[level - 1];
                let curr = &mut right[0];
                Self::create_assoc_dir_index_static(prev, curr, offset)?;
            } else {
                let curr: Vec<PointerData> = targets
                    .iter()
                    .map(|&addr| PointerData::new(addr, 0))
                    .collect();

                self.filter_pointer_ranges(&mut dirs, &mut ranges, curr, 0)?;
                first_range_idx = ranges.len();
            }
        }

        // 阶段 2: 补充静态模块索引
        for idx in first_range_idx..ranges.len() {
            let level = ranges[idx].level;
            if level > 0 {
                self.create_assoc_range_index(
                    &dirs[level as usize - 1],
                    &mut ranges[idx].results,
                    offset,
                )?;
            }
        }

        if ranges.is_empty() {
            return Ok(0);
        }

        println!(
            "\nSearch and associate finish, spend: {:.3}s",
            timer.elapsed().as_secs_f64()
        );

        // 阶段 3: 构建指针目录树
        let chain_info = self.build_pointer_dirs_tree(&dirs, &mut ranges)?;
        if chain_info.is_empty() {
            return Ok(0);
        }

        // 阶段 4: 统计并输出
        let mut total_count = 0usize;
        for range in &ranges {
            let mut module_count = 0usize;
            let level_count = &chain_info.counts[range.level as usize];

            for dir in range.results.iter() {
                module_count += level_count[dir.end as usize] - level_count[dir.start as usize];
            }

            total_count += module_count;
            println!(
                "Find {} chains from {} {}[{}]",
                module_count, range.level, range.vma.name, range.vma.count
            );
        }

        // 阶段 5: 输出到文本文件
        self.write_to_text(&chain_info, &ranges, output)?;

        println!(
            "\nFinish write into file, total spend: {:.3}s",
            timer.elapsed().as_secs_f64()
        );

        Ok(total_count)
    }

    /// 获取进程内存访问器
    pub fn process(&self) -> &ProcessMemory {
        &self.process
    }

    /// 获取全局指针数据
    pub fn global_pointers(&self) -> &[PointerData] {
        &self.global_pointers
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_mapqueue_in_scanner() {
        let mut queue: MapQueue<PointerDir> = MapQueue::new();
        
        for i in 0..1000 {
            queue.push(PointerDir::new(i * 8, i * 16)).unwrap();
        }

        assert_eq!(queue.len(), 1000);
        assert_eq!(queue[500].address, 4000);
    }
}
