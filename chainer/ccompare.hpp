#pragma once

#include "ccompare.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
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

// ============ 方案三：树上递归匹配子树 ============

template <class T>
size_t ccompare<T>::match_subtrees(
    const cprog_chain_info<T> &lhs_info,
    const cprog_chain_info<T> &rhs_info,
    const cprog_data<T> &lhs_dir,
    const cprog_data<T> &rhs_dir,
    int level,
    std::vector<T> &path,
    std::vector<std::vector<T>> &common_chains)
{
  if (level == 0) {
    // 叶子节点，找到一条相同的链
    common_chains.push_back(path);
    return 1;
  }

  int child_level = level - 1;
  if (child_level < 0 ||
      static_cast<size_t>(child_level) >= lhs_info.contents.size() ||
      static_cast<size_t>(child_level) >= rhs_info.contents.size()) {
    return 0;
  }

  size_t count = 0;
  auto &lhs_children = lhs_info.contents[child_level];
  auto &rhs_children = rhs_info.contents[child_level];

  // 构建 rhs 子节点的 offset 索引
  std::unordered_map<T, std::vector<uint32_t>> rhs_offset_map;
  for (uint32_t i = rhs_dir.start; i < rhs_dir.end && i < rhs_children.size(); ++i) {
    T offset = rhs_children[i].address - rhs_dir.value;
    rhs_offset_map[offset].push_back(i);
  }

  // 遍历 lhs 子节点，只处理在 rhs 中也存在的（剪枝）
  for (uint32_t i = lhs_dir.start; i < lhs_dir.end && i < lhs_children.size(); ++i) {
    T lhs_offset = lhs_children[i].address - lhs_dir.value;

    auto it = rhs_offset_map.find(lhs_offset);
    if (it == rhs_offset_map.end()) continue;  // 剪枝：rhs 中不存在此 offset

    // 记录当前偏移到路径
    path.push_back(lhs_offset);

    // 对每个匹配的 rhs 子节点递归
    for (uint32_t rhs_idx : it->second) {
      count += match_subtrees(
          lhs_info, rhs_info,
          lhs_children[i], rhs_children[rhs_idx],
          child_level, path, common_chains
      );
    }

    path.pop_back();
  }

  return count;
}

// ============ 方案三：匹配模块根节点 ============

template <class T>
size_t ccompare<T>::match_module_roots(
    const cprog_chain_info<T> &lhs_info,
    const cprog_chain_info<T> &rhs_info,
    const cprog_sym_integr<T> &lhs_sym,
    const cprog_sym_integr<T> &rhs_sym,
    module_chain_diff<T> &diff)
{
  if (lhs_sym.sym == nullptr || rhs_sym.sym == nullptr) return 0;
  if (lhs_sym.data.size() == 0 || rhs_sym.data.size() == 0) return 0;

  size_t count = 0;

  // 构建 rhs 根节点的 offset 索引
  std::unordered_map<T, std::vector<size_t>> rhs_root_map;
  for (size_t i = 0; i < rhs_sym.data.size(); ++i) {
    T offset = rhs_sym.data[i].address - rhs_sym.sym->start;
    rhs_root_map[offset].push_back(i);
  }

  // 遍历 lhs 根节点
  for (size_t i = 0; i < lhs_sym.data.size(); ++i) {
    T lhs_offset = lhs_sym.data[i].address - lhs_sym.sym->start;

    auto it = rhs_root_map.find(lhs_offset);
    if (it == rhs_root_map.end()) continue;  // rhs 中不存在此根 offset

    // 对每个匹配的 rhs 根节点递归匹配子树
    for (size_t rhs_idx : it->second) {
      std::vector<T> path;
      path.push_back(lhs_offset);  // 根偏移

      count += match_subtrees(
          lhs_info, rhs_info,
          lhs_sym.data[i], rhs_sym.data[rhs_idx],
          lhs_sym.sym->level, path, diff.common
      );
    }
  }

  return count;
}

// ============ 方案三：二进制文件对比主函数 ============

template <class T>
bin_compare_result<T> ccompare<T>::compare_bin_files(
    const std::string &lhs_path,
    const std::string &rhs_path)
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
    if (it == rhs_module_map.end()) continue;  // rhs 中无此模块

    const cprog_sym_integr<T> *rhs_sym = it->second;

    // 创建模块差异记录
    module_chain_diff<T> diff;
    diff.module_name = lhs_sym.sym->name;
    diff.module_index = lhs_sym.sym->count;

    // 在树上匹配根节点和子树
    size_t common_count = match_module_roots(
        lhs_info, rhs_info, lhs_sym, *rhs_sym, diff);

    if (!diff.common.empty()) {
      result.modules.push_back(std::move(diff));
      total_common += common_count;
    }
  }

  result.unchanged = total_common;
  return result;
}

// ============ 文本文件对比（保持原有实现） ============

template <class T>
bool ccompare<T>::parse_txt_line(const std::string &line,
                                 chain_signature<T> &out) {
  std::string s = line;
  auto not_space = [](int ch) { return !std::isspace(ch); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
    s.pop_back();
  }
  if (s.empty()) {
    return false;
  }

  auto left_bracket = s.find('[');
  auto right_bracket = s.find(']', left_bracket == std::string::npos
                                       ? std::string::npos
                                       : left_bracket + 1);
  if (left_bracket == std::string::npos || right_bracket == std::string::npos ||
      right_bracket <= left_bracket) {
    return false;
  }

  out.module_name = s.substr(0, left_bracket);

  std::string index_str =
      s.substr(left_bracket + 1, right_bracket - left_bracket - 1);
  try {
    out.module_index = std::stoi(index_str);
  } catch (...) {
    return false;
  }

  out.offsets.clear();

  std::string rest = s.substr(right_bracket + 1);
  std::size_t pos = 0;
  while (true) {
    auto plus_pos = rest.find("+ 0x", pos);
    if (plus_pos == std::string::npos) {
      break;
    }
    plus_pos += 4;
    std::size_t end_pos = plus_pos;
    while (end_pos < rest.size() &&
           std::isxdigit(static_cast<unsigned char>(rest[end_pos]))) {
      ++end_pos;
    }
    if (end_pos == plus_pos) {
      break;
    }
    std::string hex_str = rest.substr(plus_pos, end_pos - plus_pos);
    std::istringstream iss(hex_str);
    std::size_t value = 0;
    iss >> std::hex >> value;
    if (!iss.fail()) {
      out.offsets.emplace_back(static_cast<T>(value));
    }
    pos = end_pos;
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
  std::string line;
  chain_signature<T> sig;
  while (std::getline(in, line)) {
    if (parse_txt_line(line, sig)) {
      result.emplace_back(sig);
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
