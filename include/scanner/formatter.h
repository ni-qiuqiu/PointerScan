#pragma once

#include "scanner/pointer_chain.h"
#include <string>
#include <memory>
#include <fstream>
#include <iomanip>

namespace memchainer {

class PointerFormatter {
public:
    PointerFormatter();
    ~PointerFormatter();
    
    // 格式化为控制台输出
    void formatToConsole(const std::shared_ptr<PointerChain>& chain, size_t maxChains = 0);
    
    // 格式化为文本文件
    bool formatToTextFile(const std::shared_ptr<PointerChain>& chain, const std::string& filename);

    // 设置输出格式
    void setFormat(const std::string& format) { format_ = format; }

    // 设置是否显示详细信息
    void setShowDetails(bool show) { showDetails_ = show; }

    // 设置是否显示静态偏移
    void setShowStaticOffset(bool show) { showStaticOffset_ = show; }

private:
    // 格式化单个指针节点
    std::string formatNode(const PointerChainNode& node) const;

    // 格式化静态偏移信息
    std::string formatStaticOffset(const StaticOffset& staticOffset) const;

    // 格式化内存区域信息
    std::string formatRegion(const MemoryRegion* region) const;

    // 输出格式
    std::string format_ = "hex"; // 可选: "hex", "dec", "both"

    // 是否显示详细信息
    bool showDetails_ = true;

    // 是否显示静态偏移
    bool showStaticOffset_ = true;
};

} // namespace memchainer
