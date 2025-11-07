#pragma once

#include <memory>

#include "mapqueue.h"
#include "sutils.h"

#include "memextend.hpp"

#include "cbase.h"
#include "BufferPool.hpp"

namespace chainer {

template <class T> class search {
protected:
  utils::mapqueue<pointer_data<T>> pcoll; // pointer_coll

  utils::mapqueue<void *> cache; // 缓存
  
  // 缓冲区池，用于高效管理扫描过程中的内存缓冲区
  std::unique_ptr<memtool::BufferPool> buffer_pool_;

private:
  void output_pointer_to_file(FILE *f, T *buffer, T start, size_t maxn, T min,
                              T sub);

  void filter_pointer_to_fmmap(T start, size_t len,
                               memtool::vm_area_data *vma, FILE *&f);

  template <typename P>
  void filter_pointer_from_fmmap(P &&input, pointer_data<T> *start,
                                 size_t count, size_t offset,
                                 std::atomic<size_t> &total,
                                 utils::list_head<pointer_pcount<T>> *block);

  template <typename P>
  void filter_pointer_to_block(P &&input, size_t offset,
                               utils::list_head<pointer_pcount<T>> *node,
                               size_t avg, std::atomic<size_t> &total);

public:
  size_t get_pointers(T start, T end, bool rest, int count, int size);

  // template <typename P, template <typename> class Container> as what i say,
  // clang has bug
  template <typename P, typename U>
  void search_pointer(P &&input, U &out, size_t offset, bool rest,
                      size_t limit); // out.type = pointer_data<T> *

  search();

  ~search();
};

} // namespace chainer

#include "csearch.hpp"