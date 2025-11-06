#pragma once

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/user.h> // for PAGE_SIZE
#include <unistd.h>
#include <vector>

#include "memsetting.h"

namespace memtool
{

class base
{
private:
    static inline thread_local iovec mem_local[1];

    static inline thread_local iovec mem_remote[1];

    static inline thread_local size_t page_present;

    static inline int page_handle = -1;

protected:
    base(){};
    ~base(){};

    base(const memtool::base &b) = delete;
    base(memtool::base &&b) = delete;
    base &operator=(const memtool::base &b) = delete;
    base &operator=(memtool::base &&b) = delete;

public:
    static inline pid_t target_pid = -1;

    static int get_pid(const char *package);

    template <typename T, typename S>
    static T readv(S addr);

    template <typename S, typename T>
    static long readv(S addr, T *data);

    template <typename S>
    static long readv(S addr, void *data, size_t size);

    static long readv_batch(const std::vector<std::pair<size_t, size_t>> &addr_size_pairs,
                            std::vector<void *> &buffers);


    template <typename T, typename... Args>
    static T read_pointer(T start, Args &&...args);
};

} // namespace memtool

inline int memtool::base::get_pid(const char *package)
{

    //使用pidof 
    char command[100];
    snprintf(command, sizeof(command), "pidof %s", package);
    FILE *fp = popen(command, "r");
    if (fp == nullptr) {
        return -1;
    }
    char pid[10];
    fscanf(fp, "%s", pid);
    pclose(fp);
    return atoi(pid);
}

template <typename T, typename S>
inline T memtool::base::readv(S addr)
{
    T temp;
    mem_local->iov_base = &temp;
    mem_local->iov_len = sizeof(T);
    mem_remote->iov_base = reinterpret_cast<void *>(addr);
    mem_remote->iov_len = sizeof(T);
    syscall(SYS_process_vm_readv, target_pid, mem_local, 1, mem_remote, 1, 0);
    return temp;
}

template <typename S, typename T>
inline long memtool::base::readv(S addr, T *data)
{
    mem_local->iov_base = data;
    mem_local->iov_len = sizeof(T);
    mem_remote->iov_base = reinterpret_cast<void *>(addr);
    mem_remote->iov_len = sizeof(T);
   return syscall(SYS_process_vm_readv, target_pid, mem_local, 1, mem_remote, 1, 0);
}

template <class S>
inline long memtool::base::readv(S addr, void *data, size_t size)
{

  
    mem_local->iov_base = data;
    mem_local->iov_len = size;
    mem_remote->iov_base = reinterpret_cast<void *>(addr);
    mem_remote->iov_len = size;
    if(size>1024*1024)
    printf("readv size=  %d MB\n",size/1024/1024);

   long result= syscall(SYS_process_vm_readv, target_pid, mem_local, 1, mem_remote, 1, 0);
   
    return result;
}

// 优化为批量读取
inline long
memtool::base::readv_batch(const std::vector<std::pair<size_t, size_t>> &addr_size_pairs,
            std::vector<void *> &buffers) {
  constexpr size_t MAX_IOV = 256; // 内核限制
  std::vector<iovec> local(std::min(addr_size_pairs.size(), MAX_IOV));
  std::vector<iovec> remote(std::min(addr_size_pairs.size(), MAX_IOV));

  for (size_t i = 0; i < std::min(addr_size_pairs.size(), MAX_IOV); ++i) {
    local[i].iov_base = buffers[i];
    local[i].iov_len = addr_size_pairs[i].second;
    remote[i].iov_base = reinterpret_cast<void *>(addr_size_pairs[i].first);
    remote[i].iov_len = addr_size_pairs[i].second;
  }

  return syscall(SYS_process_vm_readv, target_pid, local.data(), local.size(),
                 remote.data(), remote.size(), 0);
}

template <typename T, typename... Args>
inline T memtool::base::read_pointer(T start, Args &&...args)
{
    T address;
    T offset[] = {(T)args...};

    address = start + offset[0];
    for (auto i = 1ul; i < sizeof...(args); ++i) {
        readv<T>(address, &address);
        address += offset[i];
    }
    return address;
}