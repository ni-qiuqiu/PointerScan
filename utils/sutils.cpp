//
// Create by 青杉白衣 on 2023
//

#ifndef APPLIED_UTILS
#define APPLIED_UTILS

#include "sutils.h"

int utils::generic_fls(int x)
{
    int r = 32;

    if (!x)
        return 0;

    if (!(x & 0xffff0000u)) {
        x <<= 16;
        r -= 16;
    }
    if (!(x & 0xff000000u)) {
        x <<= 8;
        r -= 8;
    }
    if (!(x & 0xf0000000u)) {
        x <<= 4;
        r -= 4;
    }
    if (!(x & 0xc0000000u)) {
        x <<= 2;
        r -= 2;
    }
    if (!(x & 0x80000000u)) {
        x <<= 1;
        r -= 1;
    }
    return r;
}

unsigned long utils::roundup_pow_of_two(unsigned long x)
{
    return (1UL << generic_fls(x - 1));
}


void utils::cat_file_to_another(void *buffer, size_t len, FILE *instream, FILE *outstream)
{
    size_t size;

    while ((size = fread(buffer, 1, len, instream)) > 0)
        fwrite(buffer, size, 1, outstream);

    fflush(outstream);
} // rewind or seek by uself

#endif