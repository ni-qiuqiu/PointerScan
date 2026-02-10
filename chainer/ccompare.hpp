#pragma once

#include "ccompare.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace chainer {

// ============ 二进制文件验证 ============

template <class T>
void ccompare<T>::validate_bin_file(FILE *file, const std::string &path) {
  if (file == nullptr) {
    throw std::runtime_error("无效的文件句柄: " + path);
  }

  if (fseek(file, 0, SEEK_SET) != 0) {
    throw std::runtime_error("无法定位到文件开头: " + path);
  }

  cprog_header header {};
  if (fread(&header, sizeof(header), 1, file) != 1) {
    throw std::runtime_error("文件过小或不是指针链二进制文件: " + path);
  }

  constexpr char kSignaturePrefix[] = ".bin from chainer";
  if (std::strncmp(header.sign, kSignaturePrefix,
                   sizeof(kSignaturePrefix) - 1) != 0) {
    throw std::runtime_error("检测到非指针链二进制文件: " + path);
  }

  if (header.module_count < 0 || header.level < 0 ||
      header.size != static_cast<int>(sizeof(T))) {
    throw std::runtime_error("指针链文件头字段非法: " + path);
  }

  struct stat st {};
  if (fstat(fileno(file), &st) != 0) {
    throw std::runtime_error("无法获取文件大小: " + path);
  }

  size_t min_size =
      sizeof(cprog_header) +
      static_cast<size_t>(header.module_count) * sizeof(cprog_sym<T>) +
      static_cast<size_t>(header.level) * sizeof(cprog_llen);
  if (static_cast<size_t>(st.st_size) < min_size) {
    throw std::runtime_error("指针链二进制文件损坏或不完整: " + path);
  }

  rewind(file);
}

// ============ 方案三：树上直接统计链数量 ============

template <class T>
size_t ccompare<T>::count_chains_recursive(const cprog_chain_info<T> &info,
                                           const cprog_data<T> &dir,
                                           int level) {
  if (level == 0) {
    return 1;  // 叶子节点，一条链
  }

  int child_level = level - 1;
  if (child_level < 0 || 
      static_cast<size_t>(child_level) >= info.contents.size()) {
    return 0;
  }

  size_t count = 0;
  auto &children = info.contents[child_level];
  for (uint32_t i = dir.start; i < dir.end && i < children.size(); ++i) {
    count += count_chains_recursive(info, children[i], child_level);
  }
  return count;
}

template <class T>
size_t ccompare<T>::count_chains_in_tree(const cprog_chain_info<T> &info) {
  size_t total = 0;
  for (auto &sym : info.syms) {
    if (sym.sym == nullptr || sym.data.size() == 0) continue;
    for (size_t i = 0; i < sym.data.size(); ++i) {
      total += count_chains_recursive(info, sym.data[i], sym.sym->level);
    }
  }
  return total;
}

// ============ 将匹配链写入文件 ============

template <class T>
void ccompare<T>::emit_chain_line(FILE *f, const char *module_name,
                                   int module_index,
                                   const std::vector<T> &path) {
  if (f == nullptr || path.empty()) return;
  fprintf(f, "    = %s[%d]", module_name, module_index);
  for (size_t i = 0; i < path.size(); ++i) {
    fprintf(f, "%s 0x%lX", (i == 0 ? " + " : " -> + "),
            static_cast<unsigned long>(path[i]));
  }
  fputc('\n', f);
}

// ============ 树上递归匹配子树 ============

