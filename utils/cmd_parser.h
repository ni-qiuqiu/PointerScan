#pragma once

#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <functional>
#include <sstream>
#include <iostream>
#include <iomanip>

namespace utils {

// 命令行参数选项
struct CommandOption {
    char shortName;           // 短选项名 (例如 'h' 表示 -h)
    std::string longName;     // 长选项名 (例如 "help" 表示 --help)
    std::string description;  // 选项描述
    bool hasValue;            // 是否需要值
    bool required;            // 是否必需
    std::string defaultValue; // 默认值
    
    CommandOption(char s, const std::string& l, const std::string& d, 
                 bool v = false, bool r = false, const std::string& def = "")
        : shortName(s), longName(l), description(d), 
          hasValue(v), required(r), defaultValue(def) {}
};

// 命令行解析器类
class CommandLineParser {
public:
    CommandLineParser(const std::string& programName = "", 
                    const std::string& description = "");
    
    // 添加选项定义
    void addOption(const CommandOption& option);
    
    // 解析命令行参数
    bool parse(int argc, char** argv);
    
    // 检查选项是否存在
    bool hasOption(const std::string& name) const;
    
    // 获取选项值
    std::string getOptionValue(const std::string& name, const std::string& defaultValue = "") const;
    
    // 类型安全的获取选项值
    template<typename T>
    T getOptionAs(const std::string& name, const T& defaultValue = T()) const {
        if (!hasOption(name)) {
            return defaultValue;
        }
        
        T value = defaultValue;
        std::istringstream iss(getOptionValue(name));
        iss >> value;
        return value;
    }
    
    // 获取特定类型的值
    int getIntOption(const std::string& name, int defaultValue = 0) const;
    double getDoubleOption(const std::string& name, double defaultValue = 0.0) const;
    bool getBoolOption(const std::string& name, bool defaultValue = false) const;
    
    // 获取位置参数
    std::string getPositionalArg(size_t index) const;
    size_t getPositionalArgCount() const;
    
    // 检查是否所有必需选项都提供了
    bool validateRequired() ;
    
    // 显示帮助信息
    void showHelp() const;
    
    // 显示版本信息
    void showVersion(const std::string& version) const;
    
    // 设置使用示例
    void setUsage(const std::string& usage);
    
    // 获取错误信息
    std::string getErrorMessage() const;

private:
    std::string programName_;
    std::string description_;
    std::string usage_;
    std::string errorMessage_;
    
    std::vector<CommandOption> options_;
    std::unordered_map<std::string, std::string> parsedOptions_;
    std::unordered_map<char, std::string> shortToLongMap_;
    std::vector<std::string> positionalArgs_;
    
    // 查找选项
    const CommandOption* findOption(const std::string& name) const;
    const CommandOption* findOptionByShort(char shortName) const;
};

} // namespace utils
