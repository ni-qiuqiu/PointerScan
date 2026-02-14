//! 指针链对比模块
//!
//! 支持两种对比模式：
//! - 二进制文件对比：mmap 解析后树上直接匹配，流式写入报告
//! - 文本文件对比：逐行解析后集合交集

use std::collections::{HashMap, HashSet};
use std::fs::{self, File};
use std::io::{BufRead, BufReader, BufWriter, Write};
use std::time::Instant;

use memmap2::Mmap;

use crate::error::{Result, ScanError};
use crate::pointer::{ChainHeader, ChainLevelLen, ChainSymbol, PointerDir};

// ============ 二进制文件解析数据结构 ============

/// 模块符号 + 关联的根节点数据（零拷贝切片）
struct BinSymIntegr<'a> {
    sym: &'a ChainSymbol,
    data: &'a [PointerDir],
}

/// 解析后的二进制指针链数据（零拷贝，生命周期绑定 mmap）
struct BinChainInfo<'a> {
    syms: Vec<BinSymIntegr<'a>>,
    contents: Vec<&'a [PointerDir]>,
}

/// 对比结果
pub struct CompareResult {
    pub lhs_total: usize,
    pub rhs_total: usize,
    pub unchanged: usize,
    pub modules: Vec<ModuleChainDiff>,
}

/// 单个模块的对比结果
pub struct ModuleChainDiff {
    pub module_name: String,
    pub module_index: i32,
    pub common_count: usize,
    /// 文本对比时存储完整路径；二进制对比时为空（直接流式写入）
    pub common: Vec<Vec<u64>>,
}

// ============ 二进制文件解析 ============

const SIGNATURE_PREFIX: &[u8] = b".bin from chainer";

fn parse_bin_data(mmap: &Mmap) -> Result<BinChainInfo<'_>> {
    let data = &mmap[..];
    let header_size = std::mem::size_of::<ChainHeader>();
    if data.len() < header_size {
        return Err(ScanError::InvalidChain);
    }

    let header: &ChainHeader = unsafe { &*(data.as_ptr() as *const ChainHeader) };

    // 验证签名
    if !header.sign.starts_with(SIGNATURE_PREFIX) {
        return Err(ScanError::InvalidChain);
    }
    if header.module_count < 0 || header.level < 0 || header.size != 8 {
        return Err(ScanError::InvalidChain);
    }

    let mut offset = header_size;
    let sym_size = std::mem::size_of::<ChainSymbol>();
    let dir_size = std::mem::size_of::<PointerDir>();
    let llen_size = std::mem::size_of::<ChainLevelLen>();

    // 解析模块符号和根节点数据
    let mut syms = Vec::with_capacity(header.module_count as usize);
    for _ in 0..header.module_count {
        if offset + sym_size > data.len() {
            return Err(ScanError::InvalidChain);
        }
        let sym: &ChainSymbol = unsafe { &*(data[offset..].as_ptr() as *const ChainSymbol) };
        offset += sym_size;

        let count = sym.pointer_count as usize;
        let data_bytes = count * dir_size;
        if offset + data_bytes > data.len() {
            return Err(ScanError::InvalidChain);
        }
        let dirs: &[PointerDir] = unsafe {
            std::slice::from_raw_parts(data[offset..].as_ptr() as *const PointerDir, count)
        };
        offset += data_bytes;

        syms.push(BinSymIntegr { sym, data: dirs });
    }

    // 解析各层内容
    let mut contents: Vec<&[PointerDir]> = vec![&[]; header.level as usize];
    for _ in 0..header.level {
        if offset + llen_size > data.len() {
            return Err(ScanError::InvalidChain);
        }
        let llen: &ChainLevelLen = unsafe { &*(data[offset..].as_ptr() as *const ChainLevelLen) };
        offset += llen_size;

        let count = llen.count as usize;
        let data_bytes = count * dir_size;
        if offset + data_bytes > data.len() {
            return Err(ScanError::InvalidChain);
        }
        let dirs: &[PointerDir] = unsafe {
            std::slice::from_raw_parts(data[offset..].as_ptr() as *const PointerDir, count)
        };
        offset += data_bytes;

        let level = llen.level as usize;
        if level < contents.len() {
            contents[level] = dirs;
        }
    }

    Ok(BinChainInfo { syms, contents })
}