template <class T>
size_t ccompare<T>::match_subtrees(
    const cprog_chain_info<T> &lhs_info,
    const cprog_chain_info<T> &rhs_info,
    const cprog_data<T> &lhs_dir,
    const cprog_data<T> &rhs_dir,
    int level,
    std::vector<T> &path,
    const char *module_name, int module_index,
    FILE *report)
{
  if (level == 0) {
    // 叶子节点：找到一条匹配链，直接写入文件
    if (report != nullptr) {
      emit_chain_line(report, module_name, module_index, path);
    }
    return 1;
  }

  int child_level = level - 1;
  if (child_level < 0 ||
      static_cast<size_t>(child_level) >= lhs_info.contents.size() ||
      static_cast<size_t>(child_level) >= rhs_info.contents.size()) {
    return 0;
  }

  auto &lhs_children = lhs_info.contents[child_level];
  auto &rhs_children = rhs_info.contents[child_level];

  // 构建 rhs 子节点的 offset 索引：排序数组 + lower_bound
  std::vector<std::pair<T, uint32_t>> rhs_sorted;
  uint32_t rhs_end = std::min(rhs_dir.end, static_cast<uint32_t>(rhs_children.size()));
  if (rhs_end > rhs_dir.start) {
    rhs_sorted.reserve(rhs_end - rhs_dir.start);
    for (uint32_t i = rhs_dir.start; i < rhs_end; ++i) {
      rhs_sorted.emplace_back(rhs_children[i].address - rhs_dir.value, i);
    }
    std::sort(rhs_sorted.begin(), rhs_sorted.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });
  }

  size_t count = 0;

  for (uint32_t i = lhs_dir.start; i < lhs_dir.end && i < lhs_children.size(); ++i) {
    T lhs_offset = lhs_children[i].address - lhs_dir.value;

    // 二分查找匹配的 rhs 子节点
    auto lo = std::lower_bound(
        rhs_sorted.begin(), rhs_sorted.end(), lhs_offset,
        [](const auto &elem, const T &val) { return elem.first < val; });

    if (lo == rhs_sorted.end() || lo->first != lhs_offset) continue;

    path.push_back(lhs_offset);

    for (auto it = lo; it != rhs_sorted.end() && it->first == lhs_offset; ++it) {
      count += match_subtrees(
          lhs_info, rhs_info,
          lhs_children[i], rhs_children[it->second],
          child_level, path,
          module_name, module_index, report
      );
    }

    path.pop_back();
  }

  return count;
}

// ============ 匹配模块根节点 ============

template <class T>
size_t ccompare<T>::match_module_roots(
    const cprog_chain_info<T> &lhs_info,
    const cprog_chain_info<T> &rhs_info,
    const cprog_sym_integr<T> &lhs_sym,
    const cprog_sym_integr<T> &rhs_sym,
    FILE *report)
{
  if (lhs_sym.sym == nullptr || rhs_sym.sym == nullptr) return 0;
  if (lhs_sym.data.size() == 0 || rhs_sym.data.size() == 0) return 0;

  size_t count = 0;

  // 构建 rhs 根节点的 offset 索引：排序数组
  std::vector<std::pair<T, size_t>> rhs_sorted;
  rhs_sorted.reserve(rhs_sym.data.size());
  for (size_t i = 0; i < rhs_sym.data.size(); ++i) {
    rhs_sorted.emplace_back(rhs_sym.data[i].address - rhs_sym.sym->start, i);
  }
  std::sort(rhs_sorted.begin(), rhs_sorted.end(),
            [](const auto &a, const auto &b) { return a.first < b.first; });

  // 在循环外创建 path 并复用
  std::vector<T> path;
  path.reserve(lhs_sym.sym->level + 1);

  if (report != nullptr) {
    fprintf(report, "  保持不变的链:\n");
  }

  for (size_t i = 0; i < lhs_sym.data.size(); ++i) {
    T lhs_offset = lhs_sym.data[i].address - lhs_sym.sym->start;

    auto lo = std::lower_bound(
        rhs_sorted.begin(), rhs_sorted.end(), lhs_offset,
        [](const auto &elem, const T &val) { return elem.first < val; });

    if (lo == rhs_sorted.end() || lo->first != lhs_offset) continue;

    for (auto it = lo; it != rhs_sorted.end() && it->first == lhs_offset; ++it) {
      path.clear();
      path.push_back(lhs_offset);

      count += match_subtrees(
          lhs_info, rhs_info,
          lhs_sym.data[i], rhs_sym.data[it->second],
          lhs_sym.sym->level, path,
          lhs_sym.sym->name, lhs_sym.sym->count, report
      );
    }
  }

  return count;
}

