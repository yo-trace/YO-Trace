// YO-Trace 阶段二：文本块结构（桩实现，阶段三才真正使用）
#pragma once
#include <string>
#include <vector>

struct TextBlock {
    std::string content;   // 识别出的文字（UTF-8）
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;  // 屏幕坐标包围盒
    double confidence = 0.0;             // 置信度 0~1（桩实现给固定值）
};