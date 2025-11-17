#pragma once

#include "cbase.h"

#include <cstddef>
#include <cstdio>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace chainer {

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
  // 只保留在两份文件中都存在的指针链（按模块+偏移路径完全一致）
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
  bin_compare_result<T> compare_bin_files(const std::string &lhs_path,
                                          const std::string &rhs_path);
  bin_compare_result<T> compare_txt_files(const std::string &lhs_path,
                                          const std::string &rhs_path);

 private:
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

  chain_collection parse_file(const std::string &path);
  chain_collection parse_txt_file(const std::string &path);
  bool parse_txt_line(const std::string &line, chain_signature<T> &out);
  void validate_bin_file(FILE *file, const std::string &path);
  void collect_module_chains(const cprog_chain_info<T> &info,
                             const cprog_sym_integr<T> &sym,
                             chain_collection &out);
  void collect_from_dir(const cprog_chain_info<T> &info,
                        const cprog_data<T> &dir, int level,
                        std::vector<T> &offsets,
                        const chain_signature<T> &base,
                        chain_collection &out);
  module_chain_map build_chain_map(const chain_collection &chains);
  void process_module_diff(const chain_module_key &key, const chain_set *lhs,
                           const chain_set *rhs, bin_compare_result<T> &result,
                           size_t &unchanged);
};

extern template class ccompare<uint32_t>;
extern template class ccompare<size_t>;

}  // namespace chainer


