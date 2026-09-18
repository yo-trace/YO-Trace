// YO-Trace 阶段二：窗口采集 + SQLite 存储（在阶段一基础上增加持久化与去重）
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <psapi.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <iostream>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <algorithm>

#include "sqlite_storage.h"

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "shell32.lib")

#define WM_TRAYICON (WM_APP + 1)
#define ID_TRAY_EXIT 1001

// ------------------------- 日志（UTF-8 落盘） -------------------------
static std::string g_logPath;
static std::ofstream g_log;

static std::string W2U8(const std::wstring& s) {
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0, NULL, NULL);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n, NULL, NULL);
    return out;
}
static void LogLine(const std::wstring& s) {
    std::wcout << s;
    if (g_log.is_open()) { g_log << W2U8(s); g_log.flush(); }
}
static std::wstring A2W(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
    return out;
}

// ------------------------- 窗口采集（复用阶段一引擎） -------------------------
struct WindowInfo {
    HWND hwnd;
    std::wstring title;
    std::wstring className;
    std::wstring processName;
    DWORD pid;
    int x1, y1, x2, y2;
    int zOrder;
    bool isForeground;
};
struct Snapshot {
    std::wstring timeStr;
    std::vector<WindowInfo> windows;
};

static std::vector<WindowInfo>* g_captureTarget = nullptr;

static bool IsExcludedClass(const std::wstring& cls) {
    return cls == L"Progman" || cls == L"WorkerW" || cls == L"Shell_TrayWnd";
}
static std::wstring NowString() {
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    localtime_s(&tm, &t);
    wchar_t buf[64];
    wcsftime(buf, sizeof(buf) / sizeof(wchar_t), L"%Y-%m-%d %H:%M:%S", &tm);
    return std::wstring(buf);
}
static void GetProcessInfo(HWND hwnd, WindowInfo& info) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    info.pid = pid;
    if (!pid) return;
    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProc) return;
    wchar_t pathBuf[MAX_PATH] = {0};
    DWORD pathLen = MAX_PATH;
    if (QueryFullProcessImageNameW(hProc, 0, pathBuf, &pathLen) && pathLen > 0) {
        std::wstring p(pathBuf, pathLen);
        auto pos = p.find_last_of(L"\\/");
        info.processName = (pos == std::wstring::npos) ? p : p.substr(pos + 1);
    }
    CloseHandle(hProc);
}
static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    int* zCounter = (int*)lParam;
    if (!IsWindowVisible(hwnd)) return TRUE;
    int len = GetWindowTextLengthW(hwnd);
    if (len == 0) return TRUE;
    wchar_t clsBuf[256] = {0};
    GetClassNameW(hwnd, clsBuf, 256);
    std::wstring cls(clsBuf);
    if (IsExcludedClass(cls)) return TRUE;

    WindowInfo info;
    info.hwnd = hwnd;
    info.zOrder = (*zCounter)++;
    std::wstring title((size_t)len, L'\0');
    GetWindowTextW(hwnd, &title[0], len + 1);
    info.title = title;
    RECT rc;
    if (GetWindowRect(hwnd, &rc)) {
        info.x1 = rc.left; info.y1 = rc.top; info.x2 = rc.right; info.y2 = rc.bottom;
    }
    info.className = cls;
    GetProcessInfo(hwnd, info);
    info.isForeground = (hwnd == GetForegroundWindow());
    if (g_captureTarget) g_captureTarget->push_back(info);
    return TRUE;
}
static Snapshot CaptureSnapshot() {
    Snapshot snap;
    snap.timeStr = NowString();
    int z = 0;
    std::vector<WindowInfo> collector;
    g_captureTarget = &collector;
    EnumWindows(EnumWindowsProc, (LPARAM)&z);
    g_captureTarget = nullptr;
    snap.windows = collector;
    return snap;
}

// ------------------------- 数据库与去重 -------------------------
static TraceDB g_db;
static HWND g_msgWnd = NULL;
static NOTIFYICONDATAW g_nid = {0};
static std::string g_lastSig;   // 2.5 去重：上一次快照签名

// 用"标题+坐标"拼接成签名，判断是否发生变化
static std::string ComputeSignature(const Snapshot& snap) {
    std::string sig;
    for (const auto& w : snap.windows) {
        sig += W2U8(w.title);
        sig += "|";
        sig += std::to_string(w.x1) + "," + std::to_string(w.y1) + "," +
               std::to_string(w.x2) + "," + std::to_string(w.y2);
        sig += "\n";
    }
    return sig;
}

