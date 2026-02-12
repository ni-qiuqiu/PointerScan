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

enum class read_mode {
    PROCESS_VM_READV = 0,  // 默认：process_vm_readv 系统调用
    PROC_MEM_IO = 1,       // /proc/[pid]/mem 文件IO读取
};

class base
{
private:
    static inline thread_local iovec mem_local[1];

    static inline thread_local iovec mem_remote[1];

    static inline thread_local size_t page_present;

    static inline int page_handle = -1;

    // /proc/[pid]/mem 文件描述符 (thread_local，每线程独立打开)
    static inline thread_local int mem_fd = -1;

    static int open_mem_fd();

protected:
    base(){};
    ~base(){};

    base(const memtool::base &b) = delete;
    base(memtool::base &&b) = delete;
    base &operator=(const memtool::base &b) = delete;
    base &operator=(memtool::base &&b) = delete;

public:
    static inline pid_t target_pid = -1;
    static inline read_mode mode = read_mode::PROCESS_VM_READV;

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

inline int memtool::base::open_mem_fd()
{
    if (mem_fd >= 0) return mem_fd;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/mem", target_pid);
    mem_fd = open(path, O_RDONLY);
    if (mem_fd < 0) {
        printf("警告: 无法打开 %s\n", path);
    }
    return mem_fd;
}

inline int memtool::base::get_pid(const char *package)
{
    // 验证包名只包含合法字符，防止命令注入
    for (const char *p = package; *p; ++p) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == ':' || c == '-')) {
            return -1;
        }
    }

    char command[256];
    snprintf(command, sizeof(command), "pidof %s", package);
    FILE *fp = popen(command, "r");
    if (fp == nullptr) {
        return -1;
    }
    char pid[32] = {0};
    fscanf(fp, "%31s", pid);
    pclose(fp);
    return atoi(pid);
}

template <typename T, typename S>
inline T memtool::base::readv(S addr)
{
    T temp{};
    if (mode == read_mode::PROC_MEM_IO) {
        int fd = open_mem_fd();
        if (fd >= 0) {
            pread64(fd, &temp, sizeof(T), static_cast<off64_t>(addr));
        }
    } else {
        mem_local->iov_base = &temp;
        mem_local->iov_len = sizeof(T);
        mem_remote->iov_base = reinterpret_cast<void *>(addr);
        mem_remote->iov_len = sizeof(T);
        syscall(SYS_process_vm_readv, target_pid, mem_local, 1, mem_remote, 1, 0);
    }
    return temp;
}

template <typename S, typename T>
inline long memtool::base::readv(S addr, T *data)
{
    if (mode == read_mode::PROC_MEM_IO) {
        int fd = open_mem_fd();
        if (fd < 0) return -1;
        return pread64(fd, data, sizeof(T), static_cast<off64_t>(addr));
    }
    mem_local->iov_base = data;
    mem_local->iov_len = sizeof(T);
    mem_remote->iov_base = reinterpret_cast<void *>(addr);
    mem_remote->iov_len = sizeof(T);
    return syscall(SYS_process_vm_readv, target_pid, mem_local, 1, mem_remote, 1, 0);
}

template <class S>
inline long memtool::base::readv(S addr, void *data, size_t size)
{
    if(size > 1024*1024)
        printf("readv size= %zu MB\n", size/1024/1024);

    if (mode == read_mode::PROC_MEM_IO) {
        int fd = open_mem_fd();
        if (fd < 0) return -1;
        // pread64 大块读取，按页分段避免部分失败丢失全部数据
        size_t total = 0;
        char *buf = static_cast<char *>(data);
        while (total < size) {
            size_t chunk = std::min(size - total, static_cast<size_t>(PAGE_SIZE));
            ssize_t n = pread64(fd, buf + total, chunk, static_cast<off64_t>(addr) + total);
            if (n <= 0) {
                // 该页不可读，填零跳过
                memset(buf + total, 0, chunk);
                total += chunk;
                continue;
            }
            total += n;
        }
        return static_cast<long>(total);
    }

    mem_local->iov_base = data;
    mem_local->iov_len = size;
    mem_remote->iov_base = reinterpret_cast<void *>(addr);
    mem_remote->iov_len = size;

    long result = syscall(SYS_process_vm_readv, target_pid, mem_local, 1, mem_remote, 1, 0);

    return result;
}

// 优化为批量读取
inline long
memtool::base::readv_batch(const std::vector<std::pair<size_t, size_t>> &addr_size_pairs,
            std::vector<void *> &buffers) {
  if (mode == read_mode::PROC_MEM_IO) {
    int fd = open_mem_fd();
    if (fd < 0) return -1;
    long total = 0;
    for (size_t i = 0; i < addr_size_pairs.size(); ++i) {
      ssize_t n = pread64(fd, buffers[i], addr_size_pairs[i].second,
                          static_cast<off64_t>(addr_size_pairs[i].first));
      if (n > 0) total += n;
    }
    return total;
  }

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