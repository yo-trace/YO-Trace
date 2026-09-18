// YO-Trace 阶段三：屏幕内存截取实现
#include "screen_capture.h"

bool CaptureScreen(ScreenBitmap& out) {
    int w = GetSystemMetrics(SM_CXSCREEN);
    int h = GetSystemMetrics(SM_CYSCREEN);
    if (w <= 0 || h <= 0) return false;

    HDC hdcScreen = GetDC(NULL);
    if (!hdcScreen) return false;

    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hbmp = CreateCompatibleBitmap(hdcScreen, w, h);
    bool ok = false;
    if (hbmp) {
        HGDIOBJ old = SelectObject(hdcMem, hbmp);
        // 纯内存 BitBlt，不落盘（任务 3.3）
        ok = BitBlt(hdcMem, 0, 0, w, h, hdcScreen, 0, 0, SRCCOPY) != 0;
        SelectObject(hdcMem, old);
        if (ok) {
            out.hbmp = hbmp;
            out.width = w;
            out.height = h;
        } else {
            DeleteObject(hbmp);
        }
    }
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
    return ok;
}

void FreeScreenBitmap(ScreenBitmap& b) {
    if (b.hbmp) {
        DeleteObject(b.hbmp);
        b.hbmp = NULL;
    }
    b.width = b.height = 0;
}
