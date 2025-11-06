#pragma once

#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>
#include <atomic>

namespace utils
{

/**
 * @brief 线程池类，用于管理和调度任务执行
 * 
 * 特性:
 * - 支持任意可调用对象 (函数、lambda、仿函数等)
 * - 自动管理线程生命周期
 * - 通过 std::future 获取任务返回值
 * - 支持动态调整线程数量
 * - 线程安全
 */
class threadpool
{
private:
    void work_thread();
    void kill_thread();

    std::vector<std::thread> workers;           // 工作线程集合
    std::queue<std::function<void()>> tasks;    // 任务队列

    size_t thread_count;                        // 线程数量
    std::atomic<size_t> active_tasks;           // 正在执行的任务数
    std::atomic<bool> stop;                     // 停止标志
    
    mutable std::mutex queue_mutex;             // 队列互斥锁 (mutable 以支持 const 方法)
    std::condition_variable condition;          // 条件变量 (用于唤醒工作线程)
    std::condition_variable wait_condition;     // 等待条件变量 (用于 wait())

public:
    /**
     * @brief 构造函数
     * @param count 线程数量，默认为硬件并发线程数
     */
    explicit threadpool(size_t count = std::thread::hardware_concurrency());

    /**
     * @brief 提交任务到线程池
     * @tparam F 可调用对象类型
     * @tparam Args 参数类型
     * @param f 可调用对象
     * @param args 参数
     * @return std::future 用于获取任务返回值
     * @throws std::runtime_error 如果线程池已停止
     */
    template <class F, class... Args>
    auto submit(F &&f, Args &&...args) -> std::future<decltype(f(args...))>;

    /**
     * @brief 兼容旧接口，推荐使用 submit
     */
    template <class F, class... Args>
    auto pushpool(F &&f, Args &&...args) -> std::future<decltype(f(args...))>;

    /**
     * @brief 动态调整线程池大小
     * @param count 新的线程数量
     * @param wait_finish 是否等待当前任务完成，默认为 true
     */
    void resize(size_t count, bool wait_finish = true);

    /**
     * @brief 兼容旧接口，推荐使用 resize
     */
    void change_thread(size_t count);

    /**
     * @brief 等待所有任务完成
     */
    void wait();

    /**
     * @brief 获取待处理任务数量
     */
    size_t pending_tasks() const;

    /**
     * @brief 获取正在执行的任务数量
     */
    size_t active_tasks_count() const;

    /**
     * @brief 获取线程数量
     */
    size_t size() const;

    /**
     * @brief 检查线程池是否空闲
     */
    bool is_idle() const;

    /**
     * @brief 析构函数
     */
    ~threadpool();

    // 禁止拷贝和移动
    threadpool(const threadpool &) = delete;
    threadpool &operator=(const threadpool &) = delete;
    threadpool(threadpool &&) = delete;
    threadpool &operator=(threadpool &&) = delete;
};

} // namespace utils

#include "threadpool.hpp"