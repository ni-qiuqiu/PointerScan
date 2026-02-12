#pragma once

#include <deque>
#include <list>
#include <vector>
#include <memory>

#include "sutils.h"
#include "threadpool.h"

#include "membase.hpp"
#include "memsetting.h"
#include "BufferPool.hpp"

#include <sys/mman.h>
#include <sys/user.h>

namespace memtool {

class extend final : public ::memtool::base {
private:
  extend();
  ~extend();

  extend(const memtool::extend &b) = delete;
  extend(memtool::extend &&b) = delete;
  extend &operator=(const memtool::extend &b) = delete;
  extend &operator=(memtool::extend &&b) = delete;

  template <typename F, typename... Args>
  static void employ_memory_block(size_t start, size_t size,
                                  vm_area_data *vma, F &&call,
                                  Args &&...args);

  template <typename F>
  static void divide_memory_to_block(size_t start, size_t end,
                                     vm_area_data *vma, int size, F &&call);

  template <typename C, typename F>
  static void divide_memory_to_block(size_t start, size_t end,
                                     vm_area_data *vma, int size, 
                                     C &cache, F &&call);

  template <typename F>
  static void for_each_memory_call(size_t start, size_t end, bool rest,
                                   int count, int size, F &&call);

  template <typename C, typename F> struct for_each_memory_impl {
    static auto for_each_memory_area(size_t start, size_t end, bool rest,
                                     int count, int size, F &&call);
  };

  template <typename F> struct for_each_memory_impl<void, F> {
    static void for_each_memory_area(size_t start, size_t end, bool rest,
                                     int count, int size, F &&call);
  };

public:
  // 缓冲区池，用于高效管理扫描过程中的内存缓冲区
  // 使用 shared_ptr 确保所有任务完成后才销毁
  static inline std::shared_ptr<BufferPool> buffer_pool_;

  static inline std::list<vm_area_data *> vm_area_list; // 全局模块列表

  static inline std::vector<vm_area_data *> vm_area_vec; // 指针扫描列表

  static inline std::list<vm_static_data *> vm_static_list; // 静态扫描列表

  static int get_perms_prot(char *perms);

  static int det_mem_range(char *name, char *prems);

  static int parse_process_maps();

  static int parse_process_module();

  static void set_mem_ranges(int ranges);

  static int get_target_mem();

  template <typename C, typename F>
  static auto for_each_memory_area(size_t start, size_t end, bool rest,
                                   int count, int size,
                                   F &&call); // std::conditional_t<std::is_same_v<C,
                                              // void>, void, std::vector<C>>

  template <typename F>
  static void for_each_page_size(size_t start, size_t len, F &&call);
};

} // namespace memtool

template <class F, class... Args>
void memtool::extend::employ_memory_block(size_t start, size_t size,
                                          memtool::vm_area_data *vma, F &&call,
                                          Args &&...args) {
  // 持有 shared_ptr 拷贝，防止主线程 reset 后 BufferPool 被提前销毁
  auto pool = buffer_pool_;
  if (!pool) {
    printf("警告: 缓冲区池已销毁，跳过内存块 0x%lx\n", start);
    return;
  }

  // RAII 管理缓冲区，确保异常安全
  BufferGuard guard(*pool, 10000);  // 10秒超时
  if (guard.get() == nullptr) {
    printf("警告: 获取缓冲区超时，跳过内存块 0x%lx\n", start);
    return;
  }

  call(guard.get(), start, size, vma, std::forward<Args>(args)...);
}

template <class C, class F>
void memtool::extend::divide_memory_to_block(size_t start, size_t end,
                                             memtool::vm_area_data *vma,
                                             int size, C &cache/*结果缓存*/, F &&call/*回调函数*/) {
  auto employ_memory = [&call, vma](auto s, auto e, auto &dat) {
    employ_memory_block(s, e, vma, call, dat);
  };

  auto push_pool = [&start, &employ_memory, &cache](auto t) {
    auto &dat = cache.emplace_back(typename C::value_type{});

    utils::thread_pool->pushpool(employ_memory, start, t, std::ref(dat));

    start += t;
  };

  utils::split_num_to_avg(end - start, size, push_pool);
}

