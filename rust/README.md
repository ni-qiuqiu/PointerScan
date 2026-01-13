# BFS 指针链扫描器 - Rust ARM64 实现

基于 tmpfile + mmap + 零拷贝内存管理的 BFS 指针链扫描器，专为 Android ARM64 设计。

## 核心特性

- **MapQueue**: 基于 tmpfile + mmap 的动态数组，避免 BFS 内存爆炸
- **BufferPool**: 缓冲区池管理，复用内存块减少分配开销
- **零拷贝**: 使用 mmap 直接映射文件，避免数据拷贝
- **多线程**: 使用 rayon 并行扫描内存区域

## 编译

默认目标为 `aarch64-linux-android`：

```bash
# 需要先安装 target
rustup target add aarch64-linux-android

# 编译 (默认 Android ARM64)
cargo build --release

# 本地测试编译 (Linux x86_64)
cargo build --target x86_64-unknown-linux-gnu
```

### NDK 配置

编辑 `.cargo/config.toml` 设置 NDK linker 路径：

```toml
[target.aarch64-linux-android]
linker = "/path/to/ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang"
```

## 使用

```bash
# 通过 PID 扫描
./scanner 1234 0x7f8a4c000000 -d 5 -o 0x1000

# 通过进程名扫描
./scanner com.example.app 0x12345678 --depth 3

# 指定输出文件
./scanner 1234 0x12345678 -f output.bin
```

## 架构设计

```
┌─────────────────────────────────────────────────────────────┐
│                    BFS 指针链扫描流程                         │
├─────────────────────────────────────────────────────────────┤
│  Level 0:  [目标地址]                                        │
│                │                                             │
│                ▼                                             │
│  Level 1:  dirs[0] ──► MapQueue<PointerDir> ──► tmpfile+mmap │
│                │                                             │
│                ▼                                             │
│  Level N:  dirs[N] ──► MapQueue<PointerDir> ──► tmpfile+mmap │
└─────────────────────────────────────────────────────────────┘
```

## 模块说明

| 模块 | 说明 |
|------|------|
| `mapqueue` | 基于 mmap 的动态数组，核心数据结构 |
| `buffer_pool` | 缓冲区池，复用内存块 |
| `pointer` | 指针数据结构定义 |
| `memory` | 内存区域数据结构 |
| `process` | 进程内存读取 (process_vm_readv) |
| `scanner` | BFS 扫描核心逻辑 |

## MapQueue 工作原理

```rust
// 1. 创建临时文件
let file = tempfile()?;
file.set_len(size)?;

// 2. mmap 映射
let mmap = MmapMut::map_mut(&file)?;

// 3. 直接操作映射内存
// - 按需分页加载
// - 内存压力时自动换出到文件
// - 进程结束自动清理
```

## 优势

1. **避免 OOM**: 数据可换出到磁盘
2. **零拷贝**: 直接操作映射内存
3. **自动清理**: tmpfile 进程结束自动删除
4. **按需加载**: 只有访问的页面才加载到物理内存
