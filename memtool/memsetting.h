#pragma once

#include <stdio.h>

namespace memtool
{

enum memsetting {
    All = -1,
    Anonymous = 1 << 5,
    C_alloc = 1 << 2,
    C_heap = 1 << 0,
    C_data = 1 << 4,
    C_bss = 1 << 3,
    Code_app = 1 << 14,
    Code_system = 1 << 15,
    Other = -2080896,//其他默认不解析了

};

struct memory_block_data {
    void *head;
    void *tail;
    int len;
    memory_block_data *next;
    memory_block_data() : head(nullptr), tail(nullptr), len(0), next(nullptr) {}
};

struct vm_area_data {
    size_t start;
    size_t end;
    size_t offset;
    size_t inode;
    char name[128];
    char dev[32];
    int prot;
    char perms[5];

    int range;
    void *user;

    vm_area_data() : user(nullptr) {}
};

struct vm_static_data {
    size_t start;
    size_t end;
    char name[128];
    int range;
    int count;
    bool filter;

    vm_static_data() : range(0), count(1), filter(false) {}
    vm_static_data(size_t s, size_t e) : start(s), end(e), range(0), count(1), filter(false) {}
    vm_static_data(size_t s, size_t e, int r) : start(s), end(e), range(r), count(1), filter(false) {}
};

} // namespace memtool