// ============ 前缀和统计链数量（O(N) 替代递归 O(total_chains)） ============

/// 自底向上构建每层的前缀和数组
/// prefix_sums[k] 长度为 contents[k].len() + 1
/// prefix_sums[k][i] = contents[k][0..i] 所有节点的链数量之和
fn build_prefix_sums(info: &BinChainInfo<'_>) -> Vec<Vec<u64>> {
    let level_count = info.contents.len();
    let mut prefix_sums: Vec<Vec<u64>> = Vec::with_capacity(level_count);

    for k in 0..level_count {
        let nodes = info.contents[k];
        let mut prefix = Vec::with_capacity(nodes.len() + 1);
        prefix.push(0u64);

        if k == 0 {
            // level 0: 每个节点 = 1 条链
            for i in 0..nodes.len() {
                prefix.push(prefix[i] + 1);
            }
        } else {
            // level k: 每个节点的链数 = prefix_sums[k-1][end] - prefix_sums[k-1][start]
            let prev = &prefix_sums[k - 1];
            let prev_len = prev.len();
            for (i, node) in nodes.iter().enumerate() {
                let start = (node.start as usize).min(prev_len - 1);
                let end = (node.end as usize).min(prev_len - 1);
                let node_chains = prev[end] - prev[start];
                prefix.push(prefix[i] + node_chains);
            }
        }

        prefix_sums.push(prefix);
    }

    prefix_sums
}

/// 用前缀和查询一个节点的链数量
#[inline]
fn chains_of(prefix_sums: &[Vec<u64>], dir: &PointerDir, level: usize) -> u64 {
    if level == 0 {
        return 1;
    }
    let child_level = level - 1;
    if child_level >= prefix_sums.len() {
        return 0;
    }
    let prev = &prefix_sums[child_level];
    let prev_len = prev.len();
    let start = (dir.start as usize).min(prev_len - 1);
    let end = (dir.end as usize).min(prev_len - 1);
    prev[end] - prev[start]
}

fn count_chains_prefix_sum(info: &BinChainInfo<'_>, label: &str) -> (usize, Vec<Vec<u64>>) {
    let prefix_sums = build_prefix_sums(info);

    let mut total = 0u64;
    for (idx, sym) in info.syms.iter().enumerate() {
        let level = sym.sym.level as usize;
        let mut sym_count = 0u64;
        for dir in sym.data {
            sym_count += chains_of(&prefix_sums, dir, level);
        }
        if sym_count > 0 {
            println!("  [{}] [{}/{}] {}[{}]: {} 条链",
                     label, idx + 1, info.syms.len(),
                     sym.sym.get_name(), sym.sym.count, sym_count);
        }
        total += sym_count;
    }

    (total as usize, prefix_sums)
}

// ============ 树上递归匹配子树 ============

/// 匹配进度追踪（跨递归层级）
struct MatchProgress {
    matched: usize,
    next_check: usize,
    timer: Instant,
    last_print: f64,
}

const PROGRESS_CHECK_INTERVAL: usize = 500_000;

impl MatchProgress {
    fn new() -> Self {
        Self {
            matched: 0,
            next_check: PROGRESS_CHECK_INTERVAL,
            timer: Instant::now(),
            last_print: 0.0,
        }
    }

