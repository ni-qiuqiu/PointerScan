//! 进程内存读取模块
//!
//! 支持两种读取模式：
//! - process_vm_readv 系统调用（默认）
//! - /proc/[pid]/mem 文件IO读取（解决不可读内存问题）

use std::collections::HashMap;
use std::fs::File;
use std::io::{BufRead, BufReader};
use std::os::unix::io::AsRawFd;
use std::sync::Arc;

use crate::error::{Result, ScanError};
use crate::memory::{MemRange, VmAreaData, VmStaticData};

/// 内存读取模式
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ReadMode {
    /// 默认：process_vm_readv 系统调用
    ProcessVmReadv,
    /// /proc/[pid]/mem 文件IO读取
    ProcMemIo,
}

/// 进程内存访问器
pub struct ProcessMemory {
    /// 目标进程 PID
    pid: i32,
    /// 内存区域列表
    vm_areas: Vec<VmAreaData>,
    /// 指针扫描区域列表
    scan_areas: Vec<VmAreaData>,
    /// 静态模块列表
    static_modules: Vec<VmStaticData>,
    /// 读取模式
    read_mode: ReadMode,
    /// /proc/[pid]/mem 文件描述符（IO模式使用）
    mem_file: Option<File>,
}

impl ProcessMemory {
    /// 创建进程内存访问器
    pub fn new(pid: i32) -> Result<Self> {
        Self::with_mode(pid, ReadMode::ProcessVmReadv)
    }

    /// 创建指定读取模式的进程内存访问器
    pub fn with_mode(pid: i32, read_mode: ReadMode) -> Result<Self> {
        let mem_file = if read_mode == ReadMode::ProcMemIo {
            let path = format!("/proc/{}/mem", pid);
            Some(File::open(&path).map_err(ScanError::Io)?)
        } else {
            None
        };

        let mut pm = Self {
            pid,
            vm_areas: Vec::new(),
            scan_areas: Vec::new(),
            static_modules: Vec::new(),
            read_mode,
            mem_file,
        };
        pm.parse_maps()?;
        pm.parse_modules();
        Ok(pm)
    }

    /// 获取读取模式
    pub fn read_mode(&self) -> ReadMode {
        self.read_mode
    }

    /// 通过进程名获取 PID
    pub fn find_pid(name: &str) -> Result<i32> {
        let output = std::process::Command::new("pidof")
            .arg(name)
            .output()
            .map_err(ScanError::Io)?;

        let pid_str = String::from_utf8_lossy(&output.stdout);
        pid_str
            .trim()
            .split_whitespace()
            .next()
            .and_then(|s| s.parse().ok())
            .ok_or_else(|| ScanError::ProcessNotFound(name.to_string()))
    }

    /// 解析 /proc/pid/maps
    fn parse_maps(&mut self) -> Result<()> {
        let path = format!("/proc/{}/maps", self.pid);
        let file = File::open(&path).map_err(ScanError::Io)?;
        let reader = BufReader::new(file);

        self.vm_areas.clear();
        for line in reader.lines() {
            let line = line.map_err(ScanError::Io)?;
            if let Some(vma) = VmAreaData::parse(&line) {
                self.vm_areas.push(vma);
            }
        }

        Ok(())
    }

    /// 解析静态模块
    fn parse_modules(&mut self) {
        let static_ranges = MemRange::CData as i32 | MemRange::CodeApp as i32;
        let mut module_counts: HashMap<String, i32> = HashMap::new();

        self.static_modules.clear();

        let mut prev_range = MemRange::Other;
        let mut prev_name = String::new();

        for vma in &self.vm_areas {
            let range_val = vma.range as i32;

            if range_val & static_ranges != 0 {
                let name = vma.short_name().to_string();
                let count = module_counts.entry(name.clone()).or_insert(0);
                *count += 1;

                self.static_modules.push(VmStaticData::new(
                    vma.start,
                    vma.end,
                    vma.range,
                    name.clone(),
                    *count,
                ));

                prev_name = name;
            } else if vma.range == MemRange::CBss && (prev_range as i32 & static_ranges != 0) {
                let name = format!("{}:bss", prev_name);
                let count = module_counts.entry(name.clone()).or_insert(0);
                *count += 1;

                self.static_modules.push(VmStaticData::new(
                    vma.start,
                    vma.end,
                    vma.range,
                    name,
                    *count,
                ));
            }

            prev_range = vma.range;
        }
    }

