//
// Create by 青杉白衣 on 2023
//

#pragma once

#include "varray.h"

template <class T>
inline utils::varray<T>::varray()
{
    ssize = 0;
    data = nullptr;
}

template <class T>
inline utils::varray<T>::varray(const utils::varray<T> &src) noexcept
{
    ssize = 0;
    data = nullptr;
    operator=(src);
}

template <class T>
utils::varray<T>::varray(utils::varray<T> &&src) noexcept
{
    ssize = 0;
    data = nullptr;
    operator=(std::move(src));
}

template <class T>
inline utils::varray<T> &utils::varray<T>::operator=(const utils::varray<T> &src) noexcept
{
    ssize = src.ssize, data = src.data;

    return *this;
}

template <class T>
utils::varray<T> &utils::varray<T>::operator=(utils::varray<T> &&src) noexcept
{
    if (this == &src)
        return *this;

    ssize = src.ssize, data = src.data;
    src.ssize = 0, src.data = nullptr;
    return *this;
}

template <class T>
inline utils::varray<T>::~varray()
{
}

template <class T>
inline size_t utils::varray<T>::size() const
{
    return ssize;
}

template <class T>
inline T &utils::varray<T>::operator[](size_t i)
{
    return data[i];
}

template <class T>
inline const T &utils::varray<T>::operator[](size_t i) const
{
    return data[i];
}

template <class T>
inline T *utils::varray<T>::begin()
{
    return data;
}

template <class T>
inline const T *utils::varray<T>::begin() const
{
    return data;
}

template <class T>
inline T *utils::varray<T>::end()
{
    return data + ssize;
}

template <class T>
inline const T *utils::varray<T>::end() const
{
    return data + ssize;
}

template <class T>
inline T &utils::varray<T>::front()
{
    return data[0];
}

template <class T>
inline const T &utils::varray<T>::front() const
{
    return data[0];
}

template <class T>
inline T &utils::varray<T>::back()
{
    return data[ssize - 1];
}

template <class T>
inline const T &utils::varray<T>::back() const
{
    return data[ssize - 1];
}

template <class T>
inline void utils::varray<T>::swap(utils::varray<T> &rhs)
{
    size_t rhs_size;
    T *rhs_data;

    rhs_size = rhs.ssize;
    rhs_data = rhs.data;

    rhs.ssize = ssize, rhs.data = data;
    ssize = rhs_size, data = rhs_data;
}

template <class T>
inline void utils::varray<T>::resize(size_t s_size)
{
    ssize = s_size;
}

template <class T>
inline void utils::varray<T>::set_data(T *begin)
{
    data = begin;
}

template <class T>
inline void utils::varray<T>::set_data(T *begin, T *end)
{
    data = begin, ssize = end - begin;
}

template <class T>
inline void utils::varray<T>::set_data(T *begin, size_t s_size)
{
    data = begin, ssize = s_size;
}