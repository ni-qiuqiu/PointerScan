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
    if (addr.empty())
        return 0;

    size_t fidx, count;
    utils::timer ptimer;
    std::vector<chainer::pointer_range<T>> ranges;
    std::vector<utils::mapqueue<pointer_dir<T>>> dirs(depth + 1);

    fidx = count = 0;
    ptimer.start();

    for (auto level = 0; level <= depth; ++level) {
        std::vector<pointer_data<T> *> curr;

        printf("\ncurrent level: %d\n", level);

        if (level > 0) {
            //开始根据上一层的utils::mapqueue<pointer_dir<T>> 在全局指针数据中进行搜索
            //
            this->search_pointer(dirs[level - 1], curr, offset, limit, plim);
            printf("%d: search %ld pointers\n", level, curr.size());

            if (curr.empty())
                break;
            //根据curr的数据进行查找静态数据
            //找不到则是将cur数据转到dirs[level]中
            //找到的则是将结果转到ranges中
            this->filter_pointer_ranges(dirs, ranges, curr, level);
            //创建索引
            //对dirs[level]中的指针进行建立到上一层的索引
            //dirs的每一层都是按地址排序的
            this->create_assoc_dir_index(dirs[level - 1], dirs[level], offset, 10000);
            continue;
        }

        this->trans_addr_to_pointer_data(addr, curr);
        std::sort(curr.begin(), curr.end(), [](auto x, auto y) { return x->address < y->address; });
        //获取静态区域中 目标address范围的指针数据
        //找不到则是将cur数据转到dirs[level]中
        //找到的则是将结果转到ranges中
        this->filter_pointer_ranges(dirs, ranges, curr, level);
        fidx = ranges.size();
        utils::free_container_data(curr); // why don't i put it outside the loop is because lazy and level < 0
    }

    //补充静态模块到前一层的索引
    for (; fidx < ranges.size(); ++fidx)
        this->create_assoc_dir_index(dirs[ranges[fidx].level - 1], 
    ranges[fidx].results, offset, 10000);
     // not 'associate_data_index' or not ranges[fidx].results.size() because i wanna run it by multi thread

    utils::thread_pool->wait();
    if (ranges.empty())
        return count;

    printf("\nsearch and associate finish, spend: %fs, start filter pointers\n", ptimer.get() / 1000000.0);

    auto [counts, contents] = this->build_pointer_dirs_tree(dirs, ranges);
    if (counts.size() == 0 || contents.size() == 0)
        return count;

    for (auto &r : ranges) {
        auto temp = 0ul;
        auto &ccount = counts[r.level];
        for (auto &v : r.results)
            temp += ccount[v.end] - ccount[v.start];

        count += temp;
        printf("find %lu chains from %d %s[%d]\n", temp, r.level, r.vma->name, r.vma->count);
    }
    this->integr_data_to_file(contents, ranges, outstream);

    printf("\nfinish write into file, total spend: %fs\n", ptimer.get() / 1000000.0);
    return count;
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