// ============ 二进制文件对比主函数 ============

template <class T>
bin_compare_result<T> ccompare<T>::compare_bin_files(
    const std::string &lhs_path,
    const std::string &rhs_path,
    FILE *report)
{
  // 打开并验证文件
  std::unique_ptr<FILE, decltype(&fclose)> lhs_file(
      fopen(lhs_path.c_str(), "rb"), &fclose);
  std::unique_ptr<FILE, decltype(&fclose)> rhs_file(
      fopen(rhs_path.c_str(), "rb"), &fclose);

  if (!lhs_file) {
    throw std::runtime_error("无法打开指针链文件: " + lhs_path);
  }
  if (!rhs_file) {
    throw std::runtime_error("无法打开指针链文件: " + rhs_path);
  }

  validate_bin_file(lhs_file.get(), lhs_path);
  validate_bin_file(rhs_file.get(), rhs_path);

  // 解析二进制数据（mmap，不展开链）
  auto lhs_info = this->parse_cprog_bin_data(lhs_file.get());
  auto rhs_info = this->parse_cprog_bin_data(rhs_file.get());

  bin_compare_result<T> result;

  // 统计链总数（在树上直接计算，不展开）
  result.lhs_total = count_chains_in_tree(lhs_info);
  result.rhs_total = count_chains_in_tree(rhs_info);

  // 构建 rhs 模块索引：(name, index) -> sym 指针
  std::unordered_map<chain_module_key, const cprog_sym_integr<T>*, 
                     chain_module_key_hash> rhs_module_map;
  for (auto &sym : rhs_info.syms) {
    if (sym.sym == nullptr) continue;
    chain_module_key key(sym.sym->name, sym.sym->count);
    rhs_module_map[key] = &sym;
  }

  // 遍历 lhs 模块，在 rhs 中查找匹配
  size_t total_common = 0;
  for (auto &lhs_sym : lhs_info.syms) {
    if (lhs_sym.sym == nullptr) continue;

    chain_module_key key(lhs_sym.sym->name, lhs_sym.sym->count);
    auto it = rhs_module_map.find(key);
    if (it == rhs_module_map.end()) continue;

    const cprog_sym_integr<T> *rhs_sym = it->second;

    if (report != nullptr) {
      fprintf(report, "模块: %s[%d]\n", lhs_sym.sym->name, lhs_sym.sym->count);
    }

    // 在树上匹配根节点和子树，匹配链直接写入 report
    size_t common_count = match_module_roots(
        lhs_info, rhs_info, lhs_sym, *rhs_sym, report);

    if (common_count > 0) {
      module_chain_diff<T> diff;
      diff.module_name = lhs_sym.sym->name;
      diff.module_index = lhs_sym.sym->count;
      diff.common_count = common_count;
      result.modules.push_back(std::move(diff));
      total_common += common_count;
    }

    if (report != nullptr) {
      fprintf(report, "\n");
    }
  }

  result.unchanged = total_common;
  return result;
}

// ============ 文本文件对比 ============

