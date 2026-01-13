//! MapQueue - 基于 tmpfile + mmap 的动态数组
//! 
//! 核心机制：
//! - 使用临时文件作为后备存储
//! - mmap 映射到虚拟地址空间
//! - 内存压力时 OS 自动换出到文件
//! - 避免 BFS 扫描时的内存爆炸

use std::marker::PhantomData;
use std::ops::{Index, IndexMut};
use std::ptr::NonNull;
use std::slice;

use memmap2::MmapMut;
use tempfile::tempfile;

use crate::error::{Result, ScanError};

/// 基于 mmap 的动态数组，用于存储大量 BFS 中间数据
pub struct MapQueue<T: Copy> {
    /// mmap 映射区域
    mmap: Option<MmapMut>,
    /// 数据指针
    data: Option<NonNull<T>>,
    /// 当前元素数量
    len: usize,
    /// 容量（元素数量）
    capacity: usize,
    /// 类型标记
    _marker: PhantomData<T>,
}

// Safety: MapQueue 内部数据通过 mmap 管理，可以安全地跨线程发送
unsafe impl<T: Copy + Send> Send for MapQueue<T> {}
unsafe impl<T: Copy + Sync> Sync for MapQueue<T> {}

impl<T: Copy> MapQueue<T> {
    /// 创建空的 MapQueue
    pub fn new() -> Self {
        Self {
            mmap: None,
            data: None,
            len: 0,
            capacity: 0,
            _marker: PhantomData,
        }
    }

    /// 创建指定容量的 MapQueue
    pub fn with_capacity(capacity: usize) -> Result<Self> {
        let mut queue = Self::new();
        queue.reserve(capacity)?;
        Ok(queue)
    }

    /// 当前元素数量
    #[inline]
    pub fn len(&self) -> usize {
        self.len
    }

    /// 是否为空
    #[inline]
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// 容量
    #[inline]
    pub fn capacity(&self) -> usize {
        self.capacity
    }

    /// 清空数据（不释放内存）
    pub fn clear(&mut self) {
        self.len = 0;
    }

    /// 预留容量
    pub fn reserve(&mut self, new_capacity: usize) -> Result<()> {
        if new_capacity <= self.capacity {
            return Ok(());
        }

        let new_size = new_capacity * std::mem::size_of::<T>();
        
        // 创建临时文件并设置大小
        let file = tempfile().map_err(ScanError::Io)?;
        file.set_len(new_size as u64).map_err(ScanError::Io)?;

        // 创建 mmap 映射
        let mut new_mmap = unsafe {
            MmapMut::map_mut(&file)
                .map_err(|e| ScanError::MmapFailed(e.to_string()))?
        };

        // 复制旧数据
        if let Some(old_data) = self.data {
            let old_bytes = self.len * std::mem::size_of::<T>();
            unsafe {
                std::ptr::copy_nonoverlapping(
                    old_data.as_ptr() as *const u8,
                    new_mmap.as_mut_ptr(),
                    old_bytes,
                );
            }
        }

        // 获取新数据指针
        let new_data = NonNull::new(new_mmap.as_mut_ptr() as *mut T)
            .ok_or_else(|| ScanError::MmapFailed("null pointer".into()))?;

        self.mmap = Some(new_mmap);
        self.data = Some(new_data);
        self.capacity = new_capacity;

        Ok(())
    }

    /// 计算增长后的容量
    fn grow_capacity(&self, min_capacity: usize) -> usize {
        let new_capacity = if self.capacity == 0 {
            8
        } else {
            self.capacity + self.capacity / 2
        };
        new_capacity.max(min_capacity)
    }

    /// 添加元素
    pub fn push(&mut self, value: T) -> Result<()> {
        if self.len == self.capacity {
            self.reserve(self.grow_capacity(self.len + 1))?;
        }

        unsafe {
            let ptr = self.data.unwrap().as_ptr().add(self.len);
            std::ptr::write(ptr, value);
        }
        self.len += 1;

        Ok(())
    }

