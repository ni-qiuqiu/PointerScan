#pragma once

#include "threadpool.h"

namespace utils
{

// submit 实现 - 推荐使用
template <class F, class... Args>
auto threadpool::submit(F &&f, Args &&...args) -> std::future<decltype(f(args...))>
{
    using return_type = decltype(f(args...));

    // 检查线程池是否已停止
    if (stop.load(std::memory_order_acquire)) {
        throw std::runtime_error("线程池已停止，无法提交新任务");
    }

    // 创建打包任务
    auto task = std::make_shared<std::packaged_task<return_type()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );

    std::future<return_type> result = task->get_future();

    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        
        // 再次检查，防止在获取锁期间线程池被停止
        if (stop.load(std::memory_order_acquire)) {
            throw std::runtime_error("线程池已停止，无法提交新任务");
        }

        // 将任务包装为 void() 函数并加入队列
        tasks.emplace([task]() { 
            try {
                (*task)(); 
            } catch (...) {
                // packaged_task 会自动捕获异常并存储到 future 中
            }
        });
    }

    // 唤醒一个等待的线程
    condition.notify_one();
    
    return result;
}

// pushpool 实现 - 兼容旧接口
template <class F, class... Args>
auto threadpool::pushpool(F &&f, Args &&...args) -> std::future<decltype(f(args...))>
{
    return submit(std::forward<F>(f), std::forward<Args>(args)...);
}

} // namespace utils