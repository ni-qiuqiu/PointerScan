//
// Create by 青杉白衣 on 2023
//

#pragma once

#include "csearch.h"

static auto search_pointer_by_bin_gt = [](auto &&n, auto &&target)
{ return utils::address_of(n)->address < target; };

static auto get_pointer_by_bin_gt = [](auto &&vma, auto &&target)
{ return vma->end < target; };

template <class T>
void chainer::search<T>::output_pointer_to_file(FILE *f, T *buffer, T start, size_t maxn, T min, T sub)
{
    T value;
    size_t size;
    int lower, upper;
    pointer_data<T> data;

    auto &avec = memtool::extend::vm_area_vec;
    size = avec.size();

    for (auto i = 0ul; i < maxn; ++i)
    {
        value = (*(buffer + i)) & 0xffffffffffff; // 取低48位
        if ((value - min) > sub)//值需要在maps范围内
            continue;

        utils::binary_search(avec, get_pointer_by_bin_gt, value, size, lower, upper);
        //二分查找 找到在哪个内存区域

        if ((size_t)lower == size || value < avec[lower]->start)
            continue;

        // printf("value %lx\n", (uint64_t)value);
        data.address = start + i * sizeof(T);
        data.value = value;
        fwrite(&data, sizeof(data), 1, f);
    }
}

template <class T>
void chainer::search<T>::filter_pointer_to_fmmap(char *buffer, T start, size_t len,
                    memtool::vm_area_data *vma, FILE *&f)
{
    f = tmpfile();
    if (f == nullptr) {
        return;
    }

    // 获取内存范围
    auto &vm_vec = memtool::extend::vm_area_vec;
    T min = vm_vec.front()->start;
    T max = vm_vec.back()->end;
    T sub = max - min;

    // 使用传入的缓冲区（由 employ_memory_block 通过 BufferPool 获取）
    // 避免重复获取缓冲区导致死锁
    if (memtool::extend::readv(start, buffer, len) == -1) {
        fclose(f);
        f = nullptr;
        return;
    }

    // 输出指针到文件
    size_t element_count = len / sizeof(T);
    output_pointer_to_file(f, (T *)buffer, start, element_count, min, sub);

    fflush(f);
}


template <class T>
template <typename P>
void chainer::search<T>::filter_pointer_from_fmmap(P &&input, 
    chainer::pointer_data<T> *start, size_t count, size_t offset, 
    std::atomic<size_t> &total, utils::list_head<pointer_pcount<T>> *block)
{
    // 获取内存范围
    auto &vm_vec = memtool::extend::vm_area_vec;
    T min = vm_vec.front()->start;
    T max = vm_vec.back()->end;
    T sub = max - min;
    
    size_t input_size = input.size();
    pointer_data<T> **save = block->data.data;
    size_t pcount = 0;

    // 遍历全局指针数据表，找到与上一层匹配的指针
    // 思路：
    // 1. 将全局指针数据表分为多个块，多线程处理提高效率
    // 2. 对每个指针数据进行二分查找匹配上层数据（按地址排序）
    // 复杂度：O(n) vs 常规 O(m)*O(logn)
    for (size_t i = 0; i < count; ++i) {
        pointer_data<T> *data = start + i;
        T value = data->value;
        
        // 检查值是否在有效范围内
        if ((value - min) > sub) {
            continue;
        }

        // 二分查找匹配的内存区域
        int lower, upper;
        utils::binary_search(input, search_pointer_by_bin_gt, value, 
                           input_size, lower, upper);

        // 验证匹配结果
        if (static_cast<size_t>(lower) == input_size) {
            continue;
        }
        
        T target_addr = utils::address_of(input[lower])->address;
        if (target_addr < value || (target_addr - value) > offset) {
            continue;
        }

        save[pcount++] = data;
    }

    total += pcount;
    block->data.count = pcount;
}

