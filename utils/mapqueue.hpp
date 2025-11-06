#pragma once

#include "mapqueue.h"

template <class T>
inline utils::mapqueue<T>::mapqueue()
{
    ssize = scapacity = 0;
    f = nullptr;
    fd = -1;
    data = nullptr;
    use_ashmem = false;
}

template <class T>
inline utils::mapqueue<T>::mapqueue(const utils::mapqueue<T> &src)
{
    ssize = scapacity = 0;
    f = nullptr;
    fd = -1;
    data = nullptr;
    use_ashmem = false;
    operator=(src);
}

template <class T>
utils::mapqueue<T>::mapqueue(utils::mapqueue<T> &&src) noexcept
{
    ssize = scapacity = 0;
    f = nullptr;
    fd = -1;
    data = nullptr;
    use_ashmem = false;
    operator=(std::move(src));
}

template <class T>
inline utils::mapqueue<T> &utils::mapqueue<T>::operator=(const utils::mapqueue<T> &src)
{
    shrink();
    resize(src.ssize);
    if (src.data)
        memcpy(data, src.data, ssize * sizeof(T));

    return *this;
}

template <class T>
utils::mapqueue<T> &utils::mapqueue<T>::operator=(utils::mapqueue<T> &&src) noexcept
{
    if (this == &src)
        return *this;

    close_shared_memory();

    data = src.data;
    ssize = src.ssize;
    scapacity = src.scapacity;
    f = src.f;
    fd = src.fd;
    use_ashmem = src.use_ashmem;

    src.data = nullptr;
    src.f = nullptr;
    src.fd = -1;
    src.use_ashmem = false;
    src.ssize = src.scapacity = 0;

    return *this;
}

template <class T>
inline utils::mapqueue<T>::~mapqueue()
{
    close_shared_memory();
}

template <class T>
void utils::mapqueue<T>::clear()
{
    ssize = 0;
}

template <class T>
inline void utils::mapqueue<T>::shrink()
{
    close_shared_memory();
    ssize = scapacity = 0;
    data = nullptr;
}

template <class T>
inline bool utils::mapqueue<T>::empty() const
{
    return ssize == 0;
}

template <class T>
inline size_t utils::mapqueue<T>::size() const
{
    return ssize;
}

template <class T>
inline size_t utils::mapqueue<T>::size_in_bytes() const
{
    return ssize * sizeof(T);
}

template <class T>
inline size_t utils::mapqueue<T>::max_size() const
{
    return 0x7FFFFFFF / sizeof(T);
}

template <class T>
inline size_t utils::mapqueue<T>::capacity() const
{
    return scapacity;
}

template <class T>
inline T &utils::mapqueue<T>::operator[](size_t i)
{
    return data[i];
}

template <class T>
inline const T &utils::mapqueue<T>::operator[](size_t i) const
{
    return data[i];
}

template <class T>
inline T *utils::mapqueue<T>::begin()
{
    return data;
}

template <class T>
inline const T *utils::mapqueue<T>::begin() const
{
    return data;
}

template <class T>
inline T *utils::mapqueue<T>::end()
{
    return data + ssize;
}

template <class T>
inline const T *utils::mapqueue<T>::end() const
{
    return data + ssize;
}

template <class T>
inline T &utils::mapqueue<T>::front()
{
    return data[0];
}

template <class T>
inline const T &utils::mapqueue<T>::front() const
{
    return data[0];
}

template <class T>
inline T &utils::mapqueue<T>::back()
{
    return data[ssize - 1];
}

template <class T>
inline const T &utils::mapqueue<T>::back() const
{
    return data[ssize - 1];
}

template <class T>
inline void utils::mapqueue<T>::map(FILE *new_f)
{
    struct stat st;

    close_shared_memory();
    
    f = new_f;
    fd = fileno(f);
    use_ashmem = false;
    
    if (fstat(fd, &st) != 0) {
        f = nullptr;
        fd = -1;
        return;
    }
    
    data = (T *)mmap(nullptr, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        data = nullptr;
        f = nullptr;
        fd = -1;
        return;
    }

    ssize = scapacity = (st.st_size / sizeof(T));
}