    /// 弹出最后一个元素
    pub fn pop(&mut self) -> Option<T> {
        if self.len == 0 {
            return None;
        }
        self.len -= 1;
        unsafe {
            let ptr = self.data.unwrap().as_ptr().add(self.len);
            Some(std::ptr::read(ptr))
        }
    }

    /// 调整大小
    pub fn resize(&mut self, new_len: usize, value: T) -> Result<()> {
        if new_len > self.capacity {
            self.reserve(self.grow_capacity(new_len))?;
        }

        if new_len > self.len {
            unsafe {
                let ptr = self.data.unwrap().as_ptr();
                for i in self.len..new_len {
                    std::ptr::write(ptr.add(i), value);
                }
            }
        }
        self.len = new_len;

        Ok(())
    }

    /// 获取切片
    pub fn as_slice(&self) -> &[T] {
        if self.len == 0 {
            return &[];
        }
        unsafe {
            slice::from_raw_parts(self.data.unwrap().as_ptr(), self.len)
        }
    }

    /// 获取可变切片
    pub fn as_mut_slice(&mut self) -> &mut [T] {
        if self.len == 0 {
            return &mut [];
        }
        unsafe {
            slice::from_raw_parts_mut(self.data.unwrap().as_ptr(), self.len)
        }
    }

    /// 迭代器
    pub fn iter(&self) -> impl Iterator<Item = &T> {
        self.as_slice().iter()
    }

    /// 可变迭代器
    pub fn iter_mut(&mut self) -> impl Iterator<Item = &mut T> {
        self.as_mut_slice().iter_mut()
    }

    /// 获取第一个元素
    pub fn first(&self) -> Option<&T> {
        self.as_slice().first()
    }

    /// 获取最后一个元素
    pub fn last(&self) -> Option<&T> {
        self.as_slice().last()
    }

    /// 字节大小
    pub fn size_in_bytes(&self) -> usize {
        self.len * std::mem::size_of::<T>()
    }
}

impl<T: Copy> Default for MapQueue<T> {
    fn default() -> Self {
        Self::new()
    }
}

impl<T: Copy> Index<usize> for MapQueue<T> {
    type Output = T;

    fn index(&self, index: usize) -> &Self::Output {
        assert!(index < self.len, "index out of bounds");
        unsafe { &*self.data.unwrap().as_ptr().add(index) }
    }
}

impl<T: Copy> IndexMut<usize> for MapQueue<T> {
    fn index_mut(&mut self, index: usize) -> &mut Self::Output {
        assert!(index < self.len, "index out of bounds");
        unsafe { &mut *self.data.unwrap().as_ptr().add(index) }
    }
}

impl<T: Copy> Drop for MapQueue<T> {
    fn drop(&mut self) {
        // mmap 会在 MmapMut drop 时自动 munmap
        // tempfile 会自动删除
    }
}

impl<T: Copy> Clone for MapQueue<T> {
    fn clone(&self) -> Self {
        let mut new_queue = Self::new();
        if self.len > 0 {
            new_queue.reserve(self.len).expect("clone reserve failed");
            unsafe {
                std::ptr::copy_nonoverlapping(
                    self.data.unwrap().as_ptr(),
                    new_queue.data.unwrap().as_ptr(),
                    self.len,
                );
            }
            new_queue.len = self.len;
        }
        new_queue
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_basic_operations() {
        let mut queue: MapQueue<u64> = MapQueue::new();
        
        queue.push(1).unwrap();
        queue.push(2).unwrap();
        queue.push(3).unwrap();

        assert_eq!(queue.len(), 3);
        assert_eq!(queue[0], 1);
        assert_eq!(queue[1], 2);
        assert_eq!(queue[2], 3);

        assert_eq!(queue.pop(), Some(3));
        assert_eq!(queue.len(), 2);
    }

    #[test]
    fn test_large_data() {
        let mut queue: MapQueue<u64> = MapQueue::with_capacity(1_000_000).unwrap();
        
        for i in 0..1_000_000u64 {
            queue.push(i).unwrap();
        }

        assert_eq!(queue.len(), 1_000_000);
        assert_eq!(queue[999_999], 999_999);
    }
}