    #[inline]
    fn add(&mut self, count: usize) {
        self.matched += count;
        if self.matched >= self.next_check {
            self.next_check = self.matched + PROGRESS_CHECK_INTERVAL;
            let elapsed = self.timer.elapsed().as_secs_f64();
            if elapsed - self.last_print >= 2.0 {
                let rate = self.matched as f64 / elapsed;
                println!("    已匹配 {} 条链, {:.1}s, {:.0} 条/s",
                         self.matched, elapsed, rate);
                self.last_print = elapsed;
            }
        }
    }
}

fn emit_chain_line<W: Write>(w: &mut W, module_name: &str, module_index: i32, path: &[u64]) {
    let _ = write!(w, "    = {}[{}]", module_name, module_index);
    for (i, &off) in path.iter().enumerate() {
        if i == 0 {
            let _ = write!(w, " + 0x{:X}", off);
        } else {
            let _ = write!(w, " -> + 0x{:X}", off);
        }
    }
    let _ = writeln!(w);
}

fn match_subtrees<W: Write>(
    lhs_info: &BinChainInfo<'_>,
    rhs_info: &BinChainInfo<'_>,
    lhs_prefix: &[Vec<u64>],
    rhs_prefix: &[Vec<u64>],
    lhs_dir: &PointerDir,
    rhs_dir: &PointerDir,
    level: i32,
    path: &mut Vec<u64>,
    module_name: &str,
    module_index: i32,
    report: &mut Option<BufWriter<W>>,
    progress: &mut MatchProgress,
) -> usize {
    if level == 0 {
        if let Some(ref mut w) = report {
            emit_chain_line(w, module_name, module_index, path);
        }
        return 1;
    }

    let child_level = (level - 1) as usize;
    if child_level >= lhs_info.contents.len() || child_level >= rhs_info.contents.len() {
        return 0;
    }

    let lhs_children = lhs_info.contents[child_level];
    let rhs_children = rhs_info.contents[child_level];

    let (lhs_start, lhs_end) = (lhs_dir.start as usize, lhs_dir.end as usize);
    let lhs_end = lhs_end.min(lhs_children.len());
    let (rhs_start, rhs_end) = (rhs_dir.start as usize, rhs_dir.end as usize);
    let rhs_end = rhs_end.min(rhs_children.len());

    // level == 1 快速路径：子节点是叶子，直接计数，避免逐个递归到 level 0
    if level == 1 {
        let mut li = lhs_start;
        let mut ri = rhs_start;
        let mut count = 0usize;

        while li < lhs_end && ri < rhs_end {
            let lhs_off = lhs_children[li].address.wrapping_sub(lhs_dir.value);
            let rhs_off = rhs_children[ri].address.wrapping_sub(rhs_dir.value);

            if lhs_off < rhs_off {
                li += 1;
            } else if lhs_off > rhs_off {
                ri += 1;
            } else {
                let mut lhs_dup_end = li + 1;
                while lhs_dup_end < lhs_end
                    && lhs_children[lhs_dup_end].address.wrapping_sub(lhs_dir.value) == lhs_off
                {
                    lhs_dup_end += 1;
                }
                let mut rhs_dup_end = ri + 1;
                while rhs_dup_end < rhs_end
                    && rhs_children[rhs_dup_end].address.wrapping_sub(rhs_dir.value) == lhs_off
                {
                    rhs_dup_end += 1;
                }

                let pairs = (lhs_dup_end - li) * (rhs_dup_end - ri);
                if let Some(ref mut w) = report {
                    path.push(lhs_off);
                    for _ in 0..pairs {
                        emit_chain_line(w, module_name, module_index, path);
                    }
                    path.pop();
                }
                count += pairs;
                progress.add(pairs);

                li = lhs_dup_end;
                ri = rhs_dup_end;
            }
        }
        return count;
    }

    // 通用路径：双指针归并 + 递归
    let child_tree_level = child_level;
    let mut li = lhs_start;
    let mut ri = rhs_start;
    let mut count = 0;

    while li < lhs_end && ri < rhs_end {
        let lhs_off = lhs_children[li].address.wrapping_sub(lhs_dir.value);
        let rhs_off = rhs_children[ri].address.wrapping_sub(rhs_dir.value);

        if lhs_off < rhs_off {
            li += 1;
        } else if lhs_off > rhs_off {
            ri += 1;
        } else {
            let mut lhs_dup_end = li + 1;
            while lhs_dup_end < lhs_end
                && lhs_children[lhs_dup_end].address.wrapping_sub(lhs_dir.value) == lhs_off
            {
                lhs_dup_end += 1;
            }
            let mut rhs_dup_end = ri + 1;
            while rhs_dup_end < rhs_end
                && rhs_children[rhs_dup_end].address.wrapping_sub(rhs_dir.value) == lhs_off
            {
                rhs_dup_end += 1;
            }

            path.push(lhs_off);
            for lx in li..lhs_dup_end {
                if chains_of(lhs_prefix, &lhs_children[lx], child_tree_level) == 0 {
                    continue;
                }
                for rx in ri..rhs_dup_end {
                    if chains_of(rhs_prefix, &rhs_children[rx], child_tree_level) == 0 {
                        continue;
                    }
                    count += match_subtrees(
                        lhs_info,
                        rhs_info,
                        lhs_prefix,
                        rhs_prefix,
                        &lhs_children[lx],
                        &rhs_children[rx],
                        level - 1,
                        path,
                        module_name,
                        module_index,
                        report,
                        progress,
                    );
                }
            }
            path.pop();

            li = lhs_dup_end;
            ri = rhs_dup_end;
        }
    }

    count
}

