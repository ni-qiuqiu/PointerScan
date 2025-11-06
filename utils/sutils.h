//
// Create by 青杉白衣 on 2023
//

#pragma once

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "threadpool.h"

#define color_printf(background, front, format, args...) printf("\033[0m\033[" #background ";" #front "m" format "\033[0m", ##args)

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))
#define DIV_ROUND_UP(n, d) (((n) + (d)-1) / (d))

namespace utils
{

template <class T>
struct list_head {
    T data;
    list_head *next;
    list_head() : next(nullptr){};
};

static auto thread_pool(std::make_unique<threadpool>(std::thread::hardware_concurrency() * 1.5));

template <typename T>
void free_container_data(T &container);

template <typename T, typename F, typename... Args>
void list_for_each(T list, F &&call, Args &&...args);

template <typename T, typename F, typename... Args>
void free_list_for_each(T list, F &&call, Args &&...args);

template <typename C, typename F, typename T, typename M>
void binary_search(C &container, F &&call, T target, size_t size, M &lower, M &upper);

template <typename F, typename ...Args>
void split_num_to_avg(size_t count, size_t avg, F &&call, Args &&...args);

template <typename T>
constexpr T *address_of(T *x);

template <typename T>
constexpr T *address_of(T &x);

void cat_file_to_another(void *buffer, size_t len, FILE *instream, FILE *outstream);

} // namespace utils

#include "sutils.hpp"