//! 错误类型定义

use thiserror::Error;

#[derive(Error, Debug)]
pub enum ScanError {
    #[error("IO error: {0}")]
    Io(#[from] std::io::Error),

    #[error("Memory map failed: {0}")]
    MmapFailed(String),

    #[error("Process not found: {0}")]
    ProcessNotFound(String),

    #[error("Memory read failed at address 0x{0:x}")]
    ReadFailed(u64),

    #[error("Buffer pool exhausted")]
    BufferExhausted,

    #[error("Invalid pointer chain")]
    InvalidChain,

    #[error("Timeout: {0}")]
    Timeout(String),
}

pub type Result<T> = std::result::Result<T, ScanError>;