// 执行一次采集并写入数据库（带去重 + 耗时统计）
static void TriggerCapture() {
    Snapshot snap = CaptureSnapshot();

    std::string sig = ComputeSignature(snap);
    if (sig == g_lastSig) {
        // 2.5 无变化则不写入
        LogLine(L"[采集] " + snap.timeStr + L" 窗口无变化，跳过写入（窗口数=" +
                std::to_wstring(snap.windows.size()) + L"）\n");
        return;
    }
    g_lastSig = sig;

    // 转成 UTF-8 行供存储
    std::vector<WindowRow> rows;
    rows.reserve(snap.windows.size());
    for (const auto& w : snap.windows) {
        WindowRow r;
        r.title = W2U8(w.title);
        r.app_name = W2U8(w.processName);
        r.x1 = w.x1; r.y1 = w.y1; r.x2 = w.x2; r.y2 = w.y2;
        rows.push_back(r);
    }

    double elapsed = 0;
    if (g_db.insertSnapshot(W2U8(snap.timeStr), rows, elapsed)) {
        std::wstring msg = L"[采集] " + snap.timeStr + L" 已写入 snapshot_id=" +
            std::to_wstring(g_db.lastSnapshotId()) + L" 窗口数=" +
            std::to_wstring(rows.size()) + L" 耗时=" +
            std::to_wstring((long long)(elapsed * 1000)) + L"us\n";
        LogLine(msg);
    } else {
        LogLine(L"[错误] 写入失败：" + A2W(g_db.lastError() ? std::string(g_db.lastError()) : "") + L"\n");
    }
}

// ------------------------- 托盘/消息窗口 -------------------------
static LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_TIMER:
            TriggerCapture();
            return 0;
        case WM_HOTKEY:
            if (wParam == 1) { LogLine(L"[F12] 手动采集触发\n"); TriggerCapture(); }
            return 0;
        case WM_TRAYICON:
            if (LOWORD(lParam) == WM_RBUTTONUP) {
                POINT pt; GetCursorPos(&pt);
                SetForegroundWindow(hwnd);
                HMENU hMenu = CreatePopupMenu();
                AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"退出");
                TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, NULL);
                DestroyMenu(hMenu);
            } else if (LOWORD(lParam) == WM_LBUTTONDBLCLK) {
                TriggerCapture();
            }
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == ID_TRAY_EXIT) DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}
static bool CreateMsgWindow() {
    const wchar_t* CLASS_NAME = L"YOTracePhase2MsgWindow";
    WNDCLASSW wc{};
    wc.lpfnWndProc = MsgWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = CLASS_NAME;
    if (!RegisterClassW(&wc)) return false;
    g_msgWnd = CreateWindowExW(0, CLASS_NAME, L"YO-Trace", 0, 0, 0, 0, 0,
                               HWND_MESSAGE, NULL, GetModuleHandle(NULL), NULL);
    return g_msgWnd != NULL;
}
static void AddTrayIcon(HWND hwnd) {
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, _countof(g_nid.szTip), L"YO-Trace 采集+存储");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

// ------------------------- 入口 -------------------------
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    // 日志与数据库路径：exe 同目录
    wchar_t exePath[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring exeStr(exePath);
    auto pos = exeStr.find_last_of(L"\\/");
    std::wstring exeDir = (pos == std::wstring::npos) ? L"" : exeStr.substr(0, pos + 1);
    g_logPath = W2U8(exeDir + L"yotrace_phase2.log");
    std::string dbPath = W2U8(exeDir + L"yotrace_phase2.db");

    g_log.open(g_logPath, std::ios::out | std::ios::app);
    LogLine(L"=== YO-Trace 阶段二启动 " + NowString() + L" ===\n");

    // 2.1 打开并建表
    if (!g_db.open(dbPath)) {
        LogLine(L"[错误] 无法打开数据库：" + A2W(g_db.lastError() ? std::string(g_db.lastError()) : "") + L"\n");
        return 1;
    }
    if (!g_db.createSchema()) {
        LogLine(L"[错误] 建表失败：" + A2W(g_db.lastError() ? std::string(g_db.lastError()) : "") + L"\n");
        return 1;
    }
    LogLine(L"数据库就绪：" + exeDir + L"yotrace_phase2.db\n");
    LogLine(L"托盘常驻：右键退出；F12 立即采集；每 5 秒自动采集（无变化跳过）。\n");

    if (!CreateMsgWindow()) {
        std::wcout << L"创建消息窗口失败：" << GetLastError() << L"\n";
        return 1;
    }
    AddTrayIcon(g_msgWnd);

    if (!RegisterHotKey(g_msgWnd, 1, MOD_NOREPEAT, VK_F12)) {
        LogLine(L"[警告] 注册 F12 失败，错误码 " + std::to_wstring(GetLastError()) + L"\n");
    }
    SetTimer(g_msgWnd, 2, 5000, NULL);

    TriggerCapture(); // 立即采集一次

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    LogLine(L"=== YO-Trace 阶段二退出 " + NowString() + L" ===\n");
    g_log.close();
    return 0;
}
