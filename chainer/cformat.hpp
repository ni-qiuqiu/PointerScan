//
// Create by 青杉白衣 on 2023
//

#pragma once

#include "cbase.h"

namespace chainer
{

template <class T>
class format : public ::chainer::base<T>
{
private:
    static inline thread_local FILE *out_f;
    static inline thread_local char *buf;
    static inline thread_local size_t t_count;

    void out_chain_string(char *pre, int level, chainer::cprog_data<T> &dat, std::vector<utils::varray<chainer::cprog_data<T>>> &contents);

public:
    size_t format_bin_chain_data(FILE *instream, FILE *outstream);//输出到单一文件

    size_t format_bin_chain_data(FILE *instream, const char *folder);//输出到文件夹里面 每份文件对应一个符号链 包括深度
};
}; // namespace chainer

template <class T>
void chainer::format<T>::out_chain_string(char *pre, int level, chainer::cprog_data<T> &dat, std::vector<utils::varray<chainer::cprog_data<T>>> &contents)
{
    if (level == 0) {
        *pre++ = '\n';
        *pre = 0;
        fwrite(buf, pre - buf, 1, out_f);
        ++t_count;
    } else {
        for (auto i = dat.start; i < dat.end; ++i) {
            auto n = snprintf(pre, 64, " -> + 0x%lX", (size_t)(contents[level - 1][i].address - dat.value));
            out_chain_string(pre + n, level - 1, contents[level - 1][i], contents);
        }
    }
}

template <class T>
size_t chainer::format<T>::format_bin_chain_data(FILE *instream, FILE *outstream)
{
    if (outstream == nullptr)
        return 0;

    size_t count;

    auto [addr, size, syms, contents] = this->parse_cprog_bin_data(instream);

    count = 0;
    for (auto &sym : syms) {
        // 深度10 × 每层约25字节 + 模块名80字节 = ~330字节，1024足够
        char s_buf[1024];

        for (auto &dat : sym.data) {
            t_count = 0;
            buf = s_buf;
            out_f = outstream;

            auto n = snprintf(s_buf, sizeof(s_buf), "%s[%d] + 0x%lX", sym.sym->name, sym.sym->count, (size_t)(dat.address - sym.sym->start));
            out_chain_string(s_buf + n, sym.sym->level, dat, contents);

            count += t_count;
        }
    }

    return count;
}

template <class T>
size_t chainer::format<T>::format_bin_chain_data(FILE *instream, const char *folder)
{
    if (folder == nullptr)
        return 0;

    char path[1024];
    std::atomic_size_t count(0);

    auto [addr, size, syms, contents] = this->parse_cprog_bin_data(instream);
    auto &c_contents = contents;

    auto out = [this, &c_contents, &count](chainer::cprog_sym_integr<T> &sym, auto of) {
        char s_buf[1024];

        for (auto &dat : sym.data) {
            t_count = 0;
            buf = s_buf;
            out_f = of;

            auto n = snprintf(s_buf, sizeof(s_buf), "%s[%d] + 0x%lX", sym.sym->name, sym.sym->count, (size_t)(dat.address - sym.sym->start));
            out_chain_string(s_buf + n, sym.sym->level, dat, c_contents);

            count += t_count;
        }
        fclose(of);
    };

    for (auto &sym : syms) {
        snprintf(path, sizeof(path), "%s/%d %s[%d]", folder, sym.sym->level, sym.sym->name, sym.sym->count);
        auto of = fopen(path, "w+");
        if (of == nullptr)
            continue;

        utils::thread_pool->pushpool(out, std::ref(sym), of);
    }

    utils::thread_pool->wait();

    return count.load();
}
