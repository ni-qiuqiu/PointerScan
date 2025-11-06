#include "cmd_parser.h"
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace utils {

CommandLineParser::CommandLineParser(const std::string& programName, 
                                   const std::string& description)
    : programName_(programName), description_(description) {
}

void CommandLineParser::addOption(const CommandOption& option) {
    options_.push_back(option);
    
    // 添加短选项到长选项的映射
    if (option.shortName != '\0') {
        shortToLongMap_[option.shortName] = option.longName;
    }
}

bool CommandLineParser::parse(int argc, char** argv) {
    // 清除旧数据
    parsedOptions_.clear();
    positionalArgs_.clear();
    errorMessage_.clear();
    
    // 设置程序名称（如果尚未设置）
    if (programName_.empty() && argc > 0) {
        programName_ = argv[0];
        // 提取基本文件名
        size_t pos = programName_.find_last_of("/\\");
        if (pos != std::string::npos) {
            programName_ = programName_.substr(pos + 1);
        }
    }
    
    // 逐个解析参数
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        // 检查是否是选项
        if (arg.size() > 1 && arg[0] == '-') {
            // 长选项 (--option)
            if (arg.size() > 2 && arg[1] == '-') {
                std::string optName = arg.substr(2);
                std::string optValue;
                
                // 检查是否包含等号 (--option=value)
                size_t eqPos = optName.find('=');
                if (eqPos != std::string::npos) {
                    optValue = optName.substr(eqPos + 1);
                    optName = optName.substr(0, eqPos);
                }
                
                // 查找选项定义
                const CommandOption* opt = findOption(optName);
                if (!opt) {
                    errorMessage_ = "未知选项: --" + optName;
                    return false;
                }
                
                // 检查是否需要值
                if (opt->hasValue) {
                    if (eqPos != std::string::npos) {
                        // 值已从等号后面提取
                    } else if (i + 1 < argc && argv[i + 1][0] != '-') {
                        // 下一个参数是值
                        optValue = argv[++i];
                    } else if (!opt->defaultValue.empty()) {
                        // 使用默认值
                        optValue = opt->defaultValue;
                    } else {
                        errorMessage_ = "选项 --" + optName + " 需要一个值";
                        return false;
                    }
                } else if (eqPos != std::string::npos) {
                    errorMessage_ = "选项 --" + optName + " 不接受值";
                    return false;
                } else {
                    // 不需要值的选项使用"true"作为值
                    optValue = "true";
                }
                
                // 存储选项
                parsedOptions_[opt->longName] = optValue;
            }
            // 短选项 (-o)
            else {
                // 支持组合短选项 (-abc)
                for (size_t j = 1; j < arg.size(); ++j) {
                    char shortOpt = arg[j];
                    
                    // 查找短选项对应的长选项
                    auto it = shortToLongMap_.find(shortOpt);
                    if (it == shortToLongMap_.end()) {
                        errorMessage_ = "未知选项: -" + std::string(1, shortOpt);
                        return false;
                    }
                    
                    // 查找选项定义
                    const CommandOption* opt = findOption(it->second);
                    if (!opt) {
                        errorMessage_ = "内部错误: 找不到选项定义";
                        return false;
                    }
                    
                    // 检查是否需要值
                    if (opt->hasValue) {
                        // 如果是组合选项中的最后一个
                        if (j == arg.size() - 1) {
                            if (i + 1 < argc && argv[i + 1][0] != '-') {
                                // 下一个参数是值
                                parsedOptions_[opt->longName] = argv[++i];
                            } else if (!opt->defaultValue.empty()) {
                                // 使用默认值
                                parsedOptions_[opt->longName] = opt->defaultValue;
                            } else {
                                errorMessage_ = "选项 -" + std::string(1, shortOpt) + " 需要一个值";
                                return false;
                            }
                        } else {
                            // 在组合选项中间的选项不能接受值
                            errorMessage_ = "选项 -" + std::string(1, shortOpt) + " 需要一个值，但出现在组合选项中";
                            return false;
                        }
                    } else {
                        // 不需要值的选项使用"true"作为值
                        parsedOptions_[opt->longName] = "true";
                    }
                }
            }
        }
        // 位置参数
        else {
            positionalArgs_.push_back(arg);
        }
    }
    
    // 验证必需选项
    if (!validateRequired()) {
        return false;
    }
    
    return true;
}

