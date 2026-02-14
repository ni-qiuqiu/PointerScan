//! BFS 指针链扫描器 - Rust ARM64 实现
//! 
//! 核心特性：
//! - tmpfile + mmap 避免 BFS 内存爆炸
//! - 零拷贝内存管理
//! - 多线程并行扫描

pub mod error;
pub mod memory;
pub mod mapqueue;
pub mod buffer_pool;
pub mod pointer;
pub mod scanner;
pub mod process;
pub mod compare;

#[cfg(test)]
mod tests;

pub use error::{ScanError, Result};
pub use mapqueue::MapQueue;
pub use buffer_pool::BufferPool;
pub use pointer::{PointerData, PointerDir, PointerRange};
pub use scanner::ChainScanner;
pub use process::ProcessMemory;
