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

// ============ 前缀和：O(N) 统计链数量 ============

template <class T>
auto ccompare<T>::build_prefix_sums(const cprog_chain_info<T> &info)
    -> prefix_sums_t {
  size_t level_count = info.contents.size();
  prefix_sums_t prefix_sums;
  prefix_sums.reserve(level_count);

  for (size_t k = 0; k < level_count; ++k) {
    auto &nodes = info.contents[k];
    size_t n = nodes.size();
    std::vector<uint64_t> prefix(n + 1, 0);

    if (k == 0) {
      // level 0: 每个节点 = 1 条链
      for (size_t i = 0; i < n; ++i) {
        prefix[i + 1] = prefix[i] + 1;
      }
    } else {
      // level k: 每个节点的链数 = prefix_sums[k-1][end] - prefix_sums[k-1][start]
      auto &prev = prefix_sums[k - 1];
      size_t prev_len = prev.size();
      for (size_t i = 0; i < n; ++i) {
        size_t start = std::min(static_cast<size_t>(nodes[i].start), prev_len - 1);
        size_t end = std::min(static_cast<size_t>(nodes[i].end), prev_len - 1);
        uint64_t node_chains = prev[end] - prev[start];
        prefix[i + 1] = prefix[i] + node_chains;
      }
    }

    prefix_sums.push_back(std::move(prefix));
  }

  return prefix_sums;
}

template <class T>
uint64_t ccompare<T>::chains_of(const prefix_sums_t &prefix,
                                 const cprog_data<T> &dir, int level) {
  if (level == 0) return 1;
  int child_level = level - 1;
  if (child_level < 0 ||
      static_cast<size_t>(child_level) >= prefix.size()) {
    return 0;
  }
  auto &prev = prefix[child_level];
  size_t prev_len = prev.size();
  size_t start = std::min(static_cast<size_t>(dir.start), prev_len - 1);
  size_t end = std::min(static_cast<size_t>(dir.end), prev_len - 1);
  return prev[end] - prev[start];
}

