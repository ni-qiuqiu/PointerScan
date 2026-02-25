#pragma once

#include "cbase.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace chainer {

// 匹配进度追踪（跨递归层级）
struct match_progress {
  size_t matched = 0;
  size_t next_check = 500000;
  std::chrono::steady_clock::time_point timer = std::chrono::steady_clock::now();
  double last_print = 0.0;

  void add(size_t count) {
    matched += count;
    if (matched >= next_check) {
      next_check = matched + 500000;
      auto now = std::chrono::steady_clock::now();
      double elapsed = std::chrono::duration<double>(now - timer).count();
      if (elapsed - last_print >= 2.0) {
        double rate = static_cast<double>(matched) / elapsed;
        printf("    已匹配 %zu 条链, %.1fs, %.0f 条/s\n", matched, elapsed, rate);
        last_print = elapsed;
      }
    }
  }
};

struct chain_module_key {
  std::string name;
  int index;

  chain_module_key() = default;
  chain_module_key(std::string module_name, int module_index)
      : name(std::move(module_name)), index(module_index) {}

  bool operator==(const chain_module_key &other) const noexcept {
    return index == other.index && name == other.name;
  }
};

struct chain_module_key_hash {
  size_t operator()(const chain_module_key &key) const noexcept {
    size_t seed = std::hash<std::string>{}(key.name);
    seed ^= static_cast<size_t>(key.index) + 0x9e3779b97f4a7c15ULL +
            (seed << 6) + (seed >> 2);
    return seed;
  }
};

template <class T>
struct chain_signature {
  std::string module_name;
  int module_index = 0;
  std::vector<T> offsets;
};

template <class T>
struct module_chain_diff {
  std::string module_name;
  int module_index = 0;
  size_t common_count = 0;
  // 文本对比时存储完整路径；二进制对比时为空（直接流式写入文件）
  std::vector<std::vector<T>> common;
};

template <class T>
struct bin_compare_result {
  size_t lhs_total = 0;
  size_t rhs_total = 0;
  size_t unchanged = 0;
  std::vector<module_chain_diff<T>> modules;
};

template <class T>
class ccompare : public base<T> {
 public:
  // 二进制文件对比（树上直接查找，高效低内存）
  // report 非空时，匹配到的链直接流式写入文件，不存储在内存中
  bin_compare_result<T> compare_bin_files(const std::string &lhs_path,
                                          const std::string &rhs_path,
                                          FILE *report = nullptr);
  // 文本文件对比
  bin_compare_result<T> compare_txt_files(const std::string &lhs_path,
                                          const std::string &rhs_path);

 private:
  // ============ 前缀和：O(N) 统计链数量 ============

  using prefix_sums_t = std::vector<std::vector<uint64_t>>;

  // 自底向上构建每层前缀和数组
  static prefix_sums_t build_prefix_sums(const cprog_chain_info<T> &info);

  // O(1) 查询一个节点的链数量
  static uint64_t chains_of(const prefix_sums_t &prefix,
                             const cprog_data<T> &dir, int level);

  // 统计总链数并构建前缀和
  size_t count_chains_prefix_sum(const cprog_chain_info<T> &info,
                                  const char *label,
                                  prefix_sums_t &out_prefix);

  // ============ 二进制对比相关方法 ============

  void validate_bin_file(FILE *file, const std::string &path);

  // 匹配模块根节点（双指针归并 + 前缀和剪枝）
  size_t match_module_roots(const cprog_chain_info<T> &lhs_info,
                           const cprog_chain_info<T> &rhs_info,
                           const prefix_sums_t &lhs_prefix,
                           const prefix_sums_t &rhs_prefix,
                           const cprog_sym_integr<T> &lhs_sym,
                           const cprog_sym_integr<T> &rhs_sym,
                           uint64_t lhs_chains, uint64_t rhs_chains,
                           FILE *report);

  // 递归匹配子树（双指针归并 + 前缀和剪枝 + level 1 快速路径）
  size_t match_subtrees(const cprog_chain_info<T> &lhs_info,
                       const cprog_chain_info<T> &rhs_info,
                       const prefix_sums_t &lhs_prefix,
                       const prefix_sums_t &rhs_prefix,
                       const cprog_data<T> &lhs_dir,
                       const cprog_data<T> &rhs_dir,
                       int level,
                       std::vector<T> &path,
                       const char *module_name, int module_index,
                       FILE *report,
                       match_progress &progress);

  // 将一条匹配链写入文件
  static void emit_chain_line(FILE *f, const char *module_name,
                               int module_index, const std::vector<T> &path);

  // ============ 文本文件对比相关方法（保持原有） ============
  
  struct offsets_hash {
    size_t operator()(const std::vector<T> &values) const noexcept {
      size_t seed = 0;
      for (auto value : values) {
        size_t hv = std::hash<T>{}(value);
        seed ^= hv + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
      }
      return seed;
    }
  };

  using chain_collection = std::vector<chain_signature<T>>;
  using chain_set =
      std::unordered_set<std::vector<T>, offsets_hash, std::equal_to<>>;
  using module_chain_map =
      std::unordered_map<chain_module_key, chain_set, chain_module_key_hash>;

  chain_collection parse_txt_file(const std::string &path);
  bool parse_txt_line(const std::string &line, chain_signature<T> &out);
  module_chain_map build_chain_map(const chain_collection &chains);
  void process_module_diff(const chain_module_key &key, const chain_set *lhs,
                           const chain_set *rhs, bin_compare_result<T> &result,
                           size_t &unchanged);
};

extern template class ccompare<uint32_t>;
extern template class ccompare<size_t>;

}  // namespace chainer


