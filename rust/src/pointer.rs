//! 指针数据结构定义

use crate::mapqueue::MapQueue;
use crate::memory::VmAreaData;

/// 指针数据：存储地址和值
#[derive(Debug, Clone, Copy, Default)]
#[repr(C)]
pub struct PointerData {
    /// 指针地址
    pub address: u64,
    /// 指针指向的值
    pub value: u64,
}

impl PointerData {
    pub fn new(address: u64, value: u64) -> Self {
        Self { address, value }
    }
}

/// 指针目录：存储指针信息和索引范围
/// 
/// 用于 BFS 扫描中建立层级间的关联关系
#[derive(Debug, Clone, Copy, Default)]
#[repr(C)]
pub struct PointerDir {
    /// 指针地址
    pub address: u64,
    /// 指针指向的值
    pub value: u64,
    /// 索引起始（指向下一层的范围）
    pub start: u32,
    /// 索引结束 [start, end)
    pub end: u32,
}

impl PointerDir {
    pub fn new(address: u64, value: u64) -> Self {
        Self {
            address,
            value,
            start: 0,
            end: 1,
        }
    }

    pub fn with_range(address: u64, value: u64, start: u32, end: u32) -> Self {
        Self { address, value, start, end }
    }

    /// 从 PointerData 转换
    pub fn from_data(data: &PointerData) -> Self {
        Self::new(data.address, data.value)
    }
}

/// 指针范围：关联静态模块和扫描结果
pub struct PointerRange {
    /// BFS 层级
    pub level: i32,
    /// 关联的内存区域
    pub vma: VmAreaData,
    /// 扫描结果（使用 MapQueue 避免内存爆炸）
    pub results: MapQueue<PointerDir>,
}

impl PointerRange {
    pub fn new(level: i32, vma: VmAreaData, results: MapQueue<PointerDir>) -> Self {
        Self { level, vma, results }
    }
}

/// 指针链信息：BFS 扫描的最终结果
pub struct ChainInfo {
    /// 每层的累计计数
    pub counts: Vec<MapQueue<usize>>,
    /// 每层的指针目录内容
    pub contents: Vec<MapQueue<*const PointerDir>>,
}

impl ChainInfo {
    pub fn new(counts: Vec<MapQueue<usize>>, contents: Vec<MapQueue<*const PointerDir>>) -> Self {
        Self { counts, contents }
    }

    pub fn is_empty(&self) -> bool {
        self.counts.is_empty() || self.contents.is_empty()
    }
}

// Safety: PointerDir 内部只有基本类型
unsafe impl Send for ChainInfo {}
unsafe impl Sync for ChainInfo {}

/// 二进制文件头
#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct ChainHeader {
    /// 签名
    pub sign: [u8; 128],
    /// 模块数量
    pub module_count: i32,
    /// 版本号
    pub version: i32,
    /// 指针大小（4 或 8）
    pub size: i32,
    /// 层级数 [0, level)
    pub level: i32,
}

impl Default for ChainHeader {
    fn default() -> Self {
        let mut sign = [0u8; 128];
        let sig = b".bin from chainer-rust\n";
        sign[..sig.len()].copy_from_slice(sig);
        
        Self {
            sign,
            module_count: 0,
            version: 101,
            size: 8,
            level: 0,
        }
    }
}

/// 模块符号信息
#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct ChainSymbol {
    /// 起始地址
    pub start: u64,
    /// 模块名
    pub name: [u8; 64],
    /// 内存范围类型
    pub range: i32,
    /// 模块计数
    pub count: i32,
    /// 指针数量
    pub pointer_count: i32,
    /// 层级
    pub level: i32,
}

impl Default for ChainSymbol {
    fn default() -> Self {
        Self {
            start: 0,
            name: [0u8; 64],
            range: 0,
            count: 0,
            pointer_count: 0,
            level: 0,
        }
    }
}

impl ChainSymbol {
    pub fn set_name(&mut self, name: &str) {
        let bytes = name.as_bytes();
        let len = bytes.len().min(63);
        self.name[..len].copy_from_slice(&bytes[..len]);
        self.name[len] = 0;
    }

    pub fn get_name(&self) -> &str {
        let end = self.name.iter().position(|&b| b == 0).unwrap_or(64);
        std::str::from_utf8(&self.name[..end]).unwrap_or("")
    }
}

/// 层级长度信息
#[derive(Debug, Clone, Copy, Default)]
#[repr(C)]
pub struct ChainLevelLen {
    /// 模块数量
    pub module_count: i32,
    /// 元素数量
    pub count: u32,
    /// 层级
    pub level: i32,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_pointer_dir_size() {
        // 确保结构体大小符合预期（用于二进制兼容）
        assert_eq!(std::mem::size_of::<PointerDir>(), 24);
        assert_eq!(std::mem::size_of::<PointerData>(), 16);
    }

    #[test]
    fn test_chain_symbol_name() {
        let mut sym = ChainSymbol::default();
        sym.set_name("libtest.so");
        assert_eq!(sym.get_name(), "libtest.so");
    }
}
