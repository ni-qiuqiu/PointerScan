#ifndef UTILS_THREAD_POOL_DEFINE
#define UTILS_THREAD_POOL_DEFINE

#include "threadpool.h"

namespace utils
{

// 工作线程函数
void threadpool::work_thread()
{
    while (true) {
        std::function<void()> task;

        {
            std::unique_lock<std::mutex> lock(queue_mutex);

            // 等待任务或停止信号
            condition.wait(lock, [this] { 
                return stop.load(std::memory_order_acquire) || !tasks.empty(); 
            });

            // 如果停止且任务队列为空，则退出
            if (stop.load(std::memory_order_acquire) && tasks.empty()) {
                return;
            }

            // 如果有任务，取出任务
            if (!tasks.empty()) {
                task = std::move(tasks.front());
                tasks.pop();
            }
        }

        // 执行任务（在锁外执行以减少锁持有时间）
        if (task) {
            active_tasks.fetch_add(1, std::memory_order_relaxed);
            
            try {
                task();
            } catch (...) {
                // 捕获并忽略任务执行中的异常
                // 实际应用中可以添加日志记录
            }
            
            active_tasks.fetch_sub(1, std::memory_order_relaxed);
            
            // 通知可能在等待的 wait() 函数
            wait_condition.notify_all();
        }
    }
}

// 停止所有线程
void threadpool::kill_thread()
{
    if (workers.empty()) {
        return;
    }

    // 设置停止标志
    stop.store(true, std::memory_order_release);

    // 唤醒所有等待的线程
    condition.notify_all();

    // 等待所有线程结束
    for (auto &worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

// 调整线程池大小（新接口）
void threadpool::resize(size_t count, bool wait_finish)
{
    if (count == 0) {
        throw std::invalid_argument("线程数量必须大于 0");
    }

    if (count == thread_count) {
        return; // 大小未改变
    }

    // 如果需要等待当前任务完成
    if (wait_finish) {
        wait();
    }

    // 停止当前所有线程
    kill_thread();
    workers.clear();

    // 重置停止标志
    stop.store(false, std::memory_order_release);
    thread_count = count;

    // 创建新线程
    workers.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        workers.emplace_back([this] { work_thread(); });
    }
}

// 兼容旧接口
void threadpool::change_thread(size_t count)
{
    resize(count, true);
}

// 等待所有任务完成
void threadpool::wait()
{
    std::unique_lock<std::mutex> lock(queue_mutex);
    
    // 等待任务队列为空且没有活跃任务
    wait_condition.wait(lock, [this] {
        return tasks.empty() && active_tasks.load(std::memory_order_relaxed) == 0;
    });
}

// 获取待处理任务数量
size_t threadpool::pending_tasks() const
{
    std::unique_lock<std::mutex> lock(queue_mutex);
    return tasks.size();
}

// 获取正在执行的任务数量
size_t threadpool::active_tasks_count() const
{
    return active_tasks.load(std::memory_order_relaxed);
}

// 获取线程数量
size_t threadpool::size() const
{
    return thread_count;
}

// 检查线程池是否空闲
bool threadpool::is_idle() const
{
    std::unique_lock<std::mutex> lock(queue_mutex);
    return tasks.empty() && active_tasks.load(std::memory_order_relaxed) == 0;
}

// 构造函数
threadpool::threadpool(size_t count) 
    : thread_count(count > 0 ? count : std::thread::hardware_concurrency())
    , active_tasks(0)
    , stop(false)
{
    if (thread_count == 0) {
        thread_count = 1; // 至少有一个线程
    }

    workers.reserve(thread_count);
    for (size_t i = 0; i < thread_count; ++i) {
        workers.emplace_back([this] { work_thread(); });
    }
}

// 析构函数
threadpool::~threadpool()
{
    kill_thread();
}

} // namespace utils

#endif