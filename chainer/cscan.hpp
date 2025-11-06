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
    size_t size, index;

    index = 0;
    size = nr.size();
    out.reserve(input.size() - nr.size());
    for (auto dat : input) {
        if (index < size && dat->address == nr[index]->address)
            index++;
        else
            out.emplace_back(dat->address, dat->value, 0, 1);
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
    pointer_dir<T> *start;

    auto assoc_index = [this, &prev, offset](auto start, auto count) {
         associate_data_index(prev, offset, start, count); };
    auto push_pool = [&assoc_index, &start](auto t) {
        utils::thread_pool->pushpool(assoc_index, start, t);

        start += t;
    };

    start = &curr.front();
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
    size_t size, dist;
    uint32_t left, right;
    pointer_dir<T> *dat;

    auto merge = [dir, f, &dat](auto x, auto y) {
        for (auto i = x; i < y; ++i) {
            dat = &dir[i];
            fwrite(&dat, sizeof(dat), 1, f);
        }
    };

    dist = 0;
    left = right = 0u;
    size = stn.size();
    for (auto i = 0ul; i < size; ++i) {
        auto &start = stn[i]->start, &end = stn[i]->end;

        if (right <= start) {
            dist += start - right;
            left = start, right = end;

            merge(left, right);
        } else if (right < end) {
            merge(right, end);

            right = end;
        }

        start -= dist, end -= dist;
    }
}

template <class T>
void chainer::scan<T>::filter_suit_dir(utils::mapqueue<chainer::pointer_dir<T> *> &stn, std::vector<utils::mapqueue<chainer::pointer_dir<T> *>> &contents, std::vector<utils::mapqueue<pointer_dir<T>>> &dirs, std::vector<std::vector<chainer::pointer_range<T> *>> &rmaps, int level)
{
    FILE *f;
    size_t size;

    auto comp = [](auto &&x, auto &&y) { return x->start < y->start; };

    f = tmpfile();
    if (f == nullptr)
        return;

    stn.clear();
    for (auto &r : rmaps[level]) {
        for (auto &v : r->results) {
            stn.emplace_back(&v);
        }
    }

    auto &content = contents[level];
    size = content.size();
    for (auto i = 0ul; i < size; ++i)
        stn.emplace_back(content[i]);

    std::sort(stn.begin(), stn.end(), comp);

    merge_pointer_dirs(stn, &dirs[level - 1].front(), f);
    fflush(f);
    contents[level - 1].map(f);
}

template <class T>
void chainer::scan<T>::stat_pointer_dir_count(std::vector<utils::mapqueue<size_t>> &counts, std::vector<utils::mapqueue<chainer::pointer_dir<T> *>> &contents)
{
    size_t size, count;

    count = 0;
    counts[0].emplace_back(0);
    counts[0].emplace_back(1);

    for (auto i = 1ul; i < counts.size(); ++i) {
        count = 0;
        auto &ccount = counts[i];
        auto &pcount = counts[i - 1];
        auto &content = contents[i - 1];
        size = content.size();
        ccount.reserve(size + 1);
        ccount.emplace_back(count);

        for (auto j = 0ul; j < size; ++j) {
            count += pcount[content[j]->end] - pcount[content[j]->start];
            ccount.emplace_back(count);
        }
    }
}

template <class T>
void chainer::scan<T>::integr_data_to_file(std::vector<utils::mapqueue<chainer::pointer_dir<T> *>> &contents, std::vector<chainer::pointer_range<T>> &ranges, FILE *f)
{
    size_t size;
    cprog_llen llen;
    cprog_sym<T> sym;
    cprog_header header;
    cprog_data<T> data;

    header.size = sizeof(T);
    header.version = 101; // ?
    header.module_count = ranges.size();
    header.level = contents.size() - 1;
    header.sign[0] = 0;
    strcpy(header.sign, ".bin from chainer, by 青衫白衣\n");
    // strcat(header.sign, header_h_saying);
    fwrite(&header, sizeof(header), 1, f);

    for (auto &r : ranges) {
        sym.start = r.vma->start;
        sym.range = r.vma->range;
        sym.count = r.vma->count;
        sym.level = r.level;
        sym.pointer_count = r.results.size();
        strcpy(sym.name, r.vma->name);
        fwrite(&sym, sizeof(sym), 1, f);

        fwrite(r.results.begin(), sizeof(*r.results.begin()), r.results.size(), f);
    }

    for (auto i = 0ul; i < contents.size() - 1; i++) {
        auto &content = contents[i];
        size = content.size();

        llen.level = i;
        llen.count = size;
        fwrite(&llen, sizeof(llen), 1, f);

        for (auto j = 0ul; j < size; ++j) {
            fwrite(content[j], sizeof(*content[j]), 1, f);
        }
    }
    fflush(f);
}