fn match_module_roots<W: Write>(
    lhs_info: &BinChainInfo<'_>,
    rhs_info: &BinChainInfo<'_>,
    lhs_prefix: &[Vec<u64>],
    rhs_prefix: &[Vec<u64>],
    lhs_sym: &BinSymIntegr<'_>,
    rhs_sym: &BinSymIntegr<'_>,
    lhs_chains: u64,
    rhs_chains: u64,
    report: &mut Option<BufWriter<W>>,
) -> usize {
    if lhs_sym.data.is_empty() || rhs_sym.data.is_empty() {
        return 0;
    }

    let mut path = Vec::with_capacity(lhs_sym.sym.level as usize + 1);
    let module_name = lhs_sym.sym.get_name();
    let module_index = lhs_sym.sym.count;
    let level = lhs_sym.sym.level as usize;

    if let Some(ref mut w) = report {
        let _ = writeln!(w, "  保持不变的链:");
    }

    println!("    根节点: 旧={} 新={}, 链数: 旧={} 新={}, 上界={}",
             lhs_sym.data.len(), rhs_sym.data.len(),
             lhs_chains, rhs_chains,
             lhs_chains.min(rhs_chains));

    let lhs_total_roots = lhs_sym.data.len();
    let mut progress = MatchProgress::new();

    // 双指针归并：根节点按 address 排序 → offset (address - sym.start) 也有序
    let mut li = 0usize;
    let mut ri = 0usize;
    let mut count = 0usize;

    while li < lhs_sym.data.len() && ri < rhs_sym.data.len() {
        let lhs_off = lhs_sym.data[li].address.wrapping_sub(lhs_sym.sym.start);
        let rhs_off = rhs_sym.data[ri].address.wrapping_sub(rhs_sym.sym.start);

        if lhs_off < rhs_off {
            li += 1;
        } else if lhs_off > rhs_off {
            ri += 1;
        } else {
            // 找出两侧重复节点的范围
            let mut lhs_dup_end = li + 1;
            while lhs_dup_end < lhs_sym.data.len()
                && lhs_sym.data[lhs_dup_end].address.wrapping_sub(lhs_sym.sym.start) == lhs_off
            {
                lhs_dup_end += 1;
            }
            let mut rhs_dup_end = ri + 1;
            while rhs_dup_end < rhs_sym.data.len()
                && rhs_sym.data[rhs_dup_end].address.wrapping_sub(rhs_sym.sym.start) == lhs_off
            {
                rhs_dup_end += 1;
            }

            // 交叉匹配所有重复对，用前缀和剪枝
            for lx in li..lhs_dup_end {
                if chains_of(lhs_prefix, &lhs_sym.data[lx], level) == 0 {
                    continue;
                }
                for rx in ri..rhs_dup_end {
                    if chains_of(rhs_prefix, &rhs_sym.data[rx], level) == 0 {
                        continue;
                    }
                    path.clear();
                    path.push(lhs_off);
                    count += match_subtrees(
                        lhs_info,
                        rhs_info,
                        lhs_prefix,
                        rhs_prefix,
                        &lhs_sym.data[lx],
                        &rhs_sym.data[rx],
                        lhs_sym.sym.level,
                        &mut path,
                        module_name,
                        module_index,
                        report,
                        &mut progress,
                    );
                }
            }

            li = lhs_dup_end;
            ri = rhs_dup_end;
        }
    }

    // 最终进度
    if progress.matched > 0 {
        let elapsed = progress.timer.elapsed().as_secs_f64();
        println!("    根节点遍历完成: {}/{}, 匹配 {} 条链, {:.3}s",
                 li.min(lhs_total_roots), lhs_total_roots, count, elapsed);
    }

    count
}

