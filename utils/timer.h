//
// Create by 青杉白衣 on 2023
//

#pragma once

#include <sys/time.h>
#include <time.h>

namespace utils
{

class timer
{
protected:
    struct timeval begin, end;

public:
    void start();

    long get();
};

} // namespace utils

inline void utils::timer::start()
{
    gettimeofday(&begin, nullptr);
}

inline long utils::timer::get()
{
    gettimeofday(&end, nullptr);
    return ((end.tv_sec - begin.tv_sec) * 1000000) + end.tv_usec - begin.tv_usec;
}