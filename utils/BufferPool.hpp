#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <vector>

namespace memtool {

/**
 * @brief 缓冲区池管理器
 * 
 * 使用条件变量 + 队列实现类似信号量的功能
 * 关键优化：当缓冲区不可用时，任务会真正"休眠"，释放CPU和线程执行权
 * 这样其他任务可以继续执行并释放缓冲区
 */
class BufferPool {
private:
    std::vector<char*> buffers_;           // 所有缓冲区
    std::queue<char*> available_;          // 可用缓冲区队列
    std::mutex mutex_;                     // 保护队列的互斥锁
    std::condition_variable cv_;           // 条件变量
    size_t buffer_size_;                   // 每个缓冲区大小
    std::atomic<size_t> total_count_;      // 总缓冲区数
    std::atomic<size_t> available_count_;  // 可用缓冲区数

public:
    /**
     * @brief 构造函数
     * @param count 缓冲区数量
     * @param size 每个缓冲区大小
     */
    BufferPool(size_t count, size_t size) 
        : buffer_size_(size)
        , total_count_(count)
        , available_count_(count) 
    {
        // 预分配所有缓冲区
        buffers_.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            char* buf = new char[size];
            buffers_.push_back(buf);
            available_.push(buf);
        }
    }

    /**
     * @brief 析构函数 - 释放所有缓冲区
     */
    ~BufferPool() {
        for (auto buf : buffers_) {
            delete[] buf;
        }
    }

    // 禁止拷贝和移动
    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;
    BufferPool(BufferPool&&) = delete;
    BufferPool& operator=(BufferPool&&) = delete;

    /**
     * @brief 获取一个缓冲区（阻塞直到可用）
     * @return 缓冲区指针
     * 
     * 关键：使用条件变量阻塞，线程会真正休眠，不占用CPU
     * 当缓冲区被释放时，条件变量会唤醒一个等待线程
     */
    char* acquire() {
        std::unique_lock<std::mutex> lock(mutex_);
        
        // 等待直到有可用缓冲区
        cv_.wait(lock, [this] { return !available_.empty(); });
        
        // 获取缓冲区
        char* buf = available_.front();
        available_.pop();
        available_count_.fetch_sub(1, std::memory_order_relaxed);
        
        return buf;
    }

    /**
     * @brief 尝试获取缓冲区（非阻塞）
     * @param buf 输出参数，获取到的缓冲区指针
     * @return 是否成功获取
     */
    bool try_acquire(char*& buf) {
        std::unique_lock<std::mutex> lock(mutex_);
        
        if (available_.empty()) {
            return false;
        }
        
        buf = available_.front();
        available_.pop();
        available_count_.fetch_sub(1, std::memory_order_relaxed);
        
        return true;
    }

    /**
     * @brief 尝试获取缓冲区（带超时）
     * @param buf 输出参数，获取到的缓冲区指针
     * @param timeout_ms 超时时间（毫秒）
     * @return 是否成功获取
     */
    bool try_acquire_for(char*& buf, int timeout_ms) {
        std::unique_lock<std::mutex> lock(mutex_);
        
        // 等待直到有可用缓冲区或超时
        if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                          [this] { return !available_.empty(); })) {
            return false;  // 超时
        }
        
        buf = available_.front();
        available_.pop();
        available_count_.fetch_sub(1, std::memory_order_relaxed);
        
        return true;
    }

    /**
     * @brief 释放缓冲区
     * @param buf 要释放的缓冲区指针
     */
    void release(char* buf) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            available_.push(buf);
            available_count_.fetch_add(1, std::memory_order_relaxed);
        }
        
        // 唤醒一个等待的线程
        cv_.notify_one();
    }

    /**
     * @brief 获取缓冲区大小
     */
    size_t buffer_size() const {
        return buffer_size_;
    }

    /**
     * @brief 获取总缓冲区数量
     */
    size_t total_count() const {
        return total_count_.load(std::memory_order_relaxed);
    }

    /**
     * @brief 获取当前可用缓冲区数量
     */
    size_t available_count() const {
        return available_count_.load(std::memory_order_relaxed);
    }
};

/**
 * @brief RAII 缓冲区守卫
 * 
 * 确保缓冲区在任何情况下（包括异常）都会被正确释放
 * 这是解决死锁的关键！
 */
class BufferGuard {
private:
    BufferPool* pool_;
    char* buffer_;
    bool released_;
    
public:
    BufferGuard(BufferPool& pool) 
        : pool_(&pool)
        , buffer_(pool_->acquire())  // 获取缓冲区
        , released_(false)
    {}
    
    ~BufferGuard() {
        release();  // 析构时自动释放
    }
    
    /**
     * @brief 手动释放缓冲区（可选）
     */
    void release() {
        if (!released_ && buffer_) {
            pool_->release(buffer_);
            released_ = true;
            buffer_ = nullptr;
        }
    }
    
    /**
     * @brief 获取缓冲区指针
     */
    char* get() const { return buffer_; }
    
    /**
     * @brief 重置为新缓冲区（高级用法）
     */
    void reset() {
        release();
        if (pool_) {
            buffer_ = pool_->acquire();
            released_ = false;
        }
    }
    
    // 禁止拷贝和移动
    BufferGuard(const BufferGuard&) = delete;
    BufferGuard& operator=(const BufferGuard&) = delete;
    BufferGuard(BufferGuard&&) = delete;
    BufferGuard& operator=(BufferGuard&&) = delete;
};

} // namespace memtool

