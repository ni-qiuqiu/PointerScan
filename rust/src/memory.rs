//! 内存区域数据结构

use std::fmt;

/// 内存范围类型
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
pub enum MemRange {
    Anonymous = 1,
    CHeap = 2,
    CAlloc = 4,
    CodeApp = 8,
    CodeSystem = 16,
    CBss = 32,
    CData = 64,
    Other = 128,
}

impl MemRange {
    /// 从名称和权限判断内存范围类型
    /// 注意：检测顺序很重要，与原项目 det_mem_range 保持一致
    pub fn detect(name: &str, perms: &str) -> Self {
        if name.is_empty() {
            return Self::Anonymous;
        }

        if name == "[heap]" {
            return Self::CHeap;
        }

        if name.starts_with("[anon:libc_malloc") || name.starts_with("[anon:scudo:") {
            return Self::CAlloc;
        }

        // 原项目顺序：先检测 Code_app（需要 xp 权限）
        if name.contains("/data/app/") && perms.contains('x') && name.contains(".so") {
            return Self::CodeApp;
        }

        if name.contains("/system/framework/") {
            return Self::CodeSystem;
        }

        if name.contains("[anon:.bss]") {
            return Self::CBss;
        }

        // 原项目：C_data 是 /data/app/ + .so 但没有 x 权限的区域
        if name.contains("/data/app/") && name.contains(".so") {
            return Self::CData;
        }

        Self::Other
    }

    /// 是否为静态区域（用于指针链扫描）
    pub fn is_static(&self) -> bool {
        matches!(self, Self::CData | Self::CodeApp)
    }
}

/// 内存保护标志
#[derive(Debug, Clone, Copy, Default)]
pub struct MemProt {
    pub read: bool,
    pub write: bool,
    pub exec: bool,
}

impl MemProt {
    pub fn from_perms(perms: &str) -> Self {
        Self {
            read: perms.contains('r'),
            write: perms.contains('w'),
            exec: perms.contains('x'),
        }
    }

    pub fn can_read(&self) -> bool {
        self.read
    }
}

/// 内存区域数据
#[derive(Clone)]
pub struct VmAreaData {
    /// 起始地址
    pub start: u64,
    /// 结束地址
    pub end: u64,
    /// 权限字符串
    pub perms: String,
    /// 偏移
    pub offset: u64,
    /// 设备
    pub dev: String,
    /// inode
    pub inode: u64,
    /// 名称/路径
    pub name: String,
    /// 内存范围类型
    pub range: MemRange,
    /// 保护标志
    pub prot: MemProt,
    /// 模块计数（用于指针链扫描）
    pub count: i32,
}

impl VmAreaData {
    pub fn new() -> Self {
        Self {
            start: 0,
            end: 0,
            perms: String::new(),
            offset: 0,
            dev: String::new(),
            inode: 0,
            name: String::new(),
            range: MemRange::Other,
            prot: MemProt::default(),
            count: 0,
        }
    }

    /// 从 VmStaticData 创建
    pub fn from_static(vma: &VmStaticData) -> Self {
        Self {
            start: vma.start,
            end: vma.end,
            perms: String::new(),
            offset: 0,
            dev: String::new(),
            inode: 0,
            name: vma.name.clone(),
            range: vma.range,
            prot: MemProt::default(),
            count: vma.count,
        }
    }

    /// 从 /proc/pid/maps 行解析
    pub fn parse(line: &str) -> Option<Self> {
        let parts: Vec<&str> = line.split_whitespace().collect();
        if parts.len() < 5 {
            return None;
        }

        // 解析地址范围
        let addr_parts: Vec<&str> = parts[0].split('-').collect();
        if addr_parts.len() != 2 {
            return None;
        }

        let start = u64::from_str_radix(addr_parts[0], 16).ok()?;
        let end = u64::from_str_radix(addr_parts[1], 16).ok()?;
        let perms = parts[1].to_string();
        let offset = u64::from_str_radix(parts[2], 16).unwrap_or(0);
        let dev = parts[3].to_string();
        let inode = parts[4].parse().unwrap_or(0);
        let name = if parts.len() > 5 { parts[5].to_string() } else { String::new() };

        let range = MemRange::detect(&name, &perms);
        let prot = MemProt::from_perms(&perms);

        Some(Self {
            start,
            end,
            perms,
            offset,
            dev,
            inode,
            name,
            range,
            prot,
            count: 0,
        })
    }

    /// 区域大小
    pub fn size(&self) -> u64 {
        self.end - self.start
    }

    /// 是否可读
    pub fn is_readable(&self) -> bool {
        self.prot.can_read()
    }

    /// 获取短名称（去除路径）
    pub fn short_name(&self) -> &str {
        self.name
            .rfind('/')
            .map(|pos| &self.name[pos + 1..])
            .unwrap_or(&self.name)
    }
}

impl Default for VmAreaData {
    fn default() -> Self {
        Self::new()
    }
}

impl fmt::Debug for VmAreaData {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "VmArea {{ 0x{:x}-0x{:x} {} {:?} {} }}",
            self.start, self.end, self.perms, self.range, self.name
        )
    }
}

/// 静态模块数据（用于指针链扫描）
#[derive(Clone)]
pub struct VmStaticData {
    /// 起始地址
    pub start: u64,
    /// 结束地址
    pub end: u64,
    /// 内存范围类型
    pub range: MemRange,
    /// 模块名
    pub name: String,
    /// 模块计数（同名模块的序号）
    pub count: i32,
    /// 是否过滤
    pub filter: bool,
}

impl VmStaticData {
    pub fn new(start: u64, end: u64, range: MemRange, name: String, count: i32) -> Self {
        Self {
            start,
            end,
            range,
            name,
            count,
            filter: false,
        }
    }

    pub fn from_vma(vma: &VmAreaData, count: i32) -> Self {
        Self::new(vma.start, vma.end, vma.range, vma.short_name().to_string(), count)
    }
}

impl fmt::Debug for VmStaticData {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "VmStatic {{ 0x{:x}-0x{:x} {}[{}] }}",
            self.start, self.end, self.name, self.count
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_parse_maps_line() {
        let line = "7f8a4c000000-7f8a4c021000 rw-p 00000000 00:00 0 [heap]";
        let vma = VmAreaData::parse(line).unwrap();
        
        assert_eq!(vma.start, 0x7f8a4c000000);
        assert_eq!(vma.end, 0x7f8a4c021000);
        assert_eq!(vma.perms, "rw-p");
        assert_eq!(vma.name, "[heap]");
        assert_eq!(vma.range, MemRange::CHeap);
        assert!(vma.prot.read);
        assert!(vma.prot.write);
        assert!(!vma.prot.exec);
    }

    #[test]
    fn test_detect_mem_range() {
        assert_eq!(MemRange::detect("", "rw-p"), MemRange::Anonymous);
        assert_eq!(MemRange::detect("[heap]", "rw-p"), MemRange::CHeap);
        assert_eq!(MemRange::detect("/data/app/com.test/lib/libtest.so", "r-xp"), MemRange::CodeApp);
        assert_eq!(MemRange::detect("/data/app/com.test/lib/libtest.so", "rw-p"), MemRange::CData);
    }
}
