#ifndef CHAINER_CCSCAN_CPP
#define CHAINER_CCSCAN_CPP

#include "ccscan.h"

template <class T>
size_t chainer::cscan<T>::get_pointers(T start, T end, bool rest, int count, int size)
{
    return search<T>::get_pointers(start, end, rest, count, size);
}

template <class T>
size_t chainer::cscan<T>::scan_pointer_chain(std::vector<T> &addr, int depth,
     size_t offset, bool limit, size_t plim, FILE *outstream)
{
    if (addr.empty()) {
        return 0;
    }

    // 初始化
    utils::timer ptimer;
    ptimer.start();
    
    std::vector<chainer::pointer_range<T>> ranges;
    std::vector<utils::mapqueue<pointer_dir<T>>> dirs(depth + 1);
    size_t first_range_idx = 0;
    size_t total_count = 0;

    // 阶段 1: 多级指针链扫描
    for (int level = 0; level <= depth; ++level) {
        std::vector<pointer_data<T> *> curr;
        printf("\ncurrent level: %d\n", level);

        if (level > 0) {
            // 在全局指针数据中搜索上一层的指针
            this->search_pointer(dirs[level - 1], curr, offset, limit, plim);
            printf("%d: search %ld pointers\n", level, curr.size());

            if (curr.empty()) {
                break;
            }

            // 过滤指针范围：找到的加入 ranges，找不到的加入 dirs[level]
            this->filter_pointer_ranges(dirs, ranges, curr, level);
            
            // 创建索引：对 dirs[level] 中的指针建立到上一层的索引
            // dirs 的每一层都是按地址排序的
            this->create_assoc_dir_index(dirs[level - 1], dirs[level], offset, 10000);
            continue;
        }

        // Level 0: 转换地址为指针数据
        this->trans_addr_to_pointer_data(addr, curr);
        std::sort(curr.begin(), curr.end(), 
                 [](auto x, auto y) { return x->address < y->address; });
        
        // 获取静态区域中目标 address 范围的指针数据
        // 找不到的加入 dirs[level]，找到的加入 ranges
        this->filter_pointer_ranges(dirs, ranges, curr, level);
        first_range_idx = ranges.size();
        
        // 清理临时数据
        utils::free_container_data(curr);
    }

    // 阶段 2: 补充静态模块到前一层的索引
    for (; first_range_idx < ranges.size(); ++first_range_idx) {
        this->create_assoc_dir_index(
            dirs[ranges[first_range_idx].level - 1],
            ranges[first_range_idx].results,
            offset,
            10000
        );
    }

    // 等待所有线程完成
    utils::thread_pool->wait();
    
    if (ranges.empty()) {
        return total_count;
    }

    printf("\nsearch and associate finish, spend: %fs, start filter pointers\n",
           ptimer.get() / 1000000.0);

    // 阶段 3: 构建指针目录树
    auto [counts, contents] = this->build_pointer_dirs_tree(dirs, ranges);
    if (counts.empty() || contents.empty()) {
        return total_count;
    }

    // 阶段 4: 统计每个模块的指针链数量
    for (auto &r : ranges) {
        size_t module_count = 0;
        auto &level_count = counts[r.level];
        
        for (auto &v : r.results) {
            module_count += level_count[v.end] - level_count[v.start];
        }

        total_count += module_count;
        printf("find %lu chains from %d %s[%d]\n",
               module_count, r.level, r.vma->name, r.vma->count);
    }

    // 阶段 5: 输出到二进制文件
    this->integr_data_to_file(contents, ranges, outstream);

    printf("\nfinish write into file, total spend: %fs\n",
           ptimer.get() / 1000000.0);
    
    return total_count;
}


template <class T>
size_t chainer::cscan<T>::scan_pointer_chain_to_txt(std::vector<T> &addr, int depth,
     size_t offset, bool limit, size_t plim, FILE *outstream)
{
    if (addr.empty()) {
        return 0;
    }

    // 初始化
    utils::timer ptimer;
    ptimer.start();
    
/*     struct pointer_range {
        int level;
        memtool::vm_static_data *vma;
        //使用自实现的mmap文件队列 避免内存不足oom  bfs扫描模式 内存爆炸
        utils::mapqueue<chainer::pointer_dir<T>> results;
     */
    std::vector<chainer::pointer_range<T>> ranges;
/*     struct pointer_dir {
        T address;
        T value;
        uint32_t start;//索引起始
        uint32_t end;//索引结束 */
    std::vector<utils::mapqueue<pointer_dir<T>>> dirs(depth + 1);
    size_t first_range_idx = 0;
    size_t total_count = 0;

    // 阶段 1: 多级指针链扫描
    for (int level = 0; level <= depth; ++level) {
        std::vector<pointer_data<T> *> curr;
        printf("\ncurrent level: %d\n", level);

        if (level > 0) {
            // 在全局指针数据中搜索上一层的指针
            this->search_pointer(dirs[level - 1], curr, offset, limit, plim);
            printf("%d: search %ld pointers\n", level, curr.size());

            if (curr.empty()) {
                break;
            }

            // 过滤指针范围：找到的加入 ranges，找不到的加入 dirs[level]
            this->filter_pointer_ranges(dirs, ranges, curr, level);
            
            // 创建索引：对 dirs[level] 中的指针建立到上一层的索引
            // dirs 的每一层都是按地址排序的
            this->create_assoc_dir_index(dirs[level - 1], dirs[level], offset, 10000);
            continue;
        }

        // Level 0: 转换地址为指针数据
        this->trans_addr_to_pointer_data(addr, curr);
        std::sort(curr.begin(), curr.end(), 
                 [](auto x, auto y) { return x->address < y->address; });
        
        // 获取静态区域中目标 address 范围的指针数据
        // 找不到的加入 dirs[level]，找到的加入 ranges
        this->filter_pointer_ranges(dirs, ranges, curr, level);
        first_range_idx = ranges.size();
        
        // 清理临时数据
        utils::free_container_data(curr);
    }

    // 阶段 2: 补充静态模块到前一层的索引
    for (; first_range_idx < ranges.size(); ++first_range_idx) {
        this->create_assoc_dir_index(
            dirs[ranges[first_range_idx].level - 1],
            ranges[first_range_idx].results,
            offset,
            10000
        );
    }

    // 等待所有线程完成
    utils::thread_pool->wait();
    
    if (ranges.empty()) {
        return total_count;
    }

    printf("\nsearch and associate finish, spend: %fs, start filter pointers\n",
           ptimer.get() / 1000000.0);

    // 阶段 3: 构建指针目录树
    auto [counts, contents] = this->build_pointer_dirs_tree(dirs, ranges);
    if (counts.empty() || contents.empty()) {
        return total_count;
    }

    // 阶段 4: 统计每个模块的指针链数量
    for (auto &r : ranges) {
        size_t module_count = 0;
        auto &level_count = counts[r.level];
        
        for (auto &v : r.results) {
            module_count += level_count[v.end] - level_count[v.start];
        }

        total_count += module_count;
        printf("find %lu chains from %d %s[%d]\n",
               module_count, r.level, r.vma->name, r.vma->count);
    }

    // 阶段 5: 输出到文本文件
    this->integr_data_to_txt(contents, ranges, outstream);

    printf("\nfinish write into file, total spend: %fs\n",
           ptimer.get() / 1000000.0);
    
    return total_count;
}

template <class T>
chainer::cscan<T>::cscan()
{
}

template <class T>
chainer::cscan<T>::~cscan()
{
}

//显式实例化
template class chainer::cscan<uint32_t>;
template class chainer::cscan<size_t>;

#endif
