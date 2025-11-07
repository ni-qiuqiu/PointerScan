#pragma once

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

  // 缓冲区池，替代原有的手动缓冲区管理
  static inline std::unique_ptr<BufferPool> buffer_pool_;

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
  // 使用 BufferGuard RAII 管理缓冲区
  BufferGuard buf_guard(*buffer_pool_);
  char *buf = buf_guard.get();

  // readv(start, buf, size);
  call(buf, start, size, vma, std::forward<Args>(args)...);

  // BufferGuard 析构时自动释放缓冲区
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
  // 初始化 BufferPool，替代手动分配缓冲区数组
  buffer_pool_ = std::make_unique<BufferPool>(count, size);

  printf("for_each_memory_call count %zu\n", vm_area_vec.size());

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

  // BufferPool 会在 unique_ptr 析构时自动清理
  buffer_pool_.reset();
}

template <class C, class F>
auto memtool::extend::for_each_memory_impl<C, F>::for_each_memory_area(
    size_t start, size_t end, bool rest, int count, int size, F &&call) {
  std::vector<C> cache;

  // 计算需要多少 size 大小的块
  size_t total_blocks = 0;
  for (auto &vma : vm_area_vec) {
    if (vma->prot & PROT_READ) {
      size_t mem_size = vma->end - vma->start;
      total_blocks += DIV_ROUND_UP(mem_size, size);
    }
  }
  
  // 预分配内存
  cache.reserve(total_blocks);

  // 统计处理的内存区域数量
  std::atomic<int> processed_count(0);
  
  auto for_each = [size, &call, &cache, &processed_count](
      auto mem_start, auto mem_end, auto vma) {
    if (vma->prot & PROT_READ) {
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
    if (vma->prot & PROT_READ) {
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