template <class T>
bool ccompare<T>::parse_txt_line(const std::string &line,
                                 chain_signature<T> &out) {
  const char *p = line.c_str();
  const char *end = p + line.size();

  // 跳过前导空白
  while (p < end && std::isspace(static_cast<unsigned char>(*p))) ++p;
  if (p >= end) return false;

  // 查找 '[' — 模块名结束
  const char *bracket_l = static_cast<const char*>(std::memchr(p, '[', end - p));
  if (bracket_l == nullptr || bracket_l == p) return false;

  out.module_name.assign(p, bracket_l);

  // 解析 module_index
  const char *idx_start = bracket_l + 1;
  char *idx_end = nullptr;
  long idx_val = std::strtol(idx_start, &idx_end, 10);
  if (idx_end == idx_start || idx_end >= end || *idx_end != ']') return false;
  out.module_index = static_cast<int>(idx_val);

  // 从 ']' 之后解析偏移
  out.offsets.clear();
  p = idx_end + 1;

  while (p < end) {
    // 查找 "+ 0x"
    const char *plus = std::strstr(p, "+ 0x");
    if (plus == nullptr) break;

    const char *hex_start = plus + 4;
    if (hex_start >= end) break;

    char *hex_end = nullptr;
    unsigned long value = std::strtoul(hex_start, &hex_end, 16);
    if (hex_end == hex_start) break;

    out.offsets.emplace_back(static_cast<T>(value));
    p = hex_end;
  }

  return !out.offsets.empty();
}

template <class T>
auto ccompare<T>::parse_txt_file(const std::string &path) -> chain_collection {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("无法打开指针链文本文件: " + path);
  }

  chain_collection result;
  result.reserve(4096);
  std::string line;
  chain_signature<T> sig;
  while (std::getline(in, line)) {
    if (parse_txt_line(line, sig)) {
      result.emplace_back(std::move(sig));
    }
  }
  return result;
}

template <class T>
auto ccompare<T>::build_chain_map(const chain_collection &chains)
    -> module_chain_map {
  module_chain_map result;
  for (const auto &chain : chains) {
    chain_module_key key(chain.module_name, chain.module_index);
    result[key].insert(chain.offsets);
  }
  return result;
}

template <class T>
void ccompare<T>::process_module_diff(const chain_module_key &key,
                                      const chain_set *lhs, const chain_set *rhs,
                                      bin_compare_result<T> &result,
                                      size_t &unchanged) {
  module_chain_diff<T> diff;
  diff.module_name = key.name;
  diff.module_index = key.index;

  if (lhs != nullptr && rhs != nullptr) {
    for (const auto &chain : *lhs) {
      if (rhs->find(chain) != rhs->end()) {
        diff.common.emplace_back(chain);
        ++unchanged;
      }
    }
  }

  if (!diff.common.empty()) {
    result.modules.emplace_back(std::move(diff));
  }
}

template <class T>
bin_compare_result<T> ccompare<T>::compare_txt_files(
    const std::string &lhs_path,
    const std::string &rhs_path)
{
  auto lhs_chains = parse_txt_file(lhs_path);
  auto rhs_chains = parse_txt_file(rhs_path);

  bin_compare_result<T> result;
  result.lhs_total = lhs_chains.size();
  result.rhs_total = rhs_chains.size();

  auto lhs_map = build_chain_map(lhs_chains);
  auto rhs_map = build_chain_map(rhs_chains);

  size_t unchanged = 0;
  std::unordered_set<chain_module_key, chain_module_key_hash> visited;
  visited.reserve(lhs_map.size());

  for (auto &entry : lhs_map) {
    const auto &key = entry.first;
    visited.insert(key);

    const chain_set *rhs_set = nullptr;
    auto rhs_it = rhs_map.find(key);
    if (rhs_it != rhs_map.end()) {
      rhs_set = &rhs_it->second;
    }

    process_module_diff(key, &entry.second, rhs_set, result, unchanged);
  }

  for (auto &entry : rhs_map) {
    if (visited.find(entry.first) != visited.end()) {
      continue;
    }
    process_module_diff(entry.first, nullptr, &entry.second, result, unchanged);
  }

  result.unchanged = unchanged;
  return result;
}

// 显式实例化
template class ccompare<uint32_t>;
template class ccompare<size_t>;

}  // namespace chainer