// ============ 公开 API ============

/// 二进制文件对比
pub fn compare_bin_files(
    lhs_path: &str,
    rhs_path: &str,
    report_path: Option<&str>,
) -> Result<CompareResult> {
    let timer = Instant::now();

    println!("[对比] 打开文件...");
    println!("  旧文件: {} ({} bytes)", lhs_path,
             fs::metadata(lhs_path).map(|m| m.len()).unwrap_or(0));
    println!("  新文件: {} ({} bytes)", rhs_path,
             fs::metadata(rhs_path).map(|m| m.len()).unwrap_or(0));

    let lhs_file = File::open(lhs_path).map_err(ScanError::Io)?;
    let rhs_file = File::open(rhs_path).map_err(ScanError::Io)?;

    let lhs_mmap = unsafe { Mmap::map(&lhs_file) }.map_err(|e| ScanError::MmapFailed(e.to_string()))?;
    let rhs_mmap = unsafe { Mmap::map(&rhs_file) }.map_err(|e| ScanError::MmapFailed(e.to_string()))?;

    println!("[对比] 解析二进制数据...");
    let lhs_info = parse_bin_data(&lhs_mmap)?;
    let rhs_info = parse_bin_data(&rhs_mmap)?;
    println!("  旧文件: {} 个模块, {} 层", lhs_info.syms.len(), lhs_info.contents.len());
    println!("  新文件: {} 个模块, {} 层", rhs_info.syms.len(), rhs_info.contents.len());

    println!("[对比] 统计链数量...");
    let (lhs_total, lhs_prefix) = count_chains_prefix_sum(&lhs_info, "旧");
    let (rhs_total, rhs_prefix) = count_chains_prefix_sum(&rhs_info, "新");
    println!("  旧文件总计: {} 条链", lhs_total);
    println!("  新文件总计: {} 条链", rhs_total);

    let mut result = CompareResult {
        lhs_total,
        rhs_total,
        unchanged: 0,
        modules: Vec::new(),
    };

    // 构建 rhs 模块索引
    let mut rhs_module_map: HashMap<(&str, i32), usize> = HashMap::new();
    for (i, sym) in rhs_info.syms.iter().enumerate() {
        rhs_module_map.insert((sym.sym.get_name(), sym.sym.count), i);
    }

    let mut report_writer: Option<BufWriter<File>> = if let Some(path) = report_path {
        let f = File::create(path).map_err(ScanError::Io)?;
        let mut w = BufWriter::new(f);
        let _ = writeln!(w, "=== 指针链二进制文件对比结果 ===\n");
        Some(w)
    } else {
        None
    };

    println!("[对比] 匹配模块...");
    let match_timer = Instant::now();
    let mut total_common = 0usize;
    let module_count = lhs_info.syms.len();

    for (idx, lhs_sym) in lhs_info.syms.iter().enumerate() {
        let name = lhs_sym.sym.get_name();
        let index = lhs_sym.sym.count;

        let rhs_idx = match rhs_module_map.get(&(name, index)) {
            Some(&idx) => idx,
            None => continue,
        };

        // 用前缀和计算该模块的链数量
        let lhs_level = lhs_sym.sym.level as usize;
        let rhs_level = rhs_info.syms[rhs_idx].sym.level as usize;
        let mut lhs_chains = 0u64;
        for dir in lhs_sym.data {
            lhs_chains += chains_of(&lhs_prefix, dir, lhs_level);
        }
        let mut rhs_chains = 0u64;
        for dir in rhs_info.syms[rhs_idx].data {
            rhs_chains += chains_of(&rhs_prefix, dir, rhs_level);
        }

        // 任一侧为 0 则跳过
        if lhs_chains == 0 || rhs_chains == 0 {
            continue;
        }

        if let Some(ref mut w) = report_writer {
            let _ = writeln!(w, "模块: {}[{}]", name, index);
        }

        println!("  [{}/{}] {}[{}] 开始匹配...",
                 idx + 1, module_count, name, index);

        let mod_timer = Instant::now();
        let common_count = match_module_roots(
            &lhs_info,
            &rhs_info,
            &lhs_prefix,
            &rhs_prefix,
            lhs_sym,
            &rhs_info.syms[rhs_idx],
            lhs_chains,
            rhs_chains,
            &mut report_writer,
        );

        let mod_elapsed = mod_timer.elapsed().as_secs_f64();
        total_common += common_count;

        if common_count > 0 {
            println!("  [{}/{}] {}[{}]: {} 条匹配链, 模块耗时 {:.3}s, 累计 {} 条",
                     idx + 1, module_count, name, index,
                     common_count, mod_elapsed, total_common);
            result.modules.push(ModuleChainDiff {
                module_name: name.to_string(),
                module_index: index,
                common_count,
                common: Vec::new(),
            });
        } else {
            println!("  [{}/{}] {}[{}]: 无匹配, 模块耗时 {:.3}s",
                     idx + 1, module_count, name, index, mod_elapsed);
        }

        if let Some(ref mut w) = report_writer {
            let _ = writeln!(w);
        }
    }

    println!("[对比] 匹配完成, 共 {} 条匹配链, 匹配阶段耗时 {:.3}s",
             total_common, match_timer.elapsed().as_secs_f64());

    // 写入统计摘要
    if let Some(ref mut w) = report_writer {
        let _ = writeln!(w, "--- 统计 ---");
        let _ = writeln!(w, "旧文件链数量: {}", result.lhs_total);
        let _ = writeln!(w, "新文件链数量: {}", result.rhs_total);
        let _ = writeln!(w, "保持不变链数量: {}", total_common);
        let _ = w.flush();
    }

    result.unchanged = total_common;
    println!("[对比] 完成，耗时 {:.3}s", timer.elapsed().as_secs_f64());
    Ok(result)
}

