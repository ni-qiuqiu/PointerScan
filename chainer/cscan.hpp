#pragma once

#include "cscan.h"

static auto get_addr_by_bin_gt = [](auto &&dat, auto &&target) { return utils::address_of(dat)->address < target; };
static auto get_addr_by_bin_lt = [](auto &&dat, auto &&target) { return utils::address_of(dat)->address <= target; };

template <class T>
void chainer::scan<T>::trans_addr_to_pointer_data(std::vector<T> &input, std::vector<pointer_data<T> *> &out)
{
    out.reserve(input.size());
    for (auto address : input)
        out.emplace_back(new pointer_data<T>(address, 0));
}

template <class T>
void chainer::scan<T>::trans_to_pointer_pdata(std::vector<chainer::pointer_data<T> *> &input, std::vector<chainer::pointer_data<T> *> &nr, utils::mapqueue<chainer::pointer_dir<T>> &out)
{
    size_t nr_size = nr.size();
    size_t nr_index = 0;
    
    // 预留空间：input 减去 nr 中匹配的数量
    out.reserve(input.size() - nr_size);
    
    // 遍历 input，跳过在 nr 中存在的地址
    for (auto dat : input) {
        if (nr_index < nr_size && dat->address == nr[nr_index]->address) {
            // 当前地址在 nr 中，跳过
            nr_index++;
        } else {
            // 当前地址不在 nr 中，加入输出
            out.emplace_back(dat->address, dat->value, 0, 1);
        }
    }
}

template <class T>
template <class P>
void chainer::scan<T>::associate_data_index(P &prev, size_t offset, chainer::pointer_dir<T> *start, size_t count)
{
    T value;
    size_t size;
    int lower, upper;
    pointer_dir<T> *data;

    size = prev.size();
    for (auto i = 0ul; i < count; ++i) {
        data = &start[i];
        value = data->value;

        utils::binary_search(prev, get_addr_by_bin_gt, value, size, lower, upper);
        data->start = lower;
        utils::binary_search(prev, get_addr_by_bin_lt, value + offset, size, lower, upper);
        data->end = lower;
    }
} // make sure u'd have [start, end)

template <class T>
template <class P, class C>
void chainer::scan<T>::create_assoc_dir_index(P &prev, C &curr, size_t offset, size_t avg)
{
    pointer_dir<T> *start = &curr.front();

    // Lambda: 为指针目录创建关联索引
    auto assoc_index = [this, &prev, offset](auto ptr_start, auto count) {
        associate_data_index(prev, offset, ptr_start, count);
    };
    
    // Lambda: 分块提交任务到线程池
    auto push_pool = [&](size_t block_size) {
        utils::thread_pool->pushpool(assoc_index, start, block_size);
        start += block_size;
    };

    // 将数据分块并提交到线程池处理
    utils::split_num_to_avg(curr.size(), avg, push_pool);
}

template <class T>
void chainer::scan<T>::get_results(std::vector<pointer_data<T> *> &list, std::vector<pointer_data<T> *> &save, T start, T end)
{
    size_t size;
    int lower, upper, left;

    size = list.size();

    utils::binary_search(list, get_addr_by_bin_gt, start, size, lower, upper);
    left = lower;
    utils::binary_search(list, get_addr_by_bin_gt, end, size, lower, upper);

    if (left > upper)
        return;

    save.assign(list.begin() + left, list.begin() + upper + 1);
} // convenient save [start, end)

template <class T>
void chainer::scan<T>::filter_pointer_ranges(
    std::vector<utils::mapqueue<chainer::pointer_dir<T>>> &dirs, 
    std::vector<chainer::pointer_range<T>> &ranges, 
    std::vector<chainer::pointer_data<T> *> &curr, 
    int level)
{
    std::vector<pointer_data<T> *> nr;

    auto comp = [](auto x, auto y) { return x->address < y->address; };

    for (auto vma : memtool::extend::vm_static_list) {
        if (vma->filter)
            continue;

        decltype(chainer::pointer_range<T>::results) asc;
        std::vector<pointer_data<T> *> results;
        //vm_static_list列表存的是该内存区域中的潜在指针值
        //二分获取 这份区域内 目标address范围的指针数据
        get_results(curr, results, vma->start, vma->end);
        if (results.empty())
            continue;

        asc.reserve(results.size());
        for (auto p : results) {
            nr.emplace_back(p);
            asc.emplace_back(p->address, p->value, 0, 1);
        }

        printf("%s[%d]: %ld pointers\n", vma->name, vma->count, results.size());
        ranges.emplace_back(level, vma, std::move(asc));
    }

    std::sort(nr.begin(), nr.end(), comp); // actually i can sort 'vm_static_list' at once
    trans_to_pointer_pdata(curr, nr, dirs[level]);
}

