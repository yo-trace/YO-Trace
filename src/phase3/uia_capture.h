// YO-Trace 阶段三：UIA 控件树抓取（任务 3.5）
//
// 使用 Windows 官方 UI Automation 客户端 API（uiautomation.h，随 Windows SDK / MSVC 提供）。
// 通过 CUIAutomation 取窗口根元素，递归遍历其控件树，记录控件类型、名称、AutomationId 与
// 屏幕坐标，供"屏幕记忆"按控件名/Id 检索（例如"会议"→ 找到腾讯会议主窗口里的"加入会议"按钮）。
//
// 注意：本文件依赖官方 UIA 头文件与运行时，需在 MSVC + Windows 10/11 SDK 下编译/运行。
//       Dev-Cpp(MinGW) 不含这些头文件，无法在最小工具链沙箱中编译。
#pragma once
#include <windows.h>
#include <vector>
#include "control_tree.h"

// 抓取某个窗口的 UIA 控件树，根节点代表该窗口自身。
// 调用方须处于 STA 单元（CoInitializeEx(COINIT_APARTMENTTHREADED)）。
// 成功返回 true；roots 至少含 1 个代表该窗口的根节点。
bool CaptureControlTree(HWND hwnd, std::vector<ControlNode>& roots);