// ============ 文本文件对比 ============

/// 解析后的链签名
struct ChainSignature {
    module_name: String,
    module_index: i32,
    offsets: Vec<u64>,
}

fn parse_txt_line(line: &str) -> Option<ChainSignature> {
    let line = line.trim();
    if line.is_empty() {
        return None;
    }

    // 查找 '['
    let bracket_l = line.find('[')?;
    if bracket_l == 0 {
        return None;
    }
    let module_name = line[..bracket_l].to_string();

    // 解析 module_index
    let rest = &line[bracket_l + 1..];
    let bracket_r = rest.find(']')?;
    let module_index: i32 = rest[..bracket_r].parse().ok()?;

    // 解析偏移
    let rest = &rest[bracket_r + 1..];
    let mut offsets = Vec::new();
    let mut pos = 0;
    while pos < rest.len() {
        if let Some(idx) = rest[pos..].find("+ 0x") {
            let hex_start = pos + idx + 4;
            let hex_end = rest[hex_start..]
                .find(|c: char| !c.is_ascii_hexdigit())
                .map(|i| hex_start + i)
                .unwrap_or(rest.len());
            if hex_end > hex_start {
                if let Ok(val) = u64::from_str_radix(&rest[hex_start..hex_end], 16) {
                    offsets.push(val);
                }
            }
            pos = hex_end;
        } else {
            break;
        }
    }

    if offsets.is_empty() {
        return None;
    }

    Some(ChainSignature {
        module_name,
        module_index,
        offsets,
    })
}

