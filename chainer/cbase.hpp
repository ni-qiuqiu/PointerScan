#pragma once

#include "cbase.h"
#include <stdexcept>

#define PARSE_ADDR_DATA(a, b) \
    a = (decltype(a))b;       \
    b += sizeof(*a);

template <class T>
void chainer::base<T>::parse_cprog_bin_data(chainer::cprog_chain_info<T> &b_data)
{
    cprog_llen *llen;
    cprog_sym<T> *sym;
    cprog_header *header;
    cprog_data<T> *data;

    auto addr = b_data.addr;
    auto &syms = b_data.syms;
    auto &contents = b_data.contents;

    PARSE_ADDR_DATA(header, addr);

    syms.assign(header->module_count, {});
    for (auto i = 0; i < header->module_count; ++i) {
        PARSE_ADDR_DATA(sym, addr);

        syms[i].sym = sym;
        data = (decltype(data))addr;

        syms[i].data.set_data(data, sym->pointer_count);
        addr = (char *)(data + sym->pointer_count);
    }

    contents.assign(header->level, {});
    for (auto i = 0; i < header->level; ++i) {
        PARSE_ADDR_DATA(llen, addr);

        data = (decltype(data))addr;
        contents[llen->level].set_data(data, llen->count);
        addr = (char *)(data + llen->count);
    }
}

template <class T>
chainer::cprog_chain_info<T> chainer::base<T>::parse_cprog_bin_data(FILE *f)
{
    int fd;
    struct stat st;
    chainer::cprog_chain_info<T> data;

    fd = fileno(f);
    fstat(fd, &st);
    data.size = st.st_size;
    printf("data.size %ld fd %d\n", data.size, fd);
  data.addr = (char *)mmap(nullptr, data.size, PROT_WRITE | PROT_READ, MAP_SHARED, fd, 0);
  if (data.addr == MAP_FAILED) {
    throw std::runtime_error("指针链文件映射失败");
  }
    parse_cprog_bin_data(data);
    return data;
}

