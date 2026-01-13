//! 集成测试

#[cfg(test)]
mod tests {
    use crate::mapqueue::MapQueue;
    use crate::pointer::{PointerData, PointerDir};
    use crate::buffer_pool::{BufferPool, SharedBufferPool};

    #[test]
    fn test_mapqueue_basic() {
        let mut queue: MapQueue<u64> = MapQueue::new();
        
        // 测试 push
        for i in 0..100 {
            queue.push(i).unwrap();
        }
        assert_eq!(queue.len(), 100);
        
        // 测试索引访问
        assert_eq!(queue[50], 50);
        
        // 测试迭代
        let sum: u64 = queue.iter().sum();
        assert_eq!(sum, (0..100).sum());
    }

    #[test]
    fn test_mapqueue_large() {
        // 测试大数据量（验证 mmap 机制）
        let mut queue: MapQueue<PointerDir> = MapQueue::new();
        
        for i in 0..100_000 {
            queue.push(PointerDir::new(i * 8, i * 16)).unwrap();
        }
        
        assert_eq!(queue.len(), 100_000);
        assert_eq!(queue[50_000].address, 400_000);
        assert_eq!(queue[50_000].value, 800_000);
    }

    #[test]
    fn test_buffer_pool() {
        let pool = BufferPool::new(4, 1024);
        
        // 获取所有缓冲区
        let b1 = pool.acquire().unwrap();
        let b2 = pool.acquire().unwrap();
        let b3 = pool.acquire().unwrap();
        let b4 = pool.acquire().unwrap();
        
        // 池应该为空
        assert!(pool.try_acquire().is_none());
        
        // 释放一个
        pool.release(b1);
        
        // 现在可以获取
        let b5 = pool.try_acquire();
        assert!(b5.is_some());
        
        // 清理
        pool.release(b2);
        pool.release(b3);
        pool.release(b4);
        if let Some(b) = b5 {
            pool.release(b);
        }
    }

    #[test]
    fn test_pointer_structures() {
        let data = PointerData::new(0x1000, 0x2000);
        assert_eq!(data.address, 0x1000);
        assert_eq!(data.value, 0x2000);
        
        let dir = PointerDir::from_data(&data);
        assert_eq!(dir.address, 0x1000);
        assert_eq!(dir.value, 0x2000);
        assert_eq!(dir.start, 0);
        assert_eq!(dir.end, 1);
    }
}
