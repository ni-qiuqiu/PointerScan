#include "memtool/membase.hpp"

#include "memtool/memextend.hpp"

#include "chainer/ccformat.hpp"
#include "chainer/ccscan.hpp"


#include "utils/cmd_parser.h"
#include <cstdint>

using namespace utils;

int main(int argc, char *argv[]) {
  // 创建命令行解析器
  CommandLineParser parser("newscan", "高性能内存指针链分析工具");

  // 添加命令行选项
  parser.addOption({'p', "process", "目标进程名称或PID", true, true});
  parser.addOption({'a', "address", "目标地址(16进制，不带0x前缀)", true, false});
  parser.addOption({'d', "depth", "最大搜索深度", true, false, "10"});
  parser.addOption({'o', "offset", "最大偏移量", true, false, "500"});
  parser.addOption({'l', "limit", "结果限制数量", true, false, "0"});
  parser.addOption({'f', "file", "输出文件名", true, false, "pointer_chains.txt"});
  parser.addOption({'v', "verbose", "详细输出模式", false, false});
  parser.addOption({'h', "help", "显示帮助信息", false, false});

  // 设置用法说明
  parser.setUsage("[选项] -p <进程名/PID> [-a <地址>]");

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