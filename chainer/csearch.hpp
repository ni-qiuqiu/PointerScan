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
void chainer::search<T>::filter_pointer_to_fmmap(char *buffer, T start,
                    size_t len, memtool::vm_area_data *vma, FILE *&f)
{
    T min, max, sub;

    f = tmpfile(); // wb+;
    if (f == nullptr)
        return;

    min = memtool::extend::vm_area_vec.front()->start,
    max = memtool::extend::vm_area_vec.back()->end;
    sub = max - min;

    if (memtool::extend::readv(start, buffer, len) == -1)
    {
        fclose(f), f = nullptr;
        return;
    }
    // std::vector<std::pair<size_t, size_t>> addr_size_pairs;
    // addr_size_pairs.emplace_back(start, len);
    // std::vector<void *> buffers;
    // buffers.emplace_back(buffer);
    // if (memtool::extend::readv_batch(addr_size_pairs, buffers) == -1)
    // {
    //     fclose(f), f = nullptr;
    //     return;
    // }


    output_pointer_to_file(f, (T *)buffer, start, len / sizeof(T), min, sub);

    fflush(f);
}


template <class T>
template <typename P>
void chainer::search<T>::filter_pointer_from_fmmap(P &&input, 
    chainer::pointer_data<T> *start, size_t count, size_t offset, 
    std::atomic<size_t> &total, utils::list_head<pointer_pcount<T>> *block)
{
    int lower, upper;
    size_t size, pcount;
    T min, max, sub, value;
    pointer_data<T> *data, **save;

    min = memtool::extend::vm_area_vec.front()->start;
    max = memtool::extend::vm_area_vec.back()->end;
    sub = max - min;
    size = input.size();

    pcount = 0;
    save = block->data.data;
    for (auto i = 0ul; i < count; ++i)
    {
        data = start + i;//获取全局指针数据
        //data 就是全局指针数据
        value = data->value;
        if ((value - min) > sub)
            continue;
                      //dir[level-1]
        utils::binary_search(input, search_pointer_by_bin_gt, value, size, lower, upper);
        //遍历全局指针数据表 找到与上一层匹配的指针（上一层是按地址排序的）
        //得到的是匹配索引
        //这里的思路是
        // //1.将全局指针数据表分为多个块 多线程进行 提高效率
        //2.指针数据表中的单个数据进行二分匹配上层数据（按地址排序的）
        // [上层数据比全局指针数据表小 O(n) 而常规是上层数据每一条都去查全局指针数据表 O(m)*O(logn)]
        //

        if ((size_t)lower == size || utils::address_of(input[lower])->address - value > offset)
            continue;

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
    pointer_data<T> *start, **save;

    auto &trf = reinterpret_cast<utils::mapqueue<pointer_data<T> *> &>(cache);

    auto find_pointer = [this, &input, &total, offset](auto start, auto count, auto block)
    {
        filter_pointer_from_fmmap(input, start, count, offset, total, block);
    };

    auto push_pool = [&find_pointer, &start, &save, &node](auto pos)
    {
        node->next = new utils::list_head<pointer_pcount<T>>;
        node = node->next;
        node->data.data = save;

        utils::thread_pool->pushpool(find_pointer, start, pos, node);

        start += pos, save += pos;
    };

    start = &pcoll.front();
    save = &trf.front();
    utils::split_num_to_avg(pcoll.size(), avg, push_pool);
}

template <class T> // 0, 0, false, 10, 1 << 20
size_t chainer::search<T>::get_pointers(T start, T end, bool rest, int count,
                                        int size) {
  FILE *f;
  uint32_t len;
  char *buffer; // 文件io缓冲区

  len = 1 << 20; // 1m
  buffer = new char[len];

  cache.shrink();
  pcoll.shrink();
  f = tmpfile();
  if (f == nullptr)
    return 0;

  auto fptofile = [this](auto buffer, auto start, auto len, auto vma,
                         auto &dat) {
    filter_pointer_to_fmmap(buffer, start, len, vma, dat);
  };
  
  auto ins = memtool::extend::for_each_memory_area<FILE *>(
      start, end, rest, count, size, fptofile);

  auto cat_file_list = [this, &f, &len, &buffer](auto &in) {
    if (in == nullptr)
      return;

    rewind(in);
    // 合并所有指针数据到f文件中
    utils::cat_file_to_another(buffer, len, in, f);
    fclose(in);
  }; // faster than sort i thought

  for (auto &in : ins)
    cat_file_list(in);

  delete[] buffer;

  pcoll.map(f);
  cache.reserve(pcoll.size());
  return pcoll.size();
}

template <class T>
template <typename P, typename U>
void chainer::search<T>::search_pointer(P &&input, U &out, size_t offset, bool rest, size_t limit)
{
    if (input.empty() || pcoll.begin() == nullptr || pcoll.size() == 0)
        return;

    size_t count;
    std::atomic<size_t> total(0);
    utils::list_head<pointer_pcount<T>> *head;

    auto emplace_pointer = [this, &count, &out, &limit](auto n)
    {
        if (n->data.count == 0 || count >= limit)
            return;

        size_t cnt;
        pointer_data<T> **data;

        cnt = n->data.count;
        data = n->data.data;
        for (auto i = 0u; i < cnt; ++i)
            out.emplace_back(data[i]);

        count += cnt;
    };

    count = 0;
    head = new utils::list_head<pointer_pcount<T>>;
    //          dirs[level - 1]       offset
    filter_pointer_to_block(input, offset, head, 10000, total); // 10000 is the avg to split ptr for multi threads, actually it can custom made by uself

    utils::thread_pool->wait();

    limit = rest ? limit : total.load();
    limit = std::min(limit, total.load());
    out.reserve(limit);

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