template <class T>
void chainer::scan<T>::merge_pointer_dirs(utils::mapqueue<chainer::pointer_dir<T> *> &stn, chainer::pointer_dir<T> *dir, FILE *f)
{
    pointer_dir<T> *dat;

    // Lambda: 合并指定范围的指针目录
    auto merge_range = [dir, f, &dat](uint32_t begin, uint32_t end) {
        for (uint32_t i = begin; i < end; ++i) {
            dat = &dir[i];
            fwrite(&dat, sizeof(dat), 1, f);
        }
    };

    size_t dist = 0;
    uint32_t left = 0;
    uint32_t right = 0;
    size_t size = stn.size();
    
    for (size_t i = 0; i < size; ++i) {
        uint32_t &start = stn[i]->start;
        uint32_t &end = stn[i]->end;

        if (right <= start) {
            // 当前范围与之前的不重叠
            dist += start - right;
            left = start;
            right = end;

            merge_range(left, right);
        } else if (right < end) {
            // 当前范围与之前的部分重叠
            merge_range(right, end);
            right = end;
        }

        // 更新索引范围
        start -= dist;
        end -= dist;
    }
}

template <class T>
void chainer::scan<T>::filter_suit_dir(utils::mapqueue<chainer::pointer_dir<T> *> &stn, std::vector<utils::mapqueue<chainer::pointer_dir<T> *>> &contents, std::vector<utils::mapqueue<pointer_dir<T>>> &dirs, std::vector<std::vector<chainer::pointer_range<T> *>> &rmaps, int level)
{
    // 创建临时文件
    FILE *f = tmpfile();
    if (f == nullptr) {
        return;
    }

    // Lambda: 按起始位置排序指针目录
    auto compare_by_start = [](auto &&x, auto &&y) { 
        return x->start < y->start; 
    };

    // 清空并重新填充 stn
    stn.clear();
    
    // 收集当前层级的所有指针范围结果
    for (auto &range_ptr : rmaps[level]) {
        for (auto &result : range_ptr->results) {
            stn.emplace_back(&result);
        }
    }

    // 添加当前层级的内容
    auto &current_content = contents[level];
    for (size_t i = 0; i < current_content.size(); ++i) {
        stn.emplace_back(current_content[i]);
    }

    // 按起始位置排序
    std::sort(stn.begin(), stn.end(), compare_by_start);

    // 合并指针目录并写入文件
    merge_pointer_dirs(stn, &dirs[level - 1].front(), f);
    fflush(f);
    
    // 映射文件到上一层级的内容
    contents[level - 1].map(f);
}

template <class T>
void chainer::scan<T>::stat_pointer_dir_count(std::vector<utils::mapqueue<size_t>> &counts, std::vector<utils::mapqueue<chainer::pointer_dir<T> *>> &contents)
{
    // 初始化第0层：基础计数
    counts[0].emplace_back(0);
    counts[0].emplace_back(1);

    // 遍历每一层，统计累计的指针链数量
    for (size_t i = 1; i < counts.size(); ++i) {
        auto &current_count = counts[i];
        auto &prev_count = counts[i - 1];
        auto &prev_content = contents[i - 1];
        
        size_t content_size = prev_content.size();
        current_count.reserve(content_size + 1);
        
        // 初始累计值
        size_t cumulative_count = 0;
        current_count.emplace_back(cumulative_count);

        // 累加每个节点的指针链数量
        for (size_t j = 0; j < content_size; ++j) {
            auto *dir = prev_content[j];
            cumulative_count += prev_count[dir->end] - prev_count[dir->start];
            current_count.emplace_back(cumulative_count);
        }
    }
}

template <class T>
void chainer::scan<T>::integr_data_to_file(std::vector<utils::mapqueue<chainer::pointer_dir<T> *>> &contents, std::vector<chainer::pointer_range<T>> &ranges, FILE *f)
{
    cprog_header header;
    cprog_sym<T> sym;
    cprog_llen llen;

    // 第一部分：写入文件头
    header.size = sizeof(T);
    header.version = 101;
    header.module_count = ranges.size();
    header.level = contents.size() - 1;
    header.sign[0] = 0;
    strcpy(header.sign, ".bin from chainer, by 青衫白衣\n");
    fwrite(&header, sizeof(header), 1, f);

    // 第二部分：写入每个范围的符号信息和结果
    for (auto &r : ranges) {
        // 填充符号信息
        sym.start = r.vma->start;
        sym.range = r.vma->range;
        sym.count = r.vma->count;
        sym.level = r.level;
        sym.pointer_count = r.results.size();
        strcpy(sym.name, r.vma->name);
        fwrite(&sym, sizeof(sym), 1, f);

        // 写入指针结果数据
        fwrite(r.results.begin(), sizeof(*r.results.begin()), r.results.size(), f);
    }

    // 第三部分：写入每一层的内容数据
    for (size_t i = 0; i < contents.size() - 1; i++) {
        auto &content = contents[i];
        size_t content_size = content.size();

        // 写入层级信息
        llen.level = i;
        llen.count = content_size;
        fwrite(&llen, sizeof(llen), 1, f);

        // 写入该层的所有指针目录
        for (size_t j = 0; j < content_size; ++j) {
            fwrite(content[j], sizeof(*content[j]), 1, f);
        }
    }
    
    fflush(f);
}

