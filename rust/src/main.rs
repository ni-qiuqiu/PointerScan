//! BFS 指针链扫描器 - 命令行入口
//!
//! 用法:
//!   scanner <pid|process_name> <target_address> [options]
//!
//! 示例:
//!   scanner 1234 0x7f8a4c000000 -d 5 -o 0x1000
//!   scanner com.example.app 0x12345678 --depth 3

use std::fs::File;

use pointer_chain_scanner::{ChainScanner, Result, ScanError};
use pointer_chain_scanner::memory::MemRange;
use pointer_chain_scanner::process::ReadMode;

fn main() -> Result<()> {
    let args: Vec<String> = std::env::args().collect();
    
    if args.len() < 3 {
        print_usage(&args[0]);
        return Ok(());
    }

    // 解析参数
    let target = &args[1];
    let address_str = &args[2];
    
    let mut depth = 5usize;
    let mut offset = 0x1000u64;
    let mut output_path = String::from("chains.bin");
    let mut text_output = false;
    let mut use_io = false;
    // 原项目默认: Anonymous + C_alloc + C_bss + C_data
    let mut scan_ranges = MemRange::Anonymous as i32 
                        | MemRange::CAlloc as i32 
                        | MemRange::CBss as i32
                        | MemRange::CData as i32;

    // 解析可选参数
    let mut i = 3;
    while i < args.len() {
        match args[i].as_str() {
            "-d" | "--depth" => {
                if i + 1 < args.len() {
                    depth = args[i + 1].parse().unwrap_or(5);
                    i += 1;
                }
            }
            "-o" | "--offset" => {
                if i + 1 < args.len() {
                    offset = parse_hex_or_dec(&args[i + 1]);
                    i += 1;
                }
            }
            "-f" | "--file" => {
                if i + 1 < args.len() {
                    output_path = args[i + 1].clone();
                    i += 1;
                }
            }
            "-t" | "--text" => {
                text_output = true;
            }
            "--io" => {
                use_io = true;
            }
            "-r" | "--ranges" => {
                if i + 1 < args.len() {
                    scan_ranges = parse_ranges(&args[i + 1]);
                    i += 1;
                }
            }
            "-h" | "--help" => {
                print_usage(&args[0]);
                return Ok(());
            }
            _ => {}
        }
        i += 1;
    }

    // 解析目标地址
    let target_address = parse_hex_or_dec(address_str);
    if target_address == 0 {
        eprintln!("Error: Invalid target address: {}", address_str);
        return Err(ScanError::InvalidChain);
    }

    // 创建扫描器
    let read_mode = if use_io {
        println!("使用 /proc/pid/mem IO 读取模式");
        ReadMode::ProcMemIo
    } else {
        ReadMode::ProcessVmReadv
    };

    println!("Initializing scanner...");
    let mut scanner = if let Ok(pid) = target.parse::<i32>() {
        ChainScanner::with_mode(pid, read_mode)?
    } else {
        ChainScanner::from_name_with_mode(target, read_mode)?
    };

    println!("Target: {}", target);
    println!("Address: 0x{:x}", target_address);
    println!("Depth: {}", depth);
    println!("Offset: 0x{:x}", offset);
    println!("Output: {}", output_path);
    println!();

    // 获取指针数据
    println!("Scanning for pointers...");
    let pointer_count = scanner.get_pointers(
        0x10000,           // 最小有效地址
        0x7fffffffffff,    // 最大有效地址（用户空间）
        scan_ranges,
        1024 * 1024,       // 1MB 块大小
    )?;
    println!("Found {} potential pointers\n", pointer_count);

    // 执行指针链扫描
    let targets = vec![target_address];
    
    let chain_count = if text_output {
        // 直接输出文本格式
        let txt_path = if output_path.ends_with(".bin") {
            output_path.replace(".bin", ".txt")
        } else {
            format!("{}.txt", output_path)
        };
        let mut output = File::create(&txt_path).map_err(ScanError::Io)?;
        
        println!("Output (text): {}", txt_path);
        
        scanner.scan_pointer_chain_to_txt(
            &targets,
            depth,
            offset,
            &mut output,
        )?
    } else {
        // 输出二进制格式
        let mut output = File::create(&output_path).map_err(ScanError::Io)?;
        
        scanner.scan_pointer_chain(
            &targets,
            depth,
            offset,
            &mut output,
        )?
    };

    println!("\nTotal chains found: {}", chain_count);

    Ok(())
}

fn print_usage(program: &str) {
    println!("BFS Pointer Chain Scanner - Rust ARM64 Implementation");
    println!();
    println!("Usage: {} <pid|process_name> <target_address> [options]", program);
    println!();
    println!("Arguments:");
    println!("  <pid|process_name>  Target process PID or name");
    println!("  <target_address>    Target address to scan (hex or decimal)");
    println!();
    println!("Options:");
    println!("  -d, --depth <N>     Scan depth (default: 5)");
    println!("  -o, --offset <N>    Max offset (default: 0x1000)");
    println!("  -f, --file <path>   Output file path (default: chains.bin)");
    println!("  -t, --text          Also output text format");
    println!("  --io                Use /proc/pid/mem IO read (fix unreadable memory)");
    println!("  -r, --ranges <N>    Memory ranges to scan (bitmask)");
    println!("  -h, --help          Show this help message");
    println!();
    println!("Memory Ranges:");
    println!("  1  - Anonymous");
    println!("  2  - C Heap");
    println!("  4  - C Alloc");
    println!("  8  - Code App");
    println!("  16 - Code System");
    println!("  32 - C BSS");
    println!("  64 - C Data");
    println!();
    println!("Examples:");
    println!("  {} 1234 0x7f8a4c000000 -d 5 -o 0x1000", program);
    println!("  {} com.example.app 0x12345678 --depth 3", program);
}

fn parse_hex_or_dec(s: &str) -> u64 {
    if s.starts_with("0x") || s.starts_with("0X") {
        u64::from_str_radix(&s[2..], 16).unwrap_or(0)
    } else {
        s.parse().unwrap_or(0)
    }
}

fn parse_ranges(s: &str) -> i32 {
    if s.starts_with("0x") || s.starts_with("0X") {
        i32::from_str_radix(&s[2..], 16).unwrap_or(7)
    } else {
        s.parse().unwrap_or(7)
    }
}