template <class T>
size_t ccompare<T>::count_chains_prefix_sum(const cprog_chain_info<T> &info,
                                             const char *label,
                                             prefix_sums_t &out_prefix) {
  out_prefix = build_prefix_sums(info);

  uint64_t total = 0;
  for (size_t idx = 0; idx < info.syms.size(); ++idx) {
    auto &sym = info.syms[idx];
    if (sym.sym == nullptr || sym.data.size() == 0) continue;
    int level = sym.sym->level;
    uint64_t sym_count = 0;
    for (size_t i = 0; i < sym.data.size(); ++i) {
      sym_count += chains_of(out_prefix, sym.data[i], level);
    }
    if (sym_count > 0) {
      printf("  [%s] [%zu/%zu] %s[%d]: %llu 条链\n",
             label, idx + 1, info.syms.size(),
             sym.sym->name, sym.sym->count,
             static_cast<unsigned long long>(sym_count));
    }
    total += sym_count;
  }

  return static_cast<size_t>(total);
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

// ============ 树上递归匹配子树（双指针归并 + 前缀和剪枝 + level 1 快速路径） ============

template <class T>
size_t ccompare<T>::match_subtrees(
    const cprog_chain_info<T> &lhs_info,
    const cprog_chain_info<T> &rhs_info,
    const prefix_sums_t &lhs_prefix,
    const prefix_sums_t &rhs_prefix,
    const cprog_data<T> &lhs_dir,
    const cprog_data<T> &rhs_dir,
    int level,
    std::vector<T> &path,
    const char *module_name, int module_index,
    FILE *report,
    match_progress &progress)
{
  if (level == 0) {
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

  uint32_t lhs_start = lhs_dir.start;
  uint32_t lhs_end = std::min(lhs_dir.end, static_cast<uint32_t>(lhs_children.size()));
  uint32_t rhs_start = rhs_dir.start;
  uint32_t rhs_end = std::min(rhs_dir.end, static_cast<uint32_t>(rhs_children.size()));

  // level == 1 快速路径：子节点是叶子，直接计数，避免逐个递归到 level 0
  if (level == 1) {
    uint32_t li = lhs_start, ri = rhs_start;
    size_t count = 0;

    while (li < lhs_end && ri < rhs_end) {
      T lhs_off = lhs_children[li].address - lhs_dir.value;
      T rhs_off = rhs_children[ri].address - rhs_dir.value;

      if (lhs_off < rhs_off) {
        ++li;
      } else if (lhs_off > rhs_off) {
        ++ri;
      } else {
        // 找出两侧重复节点的范围
        uint32_t lhs_dup_end = li + 1;
        while (lhs_dup_end < lhs_end &&
               lhs_children[lhs_dup_end].address - lhs_dir.value == lhs_off) {
          ++lhs_dup_end;
        }
        uint32_t rhs_dup_end = ri + 1;
        while (rhs_dup_end < rhs_end &&
               rhs_children[rhs_dup_end].address - rhs_dir.value == lhs_off) {
          ++rhs_dup_end;
        }

        size_t pairs = static_cast<size_t>(lhs_dup_end - li) *
                       static_cast<size_t>(rhs_dup_end - ri);
        if (report != nullptr) {
          path.push_back(lhs_off);
          for (size_t p = 0; p < pairs; ++p) {
            emit_chain_line(report, module_name, module_index, path);
          }
          path.pop_back();
        }
        count += pairs;
        progress.add(pairs);

        li = lhs_dup_end;
        ri = rhs_dup_end;
      }
    }
    return count;
  }

  // 通用路径：双指针归并 + 前缀和剪枝 + 递归
  uint32_t li = lhs_start, ri = rhs_start;
  size_t count = 0;

  while (li < lhs_end && ri < rhs_end) {
    T lhs_off = lhs_children[li].address - lhs_dir.value;
    T rhs_off = rhs_children[ri].address - rhs_dir.value;

    if (lhs_off < rhs_off) {
      ++li;
    } else if (lhs_off > rhs_off) {
      ++ri;
    } else {
      uint32_t lhs_dup_end = li + 1;
      while (lhs_dup_end < lhs_end &&
             lhs_children[lhs_dup_end].address - lhs_dir.value == lhs_off) {
        ++lhs_dup_end;
      }
      uint32_t rhs_dup_end = ri + 1;
      while (rhs_dup_end < rhs_end &&
             rhs_children[rhs_dup_end].address - rhs_dir.value == lhs_off) {
        ++rhs_dup_end;
      }

      path.push_back(lhs_off);
      for (uint32_t lx = li; lx < lhs_dup_end; ++lx) {
        // 前缀和剪枝：跳过空分支
        if (chains_of(lhs_prefix, lhs_children[lx], child_level) == 0) {
          continue;
        }
        for (uint32_t rx = ri; rx < rhs_dup_end; ++rx) {
          if (chains_of(rhs_prefix, rhs_children[rx], child_level) == 0) {
            continue;
          }
          count += match_subtrees(
              lhs_info, rhs_info,
              lhs_prefix, rhs_prefix,
              lhs_children[lx], rhs_children[rx],
              child_level, path,
              module_name, module_index,
              report, progress);
        }
      }
      path.pop_back();

      li = lhs_dup_end;
      ri = rhs_dup_end;
    }
  }

  return count;
}

// ============ 匹配模块根节点（双指针归并 + 前缀和剪枝） ============

template <class T>
size_t ccompare<T>::match_module_roots(
    const cprog_chain_info<T> &lhs_info,
    const cprog_chain_info<T> &rhs_info,
    const prefix_sums_t &lhs_prefix,
    const prefix_sums_t &rhs_prefix,
    const cprog_sym_integr<T> &lhs_sym,
    const cprog_sym_integr<T> &rhs_sym,
    uint64_t lhs_chains, uint64_t rhs_chains,
    FILE *report)
{
  if (lhs_sym.sym == nullptr || rhs_sym.sym == nullptr) return 0;
  if (lhs_sym.data.size() == 0 || rhs_sym.data.size() == 0) return 0;

  std::vector<T> path;
  path.reserve(lhs_sym.sym->level + 1);
  const char *module_name = lhs_sym.sym->name;
  int module_index = lhs_sym.sym->count;
  int level = lhs_sym.sym->level;

  if (report != nullptr) {
    fprintf(report, "  保持不变的链:\n");
  }

  printf("    根节点: 旧=%zu 新=%zu, 链数: 旧=%llu 新=%llu, 上界=%llu\n",
         lhs_sym.data.size(), rhs_sym.data.size(),
         static_cast<unsigned long long>(lhs_chains),
         static_cast<unsigned long long>(rhs_chains),
         static_cast<unsigned long long>(std::min(lhs_chains, rhs_chains)));

  size_t lhs_total_roots = lhs_sym.data.size();
  match_progress progress;

  // 双指针归并：根节点按 address 排序 → offset (address - sym.start) 也有序
  size_t li = 0, ri = 0;
  size_t count = 0;

  while (li < lhs_sym.data.size() && ri < rhs_sym.data.size()) {
    T lhs_off = lhs_sym.data[li].address - lhs_sym.sym->start;
    T rhs_off = rhs_sym.data[ri].address - rhs_sym.sym->start;

    if (lhs_off < rhs_off) {
      ++li;
    } else if (lhs_off > rhs_off) {
      ++ri;
    } else {
      // 找出两侧重复节点的范围
      size_t lhs_dup_end = li + 1;
      while (lhs_dup_end < lhs_sym.data.size() &&
             lhs_sym.data[lhs_dup_end].address - lhs_sym.sym->start == lhs_off) {
        ++lhs_dup_end;
      }
      size_t rhs_dup_end = ri + 1;
      while (rhs_dup_end < rhs_sym.data.size() &&
             rhs_sym.data[rhs_dup_end].address - rhs_sym.sym->start == lhs_off) {
        ++rhs_dup_end;
      }

      // 交叉匹配所有重复对，用前缀和剪枝
      for (size_t lx = li; lx < lhs_dup_end; ++lx) {
        if (chains_of(lhs_prefix, lhs_sym.data[lx], level) == 0) {
          continue;
        }
        for (size_t rx = ri; rx < rhs_dup_end; ++rx) {
          if (chains_of(rhs_prefix, rhs_sym.data[rx], level) == 0) {
            continue;
          }
          path.clear();
          path.push_back(lhs_off);
          count += match_subtrees(
              lhs_info, rhs_info,
              lhs_prefix, rhs_prefix,
              lhs_sym.data[lx], rhs_sym.data[rx],
              level, path,
              module_name, module_index,
              report, progress);
        }
      }

      li = lhs_dup_end;
      ri = rhs_dup_end;
    }
  }

  // 最终进度
  if (progress.matched > 0) {
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - progress.timer).count();
    printf("    根节点遍历完成: %zu/%zu, 匹配 %zu 条链, %.3fs\n",
           std::min(li, lhs_total_roots), lhs_total_roots, count, elapsed);
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
  printf("[对比] 解析二进制数据...\n");
  auto lhs_info = this->parse_cprog_bin_data(lhs_file.get());
  auto rhs_info = this->parse_cprog_bin_data(rhs_file.get());
  printf("  旧文件: %zu 个模块, %zu 层\n", lhs_info.syms.size(), lhs_info.contents.size());
  printf("  新文件: %zu 个模块, %zu 层\n", rhs_info.syms.size(), rhs_info.contents.size());

  bin_compare_result<T> result;

  // 用前缀和统计链总数（O(N) 替代递归 O(total_chains)）
  printf("[对比] 统计链数量...\n");
  prefix_sums_t lhs_prefix, rhs_prefix;
  result.lhs_total = count_chains_prefix_sum(lhs_info, "旧", lhs_prefix);
  result.rhs_total = count_chains_prefix_sum(rhs_info, "新", rhs_prefix);
  printf("  旧文件总计: %zu 条链\n", result.lhs_total);
  printf("  新文件总计: %zu 条链\n", result.rhs_total);

  // 构建 rhs 模块索引：(name, index) -> sym index
  std::unordered_map<chain_module_key, size_t, chain_module_key_hash> rhs_module_map;
  for (size_t i = 0; i < rhs_info.syms.size(); ++i) {
    if (rhs_info.syms[i].sym == nullptr) continue;
    chain_module_key key(rhs_info.syms[i].sym->name, rhs_info.syms[i].sym->count);
    rhs_module_map[key] = i;
  }

  // 遍历 lhs 模块，在 rhs 中查找匹配
  printf("[对比] 匹配模块...\n");
  auto match_start = std::chrono::steady_clock::now();
  size_t total_common = 0;
  size_t module_count = lhs_info.syms.size();

  for (size_t idx = 0; idx < lhs_info.syms.size(); ++idx) {
    auto &lhs_sym = lhs_info.syms[idx];
    if (lhs_sym.sym == nullptr) continue;

    chain_module_key key(lhs_sym.sym->name, lhs_sym.sym->count);
    auto it = rhs_module_map.find(key);
    if (it == rhs_module_map.end()) continue;

    size_t rhs_idx = it->second;

    // 用前缀和计算该模块的链数量
    int lhs_level = lhs_sym.sym->level;
    int rhs_level = rhs_info.syms[rhs_idx].sym->level;
    uint64_t lhs_chains = 0;
    for (size_t i = 0; i < lhs_sym.data.size(); ++i) {
      lhs_chains += chains_of(lhs_prefix, lhs_sym.data[i], lhs_level);
    }
    uint64_t rhs_chains = 0;
    for (size_t i = 0; i < rhs_info.syms[rhs_idx].data.size(); ++i) {
      rhs_chains += chains_of(rhs_prefix, rhs_info.syms[rhs_idx].data[i], rhs_level);
    }

    // 任一侧为 0 则跳过
    if (lhs_chains == 0 || rhs_chains == 0) continue;

    if (report != nullptr) {
      fprintf(report, "模块: %s[%d]\n", lhs_sym.sym->name, lhs_sym.sym->count);
    }

    printf("  [%zu/%zu] %s[%d] 开始匹配...\n",
           idx + 1, module_count, lhs_sym.sym->name, lhs_sym.sym->count);

    auto mod_start = std::chrono::steady_clock::now();
    size_t common_count = match_module_roots(
        lhs_info, rhs_info,
        lhs_prefix, rhs_prefix,
        lhs_sym, rhs_info.syms[rhs_idx],
        lhs_chains, rhs_chains,
        report);

    double mod_elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - mod_start).count();
    total_common += common_count;

    if (common_count > 0) {
      printf("  [%zu/%zu] %s[%d]: %zu 条匹配链, 模块耗时 %.3fs, 累计 %zu 条\n",
             idx + 1, module_count, lhs_sym.sym->name, lhs_sym.sym->count,
             common_count, mod_elapsed, total_common);
      module_chain_diff<T> diff;
      diff.module_name = lhs_sym.sym->name;
      diff.module_index = lhs_sym.sym->count;
      diff.common_count = common_count;
      result.modules.push_back(std::move(diff));
    } else {
      printf("  [%zu/%zu] %s[%d]: 无匹配, 模块耗时 %.3fs\n",
             idx + 1, module_count, lhs_sym.sym->name, lhs_sym.sym->count,
             mod_elapsed);
    }

    if (report != nullptr) {
      fprintf(report, "\n");
    }
  }

  double match_elapsed = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - match_start).count();
  printf("[对比] 匹配完成, 共 %zu 条匹配链, 匹配阶段耗时 %.3fs\n",
         total_common, match_elapsed);

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
