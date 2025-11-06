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
  parser.addOption(
      {'a', "address", "目标地址(16进制，不带0x前缀)", true, false});
  parser.addOption({'d', "depth", "最大搜索深度", true, false, "10"});
  parser.addOption({'o', "offset", "最大偏移量", true, false, "500"});
  parser.addOption({'l', "limit", "结果限制数量", true, false, "0"});
  parser.addOption(
      {'f', "file", "输出文件名", true, false, "pointer_chains.txt"});
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

  // 获取目标进程
  std::string targetProcess = parser.getOptionValue("process");
  int targetPid = -1;

  // 尝试解析进程ID
  try {
    targetPid = std::stoi(targetProcess);
  } catch (...) {
    // 不是数字，认为是进程名
    targetPid = memtool::base::get_pid(targetProcess.c_str());
    if (targetPid == -1) {
      std::cerr << "错误: 无法找到进程: " << targetProcess << std::endl;
      return 1;
    }
  }

  printf("get_pid %s : %d\n", targetProcess.c_str(), targetPid);

  uint32_t maxDepth = parser.getIntOption("depth", 5);
  uint32_t maxOffset = parser.getIntOption("offset", 500);

  memtool::base::target_pid = targetPid;
  chainer::cscan<size_t> t; // 假定为64位 32位改uint32_t

  memtool::extend::get_target_mem();

  memtool::extend::set_mem_ranges(memtool::Anonymous + memtool::C_alloc +
                                  memtool::C_bss + memtool::C_data);
  auto startTime = std::chrono::high_resolution_clock::now();
  // 获取潜在指针数据
  // 起始地址为0 结束地址为0 不限制 10个缓冲区 每个1M大小
  auto count = t.get_pointers(0, 0, false, 10, 1 << 20);

  printf("get_pointers %ld\n", count);
  auto endTime = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      endTime - startTime);
  printf("Time taken: %ld milliseconds\n", duration.count());

  uint64_t addr = std::stoull(parser.getOptionValue("address"), nullptr, 16);
  std::vector<size_t> addrs;
  addrs.emplace_back(addr);

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
  return 0;
}