template <class T>
inline void utils::mapqueue<T>::swap(utils::mapqueue<T> &rhs)
{
    // 交换 size 和 capacity
    size_t rhs_size = rhs.ssize;
    rhs.ssize = ssize;
    ssize = rhs_size;
    
    size_t rhs_cap = rhs.scapacity;
    rhs.scapacity = scapacity;
    scapacity = rhs_cap;
    
    // 交换 data 指针
    T *rhs_data = rhs.data;
    rhs.data = data;
    data = rhs_data;
    
    // 交换文件句柄
    FILE *rhs_f = rhs.f;
    rhs.f = f;
    f = rhs_f;
    
    // 交换文件描述符
    int rhs_fd = rhs.fd;
    rhs.fd = fd;
    fd = rhs_fd;
    
    // 交换 ashmem 标志
    bool rhs_use_ashmem = rhs.use_ashmem;
    rhs.use_ashmem = use_ashmem;
    use_ashmem = rhs_use_ashmem;
}

template <class T>
inline size_t utils::mapqueue<T>::grow_capacity(size_t sz) const
{
    size_t new_capacity = scapacity ? (scapacity + scapacity / 2) : 8;
    return new_capacity > sz ? new_capacity : sz;
}

template <class T>
inline void utils::mapqueue<T>::resize(size_t new_size)
{
    if (new_size > scapacity)
        reserve(grow_capacity(new_size));

    ssize = new_size;
}

template <class T>
inline void utils::mapqueue<T>::resize(size_t new_size, const T &v)
{
    if (new_size > scapacity)
        reserve(grow_capacity(new_size));

    if (new_size > ssize)
        for (auto n = ssize; n < new_size; n++)
            memcpy(&data[n], &v, sizeof(v));

    ssize = new_size;
}

template <class T>
inline void utils::mapqueue<T>::reserve(size_t new_capacity)
{
    if (new_capacity <= scapacity)
        return;

    size_t new_size = new_capacity * sizeof(T);
    T *new_data = nullptr;
    int new_fd = -1;
    FILE *new_f = nullptr;
    bool new_use_ashmem = false;

#ifdef USE_ASHMEM
    // Android: 尝试使用 ashmem
    new_fd = open("/dev/ashmem", O_RDWR);
    if (new_fd >= 0) {
        char name[32];
        snprintf(name, sizeof(name), "mapqueue_%p", (void*)this);
        
        if (ioctl(new_fd, ASHMEM_SET_NAME, name) >= 0 &&
            ioctl(new_fd, ASHMEM_SET_SIZE, new_size) >= 0) {
            new_data = (T *)mmap(nullptr, new_size, PROT_READ | PROT_WRITE, 
                               MAP_SHARED, new_fd, 0);
            if (new_data != MAP_FAILED && new_data != nullptr) {
                new_use_ashmem = true;
            } else {
                close(new_fd);
                new_fd = -1;
                new_data = nullptr;
            }
        } else {
            close(new_fd);
            new_fd = -1;
        }
    }
#endif

    // 回退到标准 tmpfile 方法
    if (new_data == nullptr) {
        new_f = tmpfile();
        if (new_f != nullptr) {
            new_fd = fileno(new_f);
            if (ftruncate(new_fd, new_size) == 0) {
                new_data = (T *)mmap(nullptr, new_size, PROT_READ | PROT_WRITE, 
                                   MAP_SHARED, new_fd, 0);
                if (new_data == MAP_FAILED) {
                    new_data = nullptr;
                    fclose(new_f);
                    new_f = nullptr;
                    new_fd = -1;
                }
            } else {
                fclose(new_f);
                new_f = nullptr;
                new_fd = -1;
            }
        }
    }

    if (new_data == nullptr) {
        return;  // 分配失败
    }

    // 复制旧数据
    if (data && ssize > 0) {
        memcpy(new_data, data, ssize * sizeof(T));
    }

    // 释放旧内存
    close_shared_memory();

    // 更新成员变量
    f = new_f;
    fd = new_fd;
    data = new_data;
    scapacity = new_capacity;
    use_ashmem = new_use_ashmem;
}


