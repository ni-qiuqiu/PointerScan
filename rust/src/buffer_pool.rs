//! BufferPool - 缓冲区池管理器
//! 
//! 用于高效管理扫描过程中的内存缓冲区：
//! - 预分配固定数量的缓冲区
//! - 使用条件变量实现阻塞等待
//! - 避免频繁的内存分配和释放

use std::sync::Arc;
use std::time::Duration;

use crossbeam_channel::{bounded, Receiver, Sender};

use crate::error::{Result, ScanError};

/// 缓冲区池
pub struct BufferPool {
    /// 缓冲区发送端
    sender: Sender<Vec<u8>>,
    /// 缓冲区接收端
    receiver: Receiver<Vec<u8>>,
    /// 每个缓冲区大小
    buffer_size: usize,
    /// 总缓冲区数量
    total_count: usize,
}

impl BufferPool {
    /// 创建缓冲区池
    /// 
    /// # Arguments
    /// * `count` - 缓冲区数量
    /// * `size` - 每个缓冲区大小
    pub fn new(count: usize, size: usize) -> Self {
        let (sender, receiver) = bounded(count);

        // 预分配所有缓冲区
        for _ in 0..count {
            let buffer = vec![0u8; size];
            sender.send(buffer).expect("channel should not be full");
        }

        Self {
            sender,
            receiver,
            buffer_size: size,
            total_count: count,
        }
    }

    /// 获取缓冲区（阻塞等待）
    pub fn acquire(&self) -> Result<Vec<u8>> {
        self.receiver
            .recv()
            .map_err(|_| ScanError::BufferExhausted)
    }

    /// 获取缓冲区（带超时）
    pub fn acquire_timeout(&self, timeout: Duration) -> Result<Vec<u8>> {
        self.receiver
            .recv_timeout(timeout)
            .map_err(|_| ScanError::Timeout("buffer acquire timeout".into()))
    }

    /// 尝试获取缓冲区（非阻塞）
    pub fn try_acquire(&self) -> Option<Vec<u8>> {
        self.receiver.try_recv().ok()
    }

    /// 释放缓冲区
    pub fn release(&self, buffer: Vec<u8>) {
        // 忽略发送失败（池已满或已关闭）
        let _ = self.sender.try_send(buffer);
    }

    /// 缓冲区大小
    pub fn buffer_size(&self) -> usize {
        self.buffer_size
    }

    /// 总缓冲区数量
    pub fn total_count(&self) -> usize {
        self.total_count
    }

    /// 当前可用缓冲区数量
    pub fn available_count(&self) -> usize {
        self.receiver.len()
    }
}

/// RAII 缓冲区守卫
/// 
/// 确保缓冲区在任何情况下都会被正确释放
pub struct BufferGuard {
    buffer: Option<Vec<u8>>,
    pool: Arc<BufferPool>,
}

impl BufferGuard {
    /// 从池中获取缓冲区
    pub fn new(pool: Arc<BufferPool>) -> Result<Self> {
        let buffer = pool.acquire()?;
        Ok(Self {
            buffer: Some(buffer),
            pool,
        })
    }

    /// 从池中获取缓冲区（带超时）
    pub fn with_timeout(pool: Arc<BufferPool>, timeout: Duration) -> Result<Self> {
        let buffer = pool.acquire_timeout(timeout)?;
        Ok(Self {
            buffer: Some(buffer),
            pool,
        })
    }

    /// 获取缓冲区引用
    pub fn as_slice(&self) -> &[u8] {
        self.buffer.as_ref().map(|b| b.as_slice()).unwrap_or(&[])
    }

    /// 获取可变缓冲区引用
    pub fn as_mut_slice(&mut self) -> &mut [u8] {
        self.buffer.as_mut().map(|b| b.as_mut_slice()).unwrap_or(&mut [])
    }

    /// 获取缓冲区指针
    pub fn as_ptr(&self) -> *const u8 {
        self.buffer.as_ref().map(|b| b.as_ptr()).unwrap_or(std::ptr::null())
    }

    /// 获取可变缓冲区指针
    pub fn as_mut_ptr(&mut self) -> *mut u8 {
        self.buffer.as_mut().map(|b| b.as_mut_ptr()).unwrap_or(std::ptr::null_mut())
    }
}

impl Drop for BufferGuard {
    fn drop(&mut self) {
        if let Some(buffer) = self.buffer.take() {
            self.pool.release(buffer);
        }
    }
}

/// 线程安全的缓冲区池包装
pub struct SharedBufferPool {
    inner: Arc<BufferPool>,
}

impl SharedBufferPool {
    pub fn new(count: usize, size: usize) -> Self {
        Self {
            inner: Arc::new(BufferPool::new(count, size)),
        }
    }

    pub fn acquire_guard(&self) -> Result<BufferGuard> {
        BufferGuard::new(Arc::clone(&self.inner))
    }

    pub fn acquire_guard_timeout(&self, timeout: Duration) -> Result<BufferGuard> {
        BufferGuard::with_timeout(Arc::clone(&self.inner), timeout)
    }

    pub fn pool(&self) -> &BufferPool {
        &self.inner
    }
}

impl Clone for SharedBufferPool {
    fn clone(&self) -> Self {
        Self {
            inner: Arc::clone(&self.inner),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::thread;

    #[test]
    fn test_buffer_pool() {
        let pool = BufferPool::new(4, 1024);
        
        assert_eq!(pool.total_count(), 4);
        assert_eq!(pool.available_count(), 4);

        let buf1 = pool.acquire().unwrap();
        assert_eq!(pool.available_count(), 3);

        let buf2 = pool.acquire().unwrap();
        assert_eq!(pool.available_count(), 2);

        pool.release(buf1);
        assert_eq!(pool.available_count(), 3);

        pool.release(buf2);
        assert_eq!(pool.available_count(), 4);
    }

    #[test]
    fn test_buffer_guard() {
        let pool = SharedBufferPool::new(2, 512);

        {
            let _guard1 = pool.acquire_guard().unwrap();
            let _guard2 = pool.acquire_guard().unwrap();
            assert_eq!(pool.pool().available_count(), 0);
        }

        // guards dropped, buffers returned
        assert_eq!(pool.pool().available_count(), 2);
    }

    #[test]
    fn test_concurrent_access() {
        let pool = SharedBufferPool::new(8, 256);
        let mut handles = vec![];

        for _ in 0..16 {
            let pool_clone = pool.clone();
            handles.push(thread::spawn(move || {
                for _ in 0..100 {
                    let guard = pool_clone.acquire_guard().unwrap();
                    // 模拟工作
                    thread::sleep(Duration::from_micros(10));
                    drop(guard);
                }
            }));
        }

        for handle in handles {
            handle.join().unwrap();
        }

        assert_eq!(pool.pool().available_count(), 8);
    }
}