bool CommandLineParser::hasOption(const std::string& name) const {
    return parsedOptions_.find(name) != parsedOptions_.end();
}

std::string CommandLineParser::getOptionValue(const std::string& name, const std::string& defaultValue) const {
    auto it = parsedOptions_.find(name);
    if (it != parsedOptions_.end()) {
        return it->second;
    }
    
    // 如果没有指定该选项，返回默认值
    return defaultValue;
}

int CommandLineParser::getIntOption(const std::string& name, int defaultValue) const {
    return getOptionAs<int>(name, defaultValue);
}

double CommandLineParser::getDoubleOption(const std::string& name, double defaultValue) const {
    return getOptionAs<double>(name, defaultValue);
}

bool CommandLineParser::getBoolOption(const std::string& name, bool defaultValue) const {
    if (!hasOption(name)) {
        return defaultValue;
    }
    
    std::string value = getOptionValue(name);
    std::transform(value.begin(), value.end(), value.begin(), ::tolower);
    
    return value == "true" || value == "yes" || value == "1";
}

std::string CommandLineParser::getPositionalArg(size_t index) const {
    if (index < positionalArgs_.size()) {
        return positionalArgs_[index];
    }
    return "";
}

size_t CommandLineParser::getPositionalArgCount() const {
    return positionalArgs_.size();
}

bool CommandLineParser::validateRequired()  {
    for (const auto& opt : options_) {
        if (opt.required && !hasOption(opt.longName)) {
            std::string message;
            message =  "缺少必需选项: " + (opt.shortName != '\0' ? std::string("-") + opt.shortName + ", " : "") + "--" + opt.longName;
            errorMessage_ = message;
            return false;
        }
    }
    return true;
}

void CommandLineParser::showHelp() const {
    std::cout << "用法: " << programName_;
    
    if (!usage_.empty()) {
        std::cout << " " << usage_;
    } else {
        std::cout << " [选项]";
        if (!options_.empty()) {
            for (const auto& opt : options_) {
                if (opt.required) {
                    std::cout << " --" << opt.longName;
                    if (opt.hasValue) {
                        std::cout << "=值";
                    }
                }
            }
        }
    }
    
    std::cout << std::endl << std::endl;
    
    if (!description_.empty()) {
        std::cout << description_ << std::endl << std::endl;
    }
    
    if (!options_.empty()) {
        std::cout << "选项:" << std::endl;
        
        // 计算最长的选项名，用于对齐
        size_t maxOptWidth = 0;
        for (const auto& opt : options_) {
            size_t width = opt.longName.length();
            if (opt.hasValue) {
                width += 5; // 为 "=值" 添加空间
            }
            maxOptWidth = std::max(maxOptWidth, width);
        }
        
        // 显示每个选项
        for (const auto& opt : options_) {
            std::cout << "  ";
            
            if (opt.shortName != '\0') {
                std::cout << "-" << opt.shortName;
                if (opt.hasValue) {
                    std::cout << " 值";
                }
                std::cout << ", ";
            } else {
                std::cout << "    ";
            }
            
            std::cout << "--" << opt.longName;
            if (opt.hasValue) {
                std::cout << "=值";
            }
            
            // 对齐描述
            size_t padding = maxOptWidth - opt.longName.length() - (opt.hasValue ? 5 : 0);
            std::cout << std::string(padding + 2, ' ');
            
            // 显示描述
            std::cout << opt.description;
            
            // 如果有默认值，显示它
            if (opt.hasValue && !opt.defaultValue.empty()) {
                std::cout << " (默认: " << opt.defaultValue << ")";
            }
            
            // 如果是必需选项，指出它
            if (opt.required) {
                std::cout << " [必需]";
            }
            
            std::cout << std::endl;
        }
    }
}

void CommandLineParser::showVersion(const std::string& version) const {
    std::cout << programName_ << " 版本 " << version << std::endl;
}

void CommandLineParser::setUsage(const std::string& usage) {
    usage_ = usage;
}

std::string CommandLineParser::getErrorMessage() const {
    return errorMessage_;
}

const CommandOption* CommandLineParser::findOption(const std::string& name) const {
    for (const auto& opt : options_) {
        if (opt.longName == name) {
            return &opt;
        }
    }
    return nullptr;
}

const CommandOption* CommandLineParser::findOptionByShort(char shortName) const {
    for (const auto& opt : options_) {
        if (opt.shortName == shortName) {
            return &opt;
        }
    }
    return nullptr;
}

} // namespace utils