template <class T>
inline void utils::mapqueue<T>::push_back(const T &v)
{
    if (ssize == scapacity)
        reserve(grow_capacity(ssize + 1));

    memcpy(&data[ssize], &v, sizeof(v));
    ssize++;
}

template <class T>
template <typename... Args>
inline void utils::mapqueue<T>::emplace_back(Args &&...args)
{
    if (ssize == scapacity)
        reserve(grow_capacity(ssize + 1));

    //data[ssize] = T(std::forward<Args>(args)...);
    new (&data[ssize]) T(std::forward<Args>(args)...);
    ssize++;
}

template <class T>
inline void utils::mapqueue<T>::pop_back()
{
    ssize--;
}

// ==================== 辅助方法实现 ====================

template <class T>
inline bool utils::mapqueue<T>::create_shared_memory(size_t size)
{
    if (size == 0)
        return false;

#ifdef USE_ASHMEM
    // Android: 尝试使用 ashmem
    fd = open("/dev/ashmem", O_RDWR);
    if (fd >= 0) {
        char name[32];
        snprintf(name, sizeof(name), "mapqueue_%p", (void*)this);
        
        if (ioctl(fd, ASHMEM_SET_NAME, name) >= 0 &&
            ioctl(fd, ASHMEM_SET_SIZE, size) >= 0) {
            data = (T *)mmap(nullptr, size, PROT_READ | PROT_WRITE, 
                           MAP_SHARED, fd, 0);
            if (data != MAP_FAILED && data != nullptr) {
                use_ashmem = true;
                scapacity = size / sizeof(T);
                return true;
            }
            //ASharedMemory_create(name, size);
        }
        close(fd);
        fd = -1;
    }
#endif

    // 回退到标准方法
    f = tmpfile();
    if (f != nullptr) {
        fd = fileno(f);
        if (ftruncate(fd, size) == 0) {
            data = (T *)mmap(nullptr, size, PROT_READ | PROT_WRITE, 
                           MAP_SHARED, fd, 0);
            if (data != MAP_FAILED && data != nullptr) {
                use_ashmem = false;
                scapacity = size / sizeof(T);
                return true;
            }
        }
        fclose(f);
        f = nullptr;
        fd = -1;
    }

    return false;
}

template <class T>
inline void utils::mapqueue<T>::close_shared_memory()
{
    if (data != nullptr) {
        munmap(data, scapacity * sizeof(T));
        data = nullptr;
    }

    if (use_ashmem) {
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
    } else {
        if (f != nullptr) {
            fclose(f);
            f = nullptr;
            fd = -1;
        }
    }

    use_ashmem = false;
}

template <class T>
inline bool utils::mapqueue<T>::remap_memory(size_t old_size, size_t new_size)
{
#if defined(__linux__) && !defined(__ANDROID__)
    // Linux (非 Android): 可以使用 mremap 优化
    if (fd >= 0 && !use_ashmem) {
        if (ftruncate(fd, new_size) == 0) {
            void *new_data = mremap(data, old_size, new_size, MREMAP_MAYMOVE);
            if (new_data != MAP_FAILED && new_data != nullptr) {
                data = (T *)new_data;
                scapacity = new_size / sizeof(T);
                return true;
            }
        }
    }
#endif
    
    // Android 或其他平台: 使用标准重新分配
    (void)old_size;  // 避免未使用警告
    (void)new_size;
    return false;
}