template <class T>
template <typename P>
void chainer::search<T>::filter_pointer_to_block(P &&input, size_t offset,
     utils::list_head<pointer_pcount<T>> *node, size_t avg, std::atomic<size_t> &total)
{
    // 转换缓存为指针队列
    auto &trf = reinterpret_cast<utils::mapqueue<pointer_data<T> *> &>(cache);
    
    pointer_data<T> *start = &pcoll.front();
    pointer_data<T> **save = &trf.front();

    // 创建查找指针的回调函数
    auto find_pointer = [this, &input, &total, offset](
        auto ptr_start, auto count, auto block) {
        filter_pointer_from_fmmap(input, ptr_start, count, offset, total, block);
    };

    // 创建任务分配的回调函数
    auto push_pool = [&](auto block_size) {
        // 创建新的链表节点
        node->next = new utils::list_head<pointer_pcount<T>>;
        node = node->next;
        node->data.data = save;

        // 提交任务到线程池
        utils::thread_pool->pushpool(find_pointer, start, block_size, node);

        // 更新指针位置
        start += block_size;
        save += block_size;
    };

    // 将数据分块并提交到线程池
    utils::split_num_to_avg(pcoll.size(), avg, push_pool);
}

template <class T> // 0, 0, false, 10, 1 << 20
size_t chainer::search<T>::get_pointers(T start, T end, bool rest, int count,
                                        int size) {
  // 清理缓存
  cache.shrink();
  pcoll.shrink();
  
  // 创建临时文件
  FILE *f = tmpfile();
  if (f == nullptr) {
    return 0;
  }

  // 注意：BufferPool 会在 for_each_memory_call 内部创建
  // 这里不需要手动创建，避免重复构造
  
  // 第一阶段：扫描内存，提取指针到临时文件
  auto fptofile = [this](auto buf, auto mem_start, auto mem_len, auto vma, auto &file) {
    // 使用 employ_memory_block 提供的缓冲区，避免重复获取导致死锁
    filter_pointer_to_fmmap(buf, mem_start, mem_len, vma, file);
  };
  
  auto file_list = memtool::extend::for_each_memory_area<FILE *>(
      start, end, rest, count, size, fptofile);

  // 第二阶段：合并所有临时文件
  // 为文件合并创建临时缓冲区（使用 RAII 管理）
  const uint32_t merge_buffer_size = 1 << 20; // 1MB
  std::unique_ptr<char[]> merge_buffer(new char[merge_buffer_size]);
  
  for (auto &tmp_file : file_list) {
    if (tmp_file == nullptr) {
      continue;
    }

    rewind(tmp_file);
    
    // 合并指针数据到主文件
    utils::cat_file_to_another(merge_buffer.get(), merge_buffer_size, tmp_file, f);
    fclose(tmp_file);
  }

  // merge_buffer 会在作用域结束时自动清理

  // 映射并返回结果
  pcoll.map(f);
  cache.reserve(pcoll.size());
  return pcoll.size();
}

template <class T>
template <typename P, typename U>
void chainer::search<T>::search_pointer(P &&input, U &out, size_t offset, 
                                       bool rest, size_t limit)
{
    // 检查输入有效性
    if (input.empty() || pcoll.begin() == nullptr || pcoll.size() == 0) {
        return;
    }

    // 初始化
    std::atomic<size_t> total(0);
    utils::list_head<pointer_pcount<T>> *head = new utils::list_head<pointer_pcount<T>>;
    
    // 第一阶段：分块过滤指针（多线程）
    // 10000 是每个线程处理的平均指针数量，可以根据需要调整
    const size_t avg_block_size = 10000;
    filter_pointer_to_block(input, offset, head, avg_block_size, total);

    // 等待所有线程完成
    utils::thread_pool->wait();

    // 计算最终输出限制
    size_t final_limit = rest ? limit : total.load();
    final_limit = std::min(final_limit, total.load());
    out.reserve(final_limit);

    // 第二阶段：收集结果
    size_t count = 0;
    auto emplace_pointer = [&](auto node) {
        // 检查是否达到限制
        if (node->data.count == 0 || count >= final_limit) {
            return;
        }

        // 复制指针数据到输出
        size_t node_count = node->data.count;
        pointer_data<T> **data = node->data.data;
        
        for (size_t i = 0; i < node_count; ++i) {
            out.emplace_back(data[i]);
        }

        count += node_count;
    };

    utils::free_list_for_each(head, emplace_pointer);
}

template <class T>
chainer::search<T>::search()
{
}

template <class T>
chainer::search<T>::~search()
{
}
