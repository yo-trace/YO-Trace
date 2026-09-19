// YO-Trace 阶段三：UIA 控件树抓取实现（任务 3.5）
//
// 依赖官方 uiautomation.h（Windows SDK）。手写 COM 调用，不依赖 C++/WinRT 投影。
#include "uia_capture.h"
#include <uiautomation.h>
#include <string>

// 控件类型 id（UIA_*ControlTypeId，取自 uiautomationcore.h 的稳定常量值）-> 可读名
static std::wstring ControlTypeName(int id) {
    switch (id) {
    case 50000: return L"Button";
    case 50001: return L"Calendar";
    case 50002: return L"CheckBox";
    case 50003: return L"ComboBox";
    case 50004: return L"Edit";
    case 50005: return L"Hyperlink";
    case 50006: return L"Image";
    case 50007: return L"ListItem";
    case 50008: return L"List";
    case 50009: return L"Menu";
    case 50010: return L"MenuBar";
    case 50011: return L"MenuItem";
    case 50012: return L"ProgressBar";
    case 50013: return L"RadioButton";
    case 50014: return L"ScrollBar";
    case 50015: return L"Slider";
    case 50016: return L"Spinner";
    case 50017: return L"StatusBar";
    case 50018: return L"Tab";
    case 50019: return L"TabItem";
    case 50020: return L"Text";
    case 50021: return L"ToolBar";
    case 50022: return L"ToolTip";
    case 50023: return L"Tree";
    case 50024: return L"TreeItem";
    case 50025: return L"Custom";
    case 50026: return L"Group";
    case 50027: return L"Thumb";
    case 50028: return L"DataGrid";
    case 50029: return L"DataItem";
    case 50030: return L"Document";
    case 50031: return L"SplitButton";
    case 50032: return L"Window";
    case 50033: return L"Pane";
    case 50034: return L"Header";
    case 50035: return L"HeaderItem";
    case 50036: return L"Table";
    case 50037: return L"TitleBar";
    case 50038: return L"Separator";
    default:    return std::wstring(L"Control(") + std::to_wstring(id) + L")";
    }
}

// 递归深度与节点数量上限，防止个别控件树异常庞大导致采集卡顿
static const int kMaxDepth  = 12;
static const int kMaxNodes  = 2000;

// 递归填充一个节点及其子树
static void Populate(IUIAutomationElement* pElem, int depth, int& remaining, ControlNode& node) {
    if (depth >= kMaxDepth || remaining <= 0) return;

    int typeId = 0;
    if (SUCCEEDED(pElem->get_CurrentControlType(&typeId)))
        node.controlType = ControlTypeName(typeId);

    BSTR name = nullptr;
    if (SUCCEEDED(pElem->get_CurrentName(&name)) && name) {
        node.name = name;          // wstring 从 BSTR 拷贝构造（含长度，避免 wchar_t* 截断）
        SysFreeString(name);
    }
    BSTR aid = nullptr;
    if (SUCCEEDED(pElem->get_CurrentAutomationId(&aid)) && aid) {
        node.automationId = aid;
        SysFreeString(aid);
    }
    RECT rc = {0};
    if (SUCCEEDED(pElem->get_CurrentBoundingRectangle(&rc))) {
        node.x1 = rc.left;  node.y1 = rc.top;
        node.x2 = rc.right; node.y2 = rc.bottom;
    }

    IUIAutomationElement* pChild = nullptr;
    if (FAILED(pElem->GetFirstChildElement(&pChild)) || !pChild) return;
    while (pChild && remaining > 0 && depth < kMaxDepth) {
        ControlNode child;
        --remaining;
        Populate(pChild, depth + 1, remaining, child);
        node.children.push_back(std::move(child));

        // 注意：GetNextSiblingElement 是实例方法，作用于"当前元素"自身，
        // 返回它的下一个兄弟节点（不是父节点的兄弟）。
        IUIAutomationElement* pNext = nullptr;
        HRESULT hrNext = pChild->GetNextSiblingElement(&pNext);
        pChild->Release();
        pChild = pNext;
        if (FAILED(hrNext) || !pChild) break;
    }
    if (pChild) pChild->Release();   // 循环被 break 时兜底释放
}

bool CaptureControlTree(HWND hwnd, std::vector<ControlNode>& roots) {
    if (!hwnd) return false;

    IUIAutomation* pAuto = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IUIAutomation, (void**)&pAuto);
    if (FAILED(hr) || !pAuto) return false;

    IUIAutomationElement* pRoot = nullptr;
    hr = pAuto->ElementFromHandle(hwnd, &pRoot);
    if (FAILED(hr) || !pRoot) {
        pAuto->Release();
        return false;
    }

    ControlNode root;
    int remaining = kMaxNodes;
    Populate(pRoot, 0, remaining, root);

    pRoot->Release();
    pAuto->Release();
    roots.push_back(std::move(root));
    return true;
}
