#include "memtool/membase.hpp"

#include "memtool/memextend.hpp"

#include "chainer/ccscan.hpp"  // IWYU pragma: keep
#include "chainer/ccompare.hpp"  // IWYU pragma: keep

#include "utils/cmd_parser.h"
#include <cstdint>
#include <cstdio>
#include <sstream>

using namespace utils;

namespace {

std::string format_chain_line(const std::string &module_name, int module_index,
                              const std::vector<size_t> &offsets) {
  std::ostringstream oss;
  oss << module_name << "[" << module_index << "]";
  if (!offsets.empty()) {
    oss << std::hex << std::uppercase;
    for (size_t i = 0; i < offsets.size(); ++i) {
      oss << (i == 0 ? " + 0x" : " -> + 0x") << offsets[i];
    }
    oss << std::nouppercase << std::dec;
  }
  return oss.str();
}

}  // namespace

int main(int argc, char *argv[]) {
  // 创建命令行解析器
  CommandLineParser parser("newscan", "高性能内存指针链分析工具");

  // 添加命令行选项
  parser.addOption({'p', "process", "目标进程名称或PID", true, false});
  parser.addOption({'a', "address", "目标地址(16进制，不带0x前缀)", true, false});
  parser.addOption({'d', "depth", "最大搜索深度", true, false, "10"});
  parser.addOption({'o', "offset", "最大偏移量(10进制)", true, false, "500"});
  parser.addOption({'l', "limit", "结果限制数量", true, false, "0"});
  parser.addOption({'f', "file", "输出文件名", true, false, "pointer_chains.txt"});
  parser.addOption({0, "compare-bin", "比较两份指针链二进制文件", false, false});
  parser.addOption({0, "compare-txt", "比较两份指针链文本文件", false, false});
  parser.addOption({0, "lhs", "旧版指针链二进制文件路径", true, false});
  parser.addOption({0, "rhs", "新版指针链二进制文件路径", true, false});
  parser.addOption({0, "report", "对比输出文件名", true, false,
                    "chain_compare.txt"});
  parser.addOption({'v', "verbose", "详细输出模式", false, false});
  parser.addOption({'h', "help", "显示帮助信息", false, false});

  // 设置用法说明
  parser.setUsage("[扫描] -p <进程名/PID> -a <地址> | "
                  "[对比] (--compare-bin|--compare-txt) --lhs <旧文件> --rhs <新文件>");

  // 解析命令行参数
  if (!parser.parse(argc, argv)) {
    std::cerr << "错误: " << parser.getErrorMessage() << std::endl;
    parser.showHelp();
    return 1;
  }

  // 如果请求帮助，显示帮助信息并退出
  if (parser.hasOption("help")) {
    parser.showHelp();
    return 0;
  }

  bool compare_bin_mode = parser.getBoolOption("compare-bin", false);
  bool compare_txt_mode = parser.getBoolOption("compare-txt", false);
  if (compare_bin_mode || compare_txt_mode) {
    if (!parser.hasOption("lhs") || !parser.hasOption("rhs")) {
      std::cerr << "错误: 对比模式需要提供 --lhs 与 --rhs 选项" << std::endl;
      return 1;
    }

    auto lhs_path = parser.getOptionValue("lhs");
    auto rhs_path = parser.getOptionValue("rhs");

    std::string report_path =
        parser.getOptionValue("report", "chain_compare.txt");
    FILE *report = fopen(report_path.c_str(), "w");
    if (report == nullptr) {
      std::cerr << "警告: 无法创建报告文件: " << report_path << std::endl;
    }

    chainer::ccompare<size_t> comparer;
    chainer::bin_compare_result<size_t> compare_result;

    try {
      if (compare_bin_mode) {
        // 二进制对比：匹配链直接流式写入 report，不存储在内存中
        if (report != nullptr) {
          fprintf(report, "=== 指针链二进制文件对比结果 ===\n\n");
        }
        compare_result = comparer.compare_bin_files(lhs_path, rhs_path, report);
      } else {
        compare_result = comparer.compare_txt_files(lhs_path, rhs_path);
      }
    } catch (const std::exception &ex) {
      if (report != nullptr) fclose(report);
      std::cerr << "错误: " << ex.what() << std::endl;
      return 1;
    }

    // 写入统计摘要
    if (report != nullptr) {
      if (compare_txt_mode) {
        fprintf(report, "=== 指针链文本文件对比结果 ===\n\n");
      }

      // 二进制模式下链详情已在比较过程中写入，这里追加统计
      fprintf(report, "--- 统计 ---\n");
      fprintf(report, "旧文件链数量: %zu\n", compare_result.lhs_total);
      fprintf(report, "新文件链数量: %zu\n", compare_result.rhs_total);
      fprintf(report, "保持不变链数量: %zu\n", compare_result.unchanged);
      fprintf(report, "\n");

      if (compare_txt_mode) {
        // 文本对比：链存储在内存中，在此输出
        if (compare_result.modules.empty()) {
          fprintf(report, "未找到共同存在的指针链。\n");
        } else {
          for (const auto &diff : compare_result.modules) {
            fprintf(report, "模块: %s[%d]\n", diff.module_name.c_str(),
                    diff.module_index);
            if (!diff.common.empty()) {
              fprintf(report, "  保持不变的链:\n");
              for (const auto &chain : diff.common) {
                fprintf(report, "    = %s\n",
                        format_chain_line(diff.module_name, diff.module_index,
                                          chain).c_str());
              }
            }
            fprintf(report, "\n");
          }
        }
      }

      fclose(report);
      printf("对比报告已保存至: %s\n", report_path.c_str());
    }

    return 0;
  }

  if (!parser.hasOption("process")) {
    std::cerr << "错误: 扫描模式需要提供目标进程 (-p/--process)" << std::endl;
    return 1;
  }
  if (!parser.hasOption("address")) {
    std::cerr << "错误: 扫描模式需要提供目标地址 (-a/--address)" << std::endl;
    return 1;
  }

  // 第一步：获取目标进程
  std::string target_process = parser.getOptionValue("process");
  int target_pid = -1;

  // 尝试解析进程ID
  try {
    target_pid = std::stoi(target_process);
  } catch (...) {
    // 不是数字，认为是进程名
    target_pid = memtool::base::get_pid(target_process.c_str());
    if (target_pid == -1) {
      std::cerr << "错误: 无法找到进程: " << target_process << std::endl;
      return 1;
    }
  }

  printf("Target PID: %s -> %d\n", target_process.c_str(), target_pid);

  // 获取命令行参数
  uint32_t max_depth = parser.getIntOption("depth", 5);
  uint32_t max_offset = parser.getIntOption("offset", 500);

  // 第二步：初始化扫描器
  memtool::base::target_pid = target_pid;
  chainer::cscan<size_t> scanner;  // 64位进程，32位使用 uint32_t

  // 第三步：获取目标进程内存布局
  memtool::extend::get_target_mem();
  memtool::extend::set_mem_ranges(memtool::Anonymous + memtool::C_alloc +
                                  memtool::C_bss + memtool::C_data);

  // 第四步：扫描潜在指针
  auto start_time = std::chrono::high_resolution_clock::now();
  // 参数：起始地址=0, 结束地址=0(不限制), 全扫描=false, 缓冲区数=10, 缓冲区大小=1MB
  size_t pointer_count = scanner.get_pointers(0, 0, false, 10, 1 << 20);
  printf("Found %ld potential pointers\n", pointer_count);

  auto end_time = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      end_time - start_time);
  printf("Scanning time: %lld milliseconds\n", static_cast<long long>(duration.count()));

  // 第五步：构建指针链
  uint64_t target_addr = std::stoull(parser.getOptionValue("address"), nullptr, 16);
  std::vector<size_t> target_addrs;
  target_addrs.emplace_back(target_addr);

  // 直接输出文本格式（推荐方式，避免中间二进制文件转换）
  std::string output_file = parser.getOptionValue("file", "pointer_chains.txt");
  FILE *output = fopen(output_file.c_str(), "w+");
  if (output == nullptr) {
    std::cerr << "错误: 无法创建输出文件: " << output_file << std::endl;
    return 1;
  }

  size_t chain_count = scanner.scan_pointer_chain_to_txt(
      target_addrs, max_depth, max_offset, false, 0, output);
  printf("Total pointer chains found: %ld\n", chain_count);
  fclose(output);

  /* 方式2: 原始二进制格式（需要二次转换）
  auto f = fopen("1", "wb+");
  auto chaincount =
      t.scan_pointer_chain(addrs, maxDepth, maxOffset, false, 0, f);
  printf("chaincount %ld\n", chaincount); // 10层 偏移500
  fclose(f);
  // 格式化输出
  chainer::cformat<size_t> t2;
  auto f2 = fopen("1", "rb+");

  printf("%ld\n", t2.format_bin_chain_data(f2, "2", false)); // 文件
  // printf("%ld\n", t2.format_bin_chain_data(f2, "2", true)); // 文件夹
  // 需要在当前目录有2文件夹

  fclose(f2);
  */
  
  return 0;
}