template <class F>
void memtool::extend::divide_memory_to_block(size_t start, size_t end,
                                             memtool::vm_area_data *vma,
                                             int size, F &&call) {
  auto employ_memory = [&call, vma](auto s, auto e) {
    employ_memory_block(s, e, vma, call);
  };

  auto push_pool = [&start, &employ_memory](auto t) {
    utils::thread_pool->pushpool(employ_memory, start, t);

    start += t;
  };

  utils::split_num_to_avg(end - start, size, push_pool);
}

template <class F>
void memtool::extend::for_each_memory_call(size_t start, size_t end, bool rest,
                                           int count, int size, F &&call) {
  // 【修复】确保缓冲区数量足够，避免死锁
  // 缓冲区数量应该 >= 线程池线程数
  size_t thread_count = utils::thread_pool->size();
  size_t buffer_count = std::max(static_cast<size_t>(count), thread_count + 2);
  
  buffer_pool_ = std::make_shared<BufferPool>(buffer_count, size);

  printf("for_each_memory_call count %zu, buffers %zu, threads %zu\n", 
         vm_area_vec.size(), buffer_count, thread_count);

  if (rest) {
    // 受限模式：只处理指定范围内的内存区域
    for (auto vma : vm_area_vec) {
      size_t range_start = std::max(vma->start, start);
      size_t range_end = std::min(vma->end, end);
      
      if (range_start <= range_end) {
        call(vma->start, vma->end, vma);
      }
    }
  } else {
    // 全量模式：处理所有内存区域
    for (auto vma : vm_area_vec) {
      call(vma->start, vma->end, vma);
    }
  }

  // 等待所有线程完成
  utils::thread_pool->wait();

  // 释放主线程对 BufferPool 的引用
  // 如果仍有任务持有 shared_ptr 拷贝，BufferPool 会在最后一个任务完成后销毁
  buffer_pool_.reset();
}

template <class C, class F>
auto memtool::extend::for_each_memory_impl<C, F>::for_each_memory_area(
    size_t start, size_t end, bool rest, int count, int size, F &&call) {
  // 使用 deque 而非 vector：deque 的 emplace_back 不会使已有元素的引用失效
  // 这保证了通过 std::ref(dat) 传给线程池任务的引用始终有效
  std::deque<C> cache;

  // 统计处理的内存区域数量
  std::atomic<int> processed_count(0);
  
  auto for_each = [size, &call, &cache, &processed_count](
      auto mem_start, auto mem_end, auto vma) {
    if (vma->prot & PROT_READ|| vma->prot & PROT_WRITE) {
      processed_count.fetch_add(1, std::memory_order_relaxed);
      divide_memory_to_block(mem_start, mem_end, vma, size, cache, call);
    }
  };

  for_each_memory_call(start, end, rest, count, size, for_each);
  
  printf("Processed memory areas: %d\n", processed_count.load(std::memory_order_relaxed));
  return cache;
}

template <class F>
void memtool::extend::for_each_memory_impl<void, F>::for_each_memory_area(
    size_t start, size_t end, bool rest, int count, int size, F &&call) {
  auto for_each = [size, &call](auto mem_start, auto mem_end, auto vma) {
    if (vma->prot & PROT_READ|| vma->prot & PROT_WRITE) {
      divide_memory_to_block(mem_start, mem_end, vma, size, call);
    }
  };

  for_each_memory_call(start, end, rest, count, size, for_each);
}

template <class C, class F>
auto memtool::extend::for_each_memory_area(size_t start, size_t end, bool rest,
                                           int count, int size, F &&call) {
  return for_each_memory_impl<C, F>::for_each_memory_area(
      start, end, rest, count, size, std::forward<F>(call));
}

template <class F>
void memtool::extend::for_each_page_size(size_t start, size_t len, F &&call) {
  size_t offset;

  while (len) {
    offset = std::min(PAGE_SIZE - (start & (PAGE_SIZE - 1)), len);

    call(start, offset);

    len -= offset;
    start += offset;
  }
}
