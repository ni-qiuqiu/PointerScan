#pragma once

#include "ccompare.h"

#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace chainer {

template <class T>
auto ccompare<T>::parse_file(const std::string &path) -> chain_collection {
  std::unique_ptr<FILE, decltype(&fclose)> file(fopen(path.c_str(), "rb+"),
                                                &fclose);
  if (!file) {
    throw std::runtime_error("无法打开指针链文件: " + path);
  }
  validate_bin_file(file.get(), path);
  auto info = this->parse_cprog_bin_data(file.get());

  chain_collection chains;
  for (auto &sym : info.syms) {
    collect_module_chains(info, sym, chains);
  }
  return chains;
}

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

template <class T>
void ccompare<T>::collect_module_chains(const cprog_chain_info<T> &info,
                                        const cprog_sym_integr<T> &sym,
                                        chain_collection &out) {
  if (sym.sym == nullptr || sym.data.size() == 0) {
    return;
  }

  chain_signature<T> base;
  base.module_name = sym.sym->name;
  base.module_index = sym.sym->count;

  std::vector<T> offsets;
  offsets.reserve(sym.sym->level + 1);
  for (size_t i = 0; i < sym.data.size(); ++i) {
    const auto &dir = sym.data[i];
    offsets.clear();
    offsets.emplace_back(static_cast<T>(dir.address - sym.sym->start));
    collect_from_dir(info, dir, sym.sym->level, offsets, base, out);
  }
}

template <class T>
void ccompare<T>::collect_from_dir(const cprog_chain_info<T> &info,
                                   const cprog_data<T> &dir, int level,
                                   std::vector<T> &offsets,
                                   const chain_signature<T> &base,
                                   chain_collection &out) {
  if (level == 0) {
    chain_signature<T> record;
    record.module_name = base.module_name;
    record.module_index = base.module_index;
    record.offsets = offsets;
    out.emplace_back(std::move(record));
    return;
  }

  int target_level = level - 1;
  if (target_level < 0 ||
      static_cast<size_t>(target_level) >= info.contents.size()) {
    return;
  }

  auto &child_level = info.contents[target_level];
  for (uint32_t idx = dir.start; idx < dir.end && idx < child_level.size();
       ++idx) {
    auto &child = child_level[idx];
    offsets.emplace_back(static_cast<T>(child.address - dir.value));
    collect_from_dir(info, child, target_level, offsets, base, out);
    offsets.pop_back();
  }
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
bool ccompare<T>::parse_txt_line(const std::string &line,
                                 chain_signature<T> &out) {
  std::string s = line;
  // 去掉首尾空白
  auto not_space = [](int ch) { return !std::isspace(ch); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
    s.pop_back();
  }
  if (s.empty()) {
    return false;
  }
  // 期望格式: module[index] + 0xOFFSET -> + 0xOFFSET...
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
    plus_pos += 4;  // 跳过 "+ 0x"
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
bin_compare_result<T> ccompare<T>::compare_bin_files(const std::string &lhs_path,
                                                     const std::string &rhs_path) {
  auto lhs_chains = parse_file(lhs_path);
  auto rhs_chains = parse_file(rhs_path);

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

template <class T>
bin_compare_result<T> ccompare<T>::compare_txt_files(const std::string &lhs_path,
                                                     const std::string &rhs_path) {
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

template class ccompare<uint32_t>;
template class ccompare<size_t>;

}  // namespace chainer

