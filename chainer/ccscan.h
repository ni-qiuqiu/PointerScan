#pragma once

#include "cscan.h"

namespace chainer
{

template <class T>
struct cscan : public ::chainer::scan<T>
{
    //获取潜在指针数据
    size_t get_pointers(T start, T end, bool rest, int count, int size);

    //扫描指针链
    size_t scan_pointer_chain(std::vector<T> &addr, int depth, size_t offset, 
        bool limit, size_t plim, FILE *outstream);
    //addr为指针地址列表 depth为深度 offset为偏移 limit为限制 plim为限制大小 outstream为输出文件

 
    size_t scan_pointer_chain_to_txt(std::vector<T> &addr, int depth,
      size_t offset, bool limit, size_t plim, FILE *outstream);
    //将指针链转为文本格式输出到outstream中
    cscan();

    ~cscan();
};

//显式实例化
extern template class chainer::cscan<uint32_t>;//32位
extern template class chainer::cscan<size_t>;//64位

} // namespace chainer