    /// 设置扫描范围
    pub fn set_scan_ranges(&mut self, ranges: i32) {
        self.scan_areas.clear();
        for vma in &self.vm_areas {
            if (vma.range as i32) & ranges != 0 {
                self.scan_areas.push(vma.clone());
            }
        }
    }

    /// 读取内存
    pub fn read(&self, addr: u64, buf: &mut [u8]) -> Result<usize> {
        match self.read_mode {
            ReadMode::ProcMemIo => self.read_mem_io(addr, buf),
            ReadMode::ProcessVmReadv => self.read_vm_readv(addr, buf),
        }
    }

    /// 通过 process_vm_readv 读取内存
    fn read_vm_readv(&self, addr: u64, buf: &mut [u8]) -> Result<usize> {
        let local_iov = libc::iovec {
            iov_base: buf.as_mut_ptr() as *mut libc::c_void,
            iov_len: buf.len(),
        };
        let remote_iov = libc::iovec {
            iov_base: addr as *mut libc::c_void,
            iov_len: buf.len(),
        };

        let result = unsafe {
            libc::process_vm_readv(
                self.pid,
                &local_iov,
                1,
                &remote_iov,
                1,
                0,
            )
        };

        if result < 0 {
            Err(ScanError::ReadFailed(addr))
        } else {
            Ok(result as usize)
        }
    }

    /// 通过 /proc/[pid]/mem 文件IO读取内存
    /// 按页分段读取，单页失败时填零跳过，避免整块数据丢失
    fn read_mem_io(&self, addr: u64, buf: &mut [u8]) -> Result<usize> {
        let file = self.mem_file.as_ref()
            .ok_or_else(|| ScanError::ReadFailed(addr))?;
        let fd = file.as_raw_fd();
        let page_size = unsafe { libc::sysconf(libc::_SC_PAGESIZE) as usize };
        let mut total = 0usize;

        while total < buf.len() {
            let chunk = (buf.len() - total).min(page_size);
            let offset = addr as libc::off64_t + total as libc::off64_t;
            let n = unsafe {
                libc::pread64(
                    fd,
                    buf[total..].as_mut_ptr() as *mut libc::c_void,
                    chunk,
                    offset,
                )
            };
            if n <= 0 {
                // 该页不可读，填零跳过
                buf[total..total + chunk].fill(0);
                total += chunk;
                continue;
            }
            total += n as usize;
        }

        Ok(total)
    }

    /// 读取指针值
    pub fn read_pointer(&self, addr: u64) -> Result<u64> {
        let mut buf = [0u8; 8];
        self.read(addr, &mut buf)?;
        Ok(u64::from_le_bytes(buf))
    }

    /// 读取 32 位指针值
    pub fn read_pointer32(&self, addr: u64) -> Result<u32> {
        let mut buf = [0u8; 4];
        self.read(addr, &mut buf)?;
        Ok(u32::from_le_bytes(buf))
    }

    /// 获取 PID
    pub fn pid(&self) -> i32 {
        self.pid
    }

    /// 获取所有内存区域
    pub fn vm_areas(&self) -> &[VmAreaData] {
        &self.vm_areas
    }

    /// 获取扫描区域
    pub fn scan_areas(&self) -> &[VmAreaData] {
        &self.scan_areas
    }

    /// 获取静态模块
    pub fn static_modules(&self) -> &[VmStaticData] {
        &self.static_modules
    }

    /// 获取可变静态模块引用
    pub fn static_modules_mut(&mut self) -> &mut [VmStaticData] {
        &mut self.static_modules
    }
}

/// 线程安全的进程内存访问器
pub struct SharedProcessMemory {
    inner: Arc<ProcessMemory>,
}

impl SharedProcessMemory {
    pub fn new(pid: i32) -> Result<Self> {
        Ok(Self {
            inner: Arc::new(ProcessMemory::new(pid)?),
        })
    }

    pub fn read(&self, addr: u64, buf: &mut [u8]) -> Result<usize> {
        self.inner.read(addr, buf)
    }

    pub fn read_pointer(&self, addr: u64) -> Result<u64> {
        self.inner.read_pointer(addr)
    }
}

impl Clone for SharedProcessMemory {
    fn clone(&self) -> Self {
        Self {
            inner: Arc::clone(&self.inner),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_find_pid() {
        // 测试查找 init 进程（PID 1）
        // 注意：这个测试需要 root 权限
        if let Ok(pid) = ProcessMemory::find_pid("init") {
            assert!(pid > 0);
        }
    }
}
