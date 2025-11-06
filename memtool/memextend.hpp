#pragma once

#include <list>
#include <unordered_set>
#include <vector>

#include "sutils.h"
#include "threadpool.h"

#include "membase.hpp"
#include "memsetting.h"

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

  static inline std::mutex mem_mutex;

  static inline std::condition_variable mem_condition;

  template <typename F, typename... Args>
  static void employ_memory_block(size_t start, size_t size, char **buffer,
                                  int &index, vm_area_data *vma, F &&call,
                                  Args &&...args);

  template <typename F>
  static void divide_memory_to_block(size_t start, size_t end,
                                     vm_area_data *vma, char **buffer,
                                     int &index, int size, F &&call);

  template <typename C, typename F>
  static void divide_memory_to_block(size_t start, size_t end,
                                     vm_area_data *vma, char **buffer,
                                     int &index, int size, C &cache, F &&call);

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
                                          char **buffer, int &index,
                                          memtool::vm_area_data *vma, F &&call,
                                          Args &&...args) {
  char *buf;

  std::unique_lock<std::mutex> lock(mem_mutex);
  if (index < 0)
    mem_condition.wait(lock, [&index] { return index >= 0; });

  buf = buffer[index];//获取缓冲区
  --index;
  lock.unlock();

  // readv(start, buf, size);
  call(buf, start, size, vma, std::forward<Args>(args)...);

  lock.lock();
  ++index;
  buffer[index] = buf;
  lock.unlock();

  mem_condition.notify_one();
}

template <class C, class F>
void memtool::extend::divide_memory_to_block(size_t start, size_t end,
                                             memtool::vm_area_data *vma,
                                             char **buffer, int &index,
                                             int size, C &cache/*结果缓存*/, F &&call/*回调函数*/) {
  auto employ_memory = [buffer, &call, &index, vma](auto s, auto e, auto &dat) {
    employ_memory_block(s, e, buffer, index, vma, call, dat);
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
                                             char **buffer, int &index,
                                             int size, F &&call) {
  auto employ_memory = [buffer, &call, &index, vma](auto s, auto e) {
    employ_memory_block(s, e, buffer, index, vma, call);
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
  int index;
  char *buffer[count];

  index = count - 1;
  for (auto i = 0; i <= index; ++i)
    buffer[i] = new char[size];//预分配内存

  if (rest)
    goto limit;

  printf("for_each_memory_call count %d\n", vm_area_vec.size());
  for (auto vma : vm_area_vec)
    call(vma->start, vma->end, vma, buffer, index);

  goto wait_for_finish;

limit:
  for (auto vma : vm_area_vec) {
    size_t max, min;

    max = std::max(vma->start, start);
    min = std::min(vma->end, end);
    if (max <= min)
      call(vma->start, vma->end, vma, buffer, index);
  }

wait_for_finish:
  utils::thread_pool->wait();

  for (auto i = 0; i < count; ++i)
    delete[] buffer[i];
}

template <class C, class F>
auto memtool::extend::for_each_memory_impl<C, F>::for_each_memory_area(
    size_t start, size_t end, bool rest, int count, int size, F &&call) {
  size_t t;
  std::vector<C> cache;

  t = 0;
  //计算需要多少size大小的块
  for (auto &vma : vm_area_vec) {
    if (vma->prot & PROT_READ)
      t += DIV_ROUND_UP(vma->end - vma->start, size);
  }
  //预分配内存
  cache.reserve(t);

  std::atomic<int> cout = 0;
  auto for_each = [size/*块大小*/, &call, &cache, &cout](auto start, auto end, auto vma,
                                               auto buffer/*缓冲区*/, int &index/*缓冲区大小*/) {
    if (vma->prot & PROT_READ) {
      cout.fetch_add(1, std::memory_order_relaxed);
      divide_memory_to_block(start, end, vma, buffer, index, size, cache, call);
    }
  };

  for_each_memory_call(start, end, rest, count, size, for_each);
  printf("cout %d\n", cout.load(std::memory_order_relaxed));
  return cache;
}

template <class F>
void memtool::extend::for_each_memory_impl<void, F>::for_each_memory_area(
    size_t start, size_t end, bool rest, int count, int size, F &&call) {
  auto for_each = [size, &call](auto start, auto end, auto vma, auto buffer,
                                auto &index) {
    if (vma->prot & PROT_READ)
      divide_memory_to_block(start, end, vma, buffer, index, size, call);
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
