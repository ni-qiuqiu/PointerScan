//
// Create by 青杉白衣 on 2023
//

#pragma once

#include "sutils.h"

template <class T, class F, class... Args>
void utils::list_for_each(T list, F &&call, Args &&...args)
{
    T head, node;

    if (list == nullptr)
        return;

    node = list->next;
    while (node != nullptr) {
        head = node;
        node = node->next;
        call(head, std::forward<Args>(args)...);
    }
}

template <class C, class F, class T, class M>
void utils::binary_search(C &container, F &&call, T target, size_t size, M &lower, M &upper)
{
    M middle = 0;
    lower = 0, upper = size - 1;

    while (lower <= upper) {
        middle = (lower + upper) >> 1;

        if (call(container[middle], target))
            lower = middle + 1;
        else
            upper = middle - 1;
    }
}

template <class F, class... Args>
void utils::split_num_to_avg(size_t count, size_t avg, F &&call, Args &&...args)
{
    size_t n, t, min;

    min = 0;
    n = DIV_ROUND_UP(count, avg);
    for (auto i = 0ul; i < n; ++i) {
        t = std::min(count - min, avg, std::forward<Args>(args)...);

        call(t);

        min += t;
    }
}

template <typename T>
inline void utils::free_container_data(T &container)
{
    for (auto &dat : container)
        delete dat;
}

template <typename T>
inline void utils::free_list_data(T list)
{
    T head, node;
    node = list;

    while (node != nullptr) {
        head = node;
        node = node->next;
        delete head;
    }
}

template <typename T, typename F>
void utils::free_list_data(T list, F &&call)
{
    T head, node;
    node = list;

    while (node != nullptr) {
        head = node;
        call(node);
        node = node->next;
        delete head;
    }
}

template <class T, class F, class... Args>
void utils::free_list_for_each(T list, F &&call, Args &&...args)
{
    T head, node;

    if (list == nullptr)
        return;

    node = list->next;
    delete list;
    while (node != nullptr) {
        head = node;
        node = node->next;
        call(head, std::forward<Args>(args)...);
        delete head;
    }
}

template <typename F>
void utils::strtok_name(char *name, const char *sign, F &&call)
{
    char *p;
    char *stok = strstr(name, sign);
    char *ret = name;

    while (stok != nullptr) {
        p = ret;
        ret = stok + 1;
        stok = strstr(ret, sign);
        call(ret, stok, p);
    }
}

template <typename T>
constexpr T *utils::address_of(T *x)
{
    return x;
}

template <typename T>
constexpr T *utils::address_of(T &x)
{
    return &x;
}