template <class T>
void chainer::scan<T>::integr_data_to_txt(std::vector<utils::mapqueue<chainer::pointer_dir<T> *>> &contents, std::vector<chainer::pointer_range<T>> &ranges, FILE *f)
{
    if (f == nullptr || ranges.empty())
        return;

    char buf[1024];
    std::atomic_size_t total_count(0);

    // 递归输出指针链的辅助函数
    auto out_chain_recursive = [&contents](FILE *out_f, char *buffer, int level, chainer::pointer_dir<T> *dat) {
        auto impl = [&contents](FILE *out_f, char *buffer, int level, chainer::pointer_dir<T> *dat, auto &impl_ref) -> size_t {
            if (level == 0) {
                // 到达最底层，输出完整链
                strcat(buffer, "\n");
                fwrite(buffer, strlen(buffer), 1, out_f);
                return 1;
            } else {
                size_t count = 0;
                char *current_pos = buffer + strlen(buffer);
                
                // 遍历当前节点索引的所有子节点
                for (auto i = dat->start; i < dat->end; ++i) {
                    *current_pos = 0;  // 重置当前位置
                    auto *child = contents[level - 1][i];
                    
                    // 添加偏移信息
                    auto n = sprintf(current_pos, " -> + 0x%lX", (size_t)(child->address - dat->value));
                    count += impl_ref(out_f, buffer, level - 1, child, impl_ref);
                }
                return count;
            }
        };
        return impl(out_f, buffer, level, dat, impl);
    };

    // 遍历每个模块范围
    for (auto &r : ranges) {
        printf("Writing chains for %s[%d] at level %d, count: %ld\n", 
               r.vma->name, r.vma->count, r.level, r.results.size());
        
        // 遍历该模块中的每个指针数据
        for (auto &dat : r.results) {
            *buf = 0;
            
            // 输出起始部分: 模块名[编号] + 0x偏移
            auto n = sprintf(buf, "%s[%d] + 0x%lX", 
                           r.vma->name, 
                           r.vma->count, 
                           (size_t)(dat.address - r.vma->start));
            
            // 递归输出完整的指针链
            auto count = out_chain_recursive(f, buf, r.level, &dat);
            total_count += count;
        }
    }

    fflush(f);
    printf("Total chains written to txt: %ld\n", total_count.load());
}

template <class T>
chainer::chain_info<T> chainer::scan<T>::build_pointer_dirs_tree(std::vector<utils::mapqueue<chainer::pointer_dir<T>>> &dirs, std::vector<chainer::pointer_range<T>> &ranges)
{
    std::vector<std::vector<chainer::pointer_range<T> *>> rmaps(dirs.size());
    std::vector<utils::mapqueue<size_t>> counts(ranges.back().level + 1);
    std::vector<utils::mapqueue<chainer::pointer_dir<T> *>> contents(ranges.back().level + 1);

    auto &stn = reinterpret_cast<utils::mapqueue<chainer::pointer_dir<T> *> &>(this->cache);

    for (auto &r : ranges)
        rmaps[r.level].emplace_back(&r);

    for (auto i = ranges.back().level; i > 0; i--) {
        filter_suit_dir(stn, contents, dirs, rmaps, i);
        if (contents[i - 1].begin() == nullptr || contents[i - 1].size() == 0)
            return {};

        // printf("%d %d %d %p\n", i, stn.size(), contents[i - 1].size(), contents[i - 1].begin());
    }

    stat_pointer_dir_count(counts, contents);
    return {std::move(counts), std::move(contents)};
}
