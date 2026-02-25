#ifndef CHAINER_CCSCAN_CPP
#define CHAINER_CCSCAN_CPP

#include "ccscan.h"

#include <unordered_set>

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

// ============ 正向指针路径扫描 ============

template <class T>
size_t chainer::cscan<T>::scan_pointer_path(T source, T dest, int depth,
                                             size_t offset, FILE *outstream)
{
    if (this->pcoll.size() == 0 || depth <= 0) {
        return 0;
    }

    utils::timer ptimer;
    ptimer.start();

    // 确保 pcoll 按 address 排序
    printf("[路径扫描] 排序指针数据...\n");
    this->sort_pcoll_by_address();
    printf("[路径扫描] pcoll 大小: %zu, 排序完成\n", this->pcoll.size());

    // BFS 路径节点
    struct path_node {
        T value;          // 当前地址（指针指向的值，即下一跳的基地址）
        T from_offset;    // 从父节点 value 到本指针 address 的偏移
        uint32_t parent;  // 父节点在上一层的索引
    };

    // 每层的 frontier
    std::vector<std::vector<path_node>> levels(depth + 1);

    // 第 0 层：源地址
    levels[0].push_back({source, 0, 0});

    // visited 集合：避免环路和重复展开
    std::unordered_set<T> visited;
    visited.insert(source);

    // 找到的路径（存储为偏移序列）
    struct found_path {
        std::vector<T> offsets;  // 每一跳的偏移
        T final_offset;          // 最后一跳到 dest 的偏移
    };
    std::vector<found_path> paths;

    // pcoll 的 address 二分查找辅助
    auto &pc = this->pcoll;
    auto pcoll_lower = [&pc](T addr) -> size_t {
        size_t lo = 0, hi = pc.size();
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (pc[mid].address < addr) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    };

    size_t total_found = 0;
    const size_t max_paths = 10000;       // 最大路径数限制
    const size_t max_frontier = 500000;   // 每层最大 frontier 大小

    printf("[路径扫描] 源: 0x%lX → 目标: 0x%lX, 深度: %d, 偏移: 0x%lX\n",
           static_cast<unsigned long>(source),
           static_cast<unsigned long>(dest),
           depth,
           static_cast<unsigned long>(offset));

    for (int level = 0; level < depth && total_found < max_paths; ++level) {
        auto &curr = levels[level];
        auto &next = levels[level + 1];

        printf("[路径扫描] 层 %d: frontier 大小 = %zu\n", level, curr.size());

        if (curr.empty()) break;

        for (uint32_t idx = 0; idx < curr.size() && total_found < max_paths; ++idx) {
            T base_addr = curr[idx].value;

            // 在 pcoll 中查找 address ∈ [base_addr, base_addr + offset]
            size_t lo = pcoll_lower(base_addr);
            size_t hi_limit = pc.size();

            for (size_t pi = lo; pi < hi_limit; ++pi) {
                T ptr_addr = pc[pi].address;
                if (ptr_addr > base_addr + offset) break;

                T ptr_value = pc[pi].value;
                T off = ptr_addr - base_addr;

                // 检查是否到达目标
                if (ptr_value >= dest && (ptr_value - dest) <= offset) {
                    // 回溯路径
                    found_path fp;
                    fp.final_offset = dest - ptr_value;

                    std::vector<T> rev_offsets;
                    rev_offsets.push_back(off);

                    uint32_t p_idx = idx;
                    for (int l = level; l > 0; --l) {
                        rev_offsets.push_back(levels[l][p_idx].from_offset);
                        p_idx = levels[l][p_idx].parent;
                    }

                    // 反转得到正序
                    fp.offsets.resize(rev_offsets.size());
                    for (size_t i = 0; i < rev_offsets.size(); ++i) {
                        fp.offsets[i] = rev_offsets[rev_offsets.size() - 1 - i];
                    }

                    paths.push_back(std::move(fp));
                    ++total_found;
                    continue;
                }

                // 未到达目标，加入下一层 frontier
                if (visited.find(ptr_value) == visited.end() && next.size() < max_frontier) {
                    visited.insert(ptr_value);
                    next.push_back({ptr_value, off, idx});
                }
            }
        }

        printf("[路径扫描] 层 %d 完成: 找到 %zu 条路径, 下一层 frontier = %zu\n",
               level, total_found, next.size());
    }

    // 输出结果
    printf("\n[路径扫描] 共找到 %zu 条路径, 耗时 %.3fs\n",
           paths.size(), ptimer.get() / 1000000.0);

    if (outstream != nullptr) {
        fprintf(outstream, "=== 指针路径: 0x%lX → 0x%lX ===\n",
                static_cast<unsigned long>(source),
                static_cast<unsigned long>(dest));
        fprintf(outstream, "深度: %d, 最大偏移: 0x%lX\n\n",
                depth, static_cast<unsigned long>(offset));

        for (size_t i = 0; i < paths.size(); ++i) {
            auto &fp = paths[i];
            fprintf(outstream, "路径 %zu: 0x%lX",
                    i + 1, static_cast<unsigned long>(source));
            for (size_t j = 0; j < fp.offsets.size(); ++j) {
                fprintf(outstream, " + 0x%lX ->",
                        static_cast<unsigned long>(fp.offsets[j]));
            }
            fprintf(outstream, " + 0x%lX [dest]\n",
                    static_cast<unsigned long>(fp.final_offset));
        }

        fprintf(outstream, "\n共找到 %zu 条路径\n", paths.size());
        fflush(outstream);
    }

    return paths.size();
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