template <class T>
void chainer::scan<T>::integr_data_to_txt(std::vector<utils::mapqueue<chainer::pointer_dir<T> *>> &contents, std::vector<chainer::pointer_range<T>> &ranges, FILE *f)
{
    if (f == nullptr || ranges.empty()) {
        return;
    }

    char buffer[1024];
    std::atomic_size_t total_chains(0);

    // Lambda: 递归输出指针链
    // 这个 lambda 使用立即调用的 lambda 表达式 (IIFE) 来实现递归
    auto output_chain_recursive = [&contents](FILE *out_file, char *buf, int level, chainer::pointer_dir<T> *dir) {
        // 递归实现函数
        auto recursive_impl = [&contents](FILE *out_file, char *buf, int level, 
                                         chainer::pointer_dir<T> *dir, auto &self_ref) -> size_t {
            if (level == 0) {
                // 基础情况：到达最底层，输出完整链
                strcat(buf, "\n");
                fwrite(buf, strlen(buf), 1, out_file);
                return 1;
            }
            
            // 递归情况：遍历子节点
            size_t chain_count = 0;
            char *write_position = buf + strlen(buf);
            
            for (uint32_t i = dir->start; i < dir->end; ++i) {
                *write_position = '\0';  // 重置写入位置
                auto *child_dir = contents[level - 1][i];
                
                // 添加偏移信息到缓冲区
                sprintf(write_position, " -> + 0x%lX", 
                       static_cast<size_t>(child_dir->address - dir->value));
                
                // 递归处理子节点
                chain_count += self_ref(out_file, buf, level - 1, child_dir, self_ref);
            }
            
            return chain_count;
        };
        
        // 启动递归
        return recursive_impl(out_file, buf, level, dir, recursive_impl);
    };

    // 遍历每个模块范围
    for (auto &range : ranges) {
        printf("Writing chains for %s[%d] at level %d, count: %ld\n", 
               range.vma->name, range.vma->count, range.level, range.results.size());
        
        // 遍历该模块中的每个指针数据
        for (auto &pointer_dir : range.results) {
            // 重置缓冲区
            buffer[0] = '\0';
            
            // 输出起始部分: 模块名[编号] + 0x偏移
            sprintf(buffer, "%s[%d] + 0x%lX", 
                   range.vma->name, 
                   range.vma->count, 
                   static_cast<size_t>(pointer_dir.address - range.vma->start));
            
            // 递归输出完整的指针链
            size_t chains = output_chain_recursive(f, buffer, range.level, &pointer_dir);
            total_chains += chains;
        }
    }

    fflush(f);
    printf("Total chains written to txt: %ld\n", total_chains.load());
}

template <class T>
chainer::chain_info<T> chainer::scan<T>::build_pointer_dirs_tree(std::vector<utils::mapqueue<chainer::pointer_dir<T>>> &dirs, std::vector<chainer::pointer_range<T>> &ranges)
{
    // 初始化数据结构
    int max_level = ranges.back().level;
    std::vector<std::vector<chainer::pointer_range<T> *>> range_maps(dirs.size());
    std::vector<utils::mapqueue<size_t>> counts(max_level + 1);
    std::vector<utils::mapqueue<chainer::pointer_dir<T> *>> contents(max_level + 1);

    // 复用缓存作为临时存储
    auto &temp_storage = reinterpret_cast<utils::mapqueue<chainer::pointer_dir<T> *> &>(this->cache);

    // 构建范围映射：按层级分组
    for (auto &range : ranges) {
        range_maps[range.level].emplace_back(&range);
    }

    // 从最高层向下过滤和构建指针目录树
    for (int level = max_level; level > 0; --level) {
        filter_suit_dir(temp_storage, contents, dirs, range_maps, level);
        
        // 验证该层内容是否有效
        if (contents[level - 1].empty() || contents[level - 1].begin() == nullptr) {
            return {};  // 返回空结果
        }
    }

    // 统计每层的指针目录数量
    stat_pointer_dir_count(counts, contents);
    
    // 返回构建结果（使用移动语义）
    return {std::move(counts), std::move(contents)};
}
