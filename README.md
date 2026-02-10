# PointerScan (newscan)

高性能 Android 进程内存指针链分析工具。通过扫描目标进程内存，找到指向目标地址的指针链（pointer chains），并支持在不同版本之间对比指针链的变化。

## 功能特性

- 多线程扫描目标进程内存，收集潜在指针
- DFS 反向构建指针链，支持自定义搜索深度和偏移量
- 基于 mmap 的文件后端队列，避免大规模扫描时 OOM
- 指针链对比：支持二进制和文本两种格式的对比
- 模板化设计，同时支持 64 位（ARM64）和 32 位进程

## 环境要求

- Android NDK r25c（默认使用 OLLVM 版本）
- CMake 3.10+
- C++17
- 目标平台：Android ARM64-v8a

## 构建

```bash
# 配置
cmake -B build -S .

# 编译
cmake --build build
```

输出二进制位于 `outputs/newscan`。

> NDK 路径硬编码在 `CMakeLists.txt` 第 13 行，默认为 `D:/ndkollvm/android-ndk-r25c.Ollvm/android-ndk-r25c`，如需修改请编辑该行。

## 使用方式

### 扫描模式

扫描目标进程内存，构建指向指定地址的指针链：

```bash
newscan -p <进程名|PID> -a <目标地址hex> [-d 深度] [-o 最大偏移] [-f 输出文件]
```

示例：

```bash
# 扫描进程 com.example.app 中指向地址 7A1B2C3D00 的指针链
newscan -p com.example.app -a 7A1B2C3D00 -d 8 -o 1000 -f result.txt
```

### 二进制文件对比

对比两份指针链二进制文件，找出保持不变的链：

```bash
newscan --compare-bin --lhs <旧文件> --rhs <新文件> [--report 输出文件]
```

### 文本文件对比

对比两份指针链文本文件：

```bash
newscan --compare-txt --lhs <旧文件> --rhs <新文件> [--report 输出文件]
```

## 命令行参数

| 参数 | 短选项 | 说明 | 默认值 |
|------|--------|------|--------|
| `--process` | `-p` | 目标进程名称或 PID | — |
| `--address` | `-a` | 目标地址（16 进制，不带 0x 前缀） | — |
| `--depth` | `-d` | 最大搜索深度 | 10 |
| `--offset` | `-o` | 最大偏移量（16 进制，不带 0x 前缀） | 500 |
| `--limit` | `-l` | 结果限制数量（0 为不限制） | 0 |
| `--file` | `-f` | 输出文件名 | `pointer_chains.txt` |
| `--compare-bin` | — | 比较两份指针链二进制文件 | — |
| `--compare-txt` | — | 比较两份指针链文本文件 | — |
| `--lhs` | — | 旧版指针链文件路径 | — |
| `--rhs` | — | 新版指针链文件路径 | — |
| `--report` | — | 对比输出文件名 | `chain_compare.txt` |
| `--verbose` | `-v` | 详细输出模式 | — |
| `--help` | `-h` | 显示帮助信息 | — |

## 项目架构

```
├── main.cpp              # 入口，命令行解析与流程控制
├── chainer/              # 指针链扫描与对比逻辑
│   ├── cbase.h/hpp       # 基础数据结构（pointer_data、二进制格式头等）
│   ├── cscan.h/hpp       # 高层扫描接口：收集指针 + DFS 构建指针链
│   ├── csearch.h/hpp     # 底层指针搜索：从内存区域中筛选有效指针
│   ├── ccompare.h/hpp    # 指针链对比：基于树的直接匹配算法
│   ├── ccscan.h/hpp      # 扫描操作封装层
│   └── cformat.hpp       # 二进制指针链数据的格式化输出
├── memtool/              # 进程内存访问
│   ├── membase.hpp       # process_vm_readv 系统调用封装
│   ├── memextend.hpp/cpp # 解析 /proc/[pid]/maps，过滤内存区域
│   └── memsetting.h      # 内存区域类型枚举
├── threadpool/           # 基于 std::future 的线程池
├── utils/                # 工具组件
│   ├── BufferPool.hpp    # 预分配缓冲池
│   ├── mapqueue.h/hpp    # mmap 文件后端队列
│   ├── varray.h/hpp      # 零拷贝 mmap 视图包装器
│   ├── cmd_parser.h/cpp  # 命令行参数解析
│   └── sutils.h/hpp      # 通用工具函数
└── CMakeLists.txt
```

### 数据流

1. `main.cpp` 解析命令行参数，初始化扫描器
2. `memtool::extend::get_target_mem()` 解析目标进程内存布局（`/proc/[pid]/maps`）
3. `cscan::get_pointers()` 多线程扫描内存区域，收集潜在指针存入 mapqueue
4. `cscan::scan_pointer_chain()` 用 DFS 从目标地址反向构建指针链
5. 结果输出为文本或二进制格式

### 模板设计

核心类（`cscan`、`csearch`、`ccompare` 等）均为模板类，模板参数 `T` 控制指针宽度：

- `size_t`（64 位）— 用于 ARM64 进程
- `uint32_t`（32 位）— 用于 32 位进程

## 输出格式

### 文本格式

每行一条指针链，格式为：

```
模块名[索引] + 0x偏移1 -> + 0x偏移2 -> + 0x偏移3
```

示例：

```
libunity.so[0] + 0x1A2B3C -> + 0x48 -> + 0x10
```

表示从 `libunity.so` 基址加偏移 `0x1A2B3C` 处读取指针，再加偏移 `0x48` 读取，再加偏移 `0x10` 即为目标地址。

### 二进制格式

二进制文件包含文件头（`cprog_header`）、模块符号表（`cprog_sym`）和各层级指针数据（`cprog_data`），适用于大规模数据存储和后续程序化对比。
