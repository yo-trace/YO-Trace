// YO-Trace 阶段三：屏幕内存截取（任务 3.3）
#pragma once
#include <windows.h>

struct ScreenBitmap {
    HBITMAP hbmp = NULL;   // 内存位图，不落盘
    int width = 0, height = 0;
};

// 截取主屏全屏到内存位图（BitBlt + 兼容 DC，纯内存操作，不写磁盘）
bool CaptureScreen(ScreenBitmap& out);

// 释放位图
void FreeScreenBitmap(ScreenBitmap& b);
