#pragma once

#include "mapqueue.h"
#include "sutils.h"
#include "timer.h"

#include "memextend.hpp"

#include "cbase.h"
#include "csearch.h"

namespace chainer
{

template <class T>
class scan : public ::chainer::search<T>
{
protected:
    void trans_addr_to_pointer_data(std::vector<T> &input, std::vector<pointer_data<T> *> &out);

    void trans_to_pointer_pdata(std::vector<pointer_data<T> *> &input, std::vector<pointer_data<T> *> &nr, utils::mapqueue<chainer::pointer_dir<T>> &out);

    template <class P>
    void associate_data_index(P &prev, size_t offset, pointer_dir<T> *start, size_t count);

    // template <class P, template <typename> class Container> clang has fucking bug
    template <class P, class C>
    void create_assoc_dir_index(P &prev, C &curr, size_t offset, size_t avg); // C.type = pointer_dir<T>

    void get_results(std::vector<pointer_data<T> *> &list, std::vector<pointer_data<T> *> &save, T start, T end);

    void filter_pointer_ranges(std::vector<utils::mapqueue<pointer_dir<T>>> &dirs, std::vector<chainer::pointer_range<T>> &ranges, std::vector<chainer::pointer_data<T> *> &curr, int level);

    void merge_pointer_dirs(utils::mapqueue<chainer::pointer_dir<T> *> &stn, pointer_dir<T> *dir, FILE *f);

    void filter_suit_dir(utils::mapqueue<chainer::pointer_dir<T> *> &stn, std::vector<utils::mapqueue<chainer::pointer_dir<T> *>> &contents, std::vector<utils::mapqueue<pointer_dir<T>>> &dirs, std::vector<std::vector<chainer::pointer_range<T> *>> &rmaps, int level);

    void stat_pointer_dir_count(std::vector<utils::mapqueue<size_t>> &counts, std::vector<utils::mapqueue<chainer::pointer_dir<T> *>> &contents);

    void integr_data_to_file(std::vector<utils::mapqueue<chainer::pointer_dir<T> *>> &contents, std::vector<chainer::pointer_range<T>> &ranges, FILE *f);

    void integr_data_to_txt(std::vector<utils::mapqueue<chainer::pointer_dir<T> *>> &contents, std::vector<chainer::pointer_range<T>> &ranges, FILE *f);

    chain_info<T> build_pointer_dirs_tree(std::vector<utils::mapqueue<pointer_dir<T>>> &dirs, std::vector<chainer::pointer_range<T>> &ranges);
}; // about constructor or deconstructor ....

} // namespace chainer

#include "cscan.hpp"