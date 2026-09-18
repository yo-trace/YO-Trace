// YO-Trace 阶段三：OCR 引擎接口（任务 3.2/3.4）
#pragma once
#include <windows.h>
#include <vector>
#include <string>
#include "text_block.h"

// 传给 OCR 的窗口线索：桩实现用它生成文本块；
// 真实引擎（Windows.Media.Ocr，C++/WinRT，需 MSVC）会用 hbmp 实际识别。
struct ScreenWindow {
    std::wstring title;
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
};

class IOcrEngine {
public:
    virtual ~IOcrEngine() = default;

    // hbmp: 屏幕内存位图；wins: 当前窗口列表；out: 识别出的文本块
    // 返回是否成功（out 内容是否有效由调用方判断）
    virtual bool recognize(HBITMAP hbmp,
                           const std::vector<ScreenWindow>& wins,
                           std::vector<TextBlock>& out) = 0;

    virtual const char* name() const = 0;
};

// 工厂：优先 Windows 原生 OCR；不可用时回退到 Tesseract（见 ocr_windows.cpp）
IOcrEngine* CreateOcrEngine();

// 回退工厂：直接返回 Tesseract 子进程引擎（由 ocr_tesseract.cpp 实现）
IOcrEngine* CreateTesseractOcrEngine();