fn parse_txt_file(path: &str) -> Result<Vec<ChainSignature>> {
    let file = File::open(path).map_err(ScanError::Io)?;
    let reader = BufReader::new(file);
    let mut result = Vec::with_capacity(4096);

    for line in reader.lines() {
        let line = line.map_err(ScanError::Io)?;
        if let Some(sig) = parse_txt_line(&line) {
            result.push(sig);
        }
    }

    Ok(result)
}

type ChainSet = HashSet<Vec<u64>>;
type ModuleChainMap = HashMap<(String, i32), ChainSet>;

fn build_chain_map(chains: &[ChainSignature]) -> ModuleChainMap {
    let mut map: ModuleChainMap = HashMap::new();
    for chain in chains {
        map.entry((chain.module_name.clone(), chain.module_index))
            .or_default()
            .insert(chain.offsets.clone());
    }
    map
}

/// 文本文件对比
pub fn compare_txt_files(
    lhs_path: &str,
    rhs_path: &str,
) -> Result<CompareResult> {
    let timer = Instant::now();

    println!("[对比] 解析文本文件...");
    let lhs_chains = parse_txt_file(lhs_path)?;
    println!("  旧文件: {} 条链", lhs_chains.len());
    let rhs_chains = parse_txt_file(rhs_path)?;
    println!("  新文件: {} 条链", rhs_chains.len());

    let mut result = CompareResult {
        lhs_total: lhs_chains.len(),
        rhs_total: rhs_chains.len(),
        unchanged: 0,
        modules: Vec::new(),
    };

    println!("[对比] 构建索引...");
    let lhs_map = build_chain_map(&lhs_chains);
    let rhs_map = build_chain_map(&rhs_chains);
    println!("  旧文件: {} 个模块", lhs_map.len());
    println!("  新文件: {} 个模块", rhs_map.len());

    println!("[对比] 匹配链...");
    let mut unchanged = 0usize;
    let mut visited: HashSet<(String, i32)> = HashSet::new();

    for (key, lhs_set) in &lhs_map {
        visited.insert(key.clone());

        if let Some(rhs_set) = rhs_map.get(key) {
            let common: Vec<Vec<u64>> = lhs_set
                .iter()
                .filter(|chain| rhs_set.contains(*chain))
                .cloned()
                .collect();

            if !common.is_empty() {
                println!("  {}[{}]: {} 条匹配链", key.0, key.1, common.len());
                unchanged += common.len();
                result.modules.push(ModuleChainDiff {
                    module_name: key.0.clone(),
                    module_index: key.1,
                    common_count: common.len(),
                    common,
                });
            }
        }
    }

    // rhs 中独有的模块（无交集，不产生 diff）
    // 与 C++ 逻辑一致：遍历但不会产生 common

    result.unchanged = unchanged;
    println!("[对比] 完成，耗时 {:.3}s", timer.elapsed().as_secs_f64());
    Ok(result)
}
