// YO-Trace 阶段三：UIA 控件树节点定义
#pragma once
#include <string>
#include <vector>

struct ControlNode {
    long long windowId = 0;     // 所属窗口 id（查询时回填；采集时为 0）
    std::wstring controlType;   // 可读名，如 "Button" / "Edit" / "Window"
    std::wstring name;          // UIA_NamePropertyId
    std::wstring automationId;  // UIA_AutomationIdPropertyId
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;  // BoundingRectangle（屏幕坐标）
    std::vector<ControlNode> children;
};
