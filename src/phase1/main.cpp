// YO-Trace 阶段一：窗口采集引擎
// 目标：枚举所有可见窗口，记录标题/坐标/进程/类名/Z序/激活状态，
//       每 5 秒定时采集一次，并支持按 F12 手动触发。
//
// 编译（Dev-Cpp MinGW 或本机 g++）：
//   g++.exe main.cpp -o yotrace_phase1.exe -municode -static -mwindows
// 说明：-mwindows 去掉控制台黑窗口；若想保留控制台看日志，去掉 -mwindows
//       并把 WinMain 换成 main。下面用 WinMain + 自定义日志文件，方便后台运行。

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

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "shell32.lib")

// 托盘图标自定义消息与菜单项 ID
#define WM_TRAYICON (WM_APP + 1)
#define ID_TRAY_EXIT 1001

// ------------------------- 数据结构 -------------------------

struct WindowInfo {
    HWND hwnd;
    std::wstring title;
    std::wstring className;
    std::wstring processName;   // 例如 explorer.exe
    std::wstring processPath;   // 完整镜像路径
    DWORD pid;
    int x1, y1, x2, y2;
    int zOrder;                 // 0 = 最顶层（EnumWindows 顺序）
    bool isForeground;          // 是否当前激活窗口
};

struct Snapshot {
    std::wstring timeStr;       // 采集时间（本地）
    std::vector<WindowInfo> windows;
};

// ------------------------- 全局状态 -------------------------

static std::string g_logPath;   // 启动时计算为 exe 同目录下的 yotrace_phase1.log
static std::ofstream g_log;
static HWND g_msgWnd = NULL;
static NOTIFYICONDATAW g_nid = {0};

// 采集时由 CaptureSnapshot 指向当前收集容器，供 EnumWindows 回调回写结果
static std::vector<WindowInfo>* g_captureTarget = nullptr;

// 宽字符串 -> UTF-8（用于写入日志文件，避免 wofstream 默认 locale 吞掉输出）
static std::string W2U8(const std::wstring& s) {
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
                                NULL, 0, NULL, NULL);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
                        &out[0], n, NULL, NULL);
    return out;
}

// 同时输出到控制台与日志文件（日志以 UTF-8 写入）
static void LogLine(const std::wstring& s) {
    std::wcout << s;
    if (g_log.is_open()) { g_log << W2U8(s); g_log.flush(); }
}

// 需要过滤的桌面/外壳窗口类名
static bool IsExcludedClass(const std::wstring& cls) {
    // Progman / WorkerW = 桌面窗口；Shell_TrayWnd = 任务栏
    return cls == L"Progman" || cls == L"WorkerW" || cls == L"Shell_TrayWnd";
}

// 将当前时间格式化为 YYYY-MM-DD HH:MM:SS
static std::wstring NowString() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    wchar_t buf[64];
    wcsftime(buf, sizeof(buf) / sizeof(wchar_t), L"%Y-%m-%d %H:%M:%S", &tm);
    return std::wstring(buf);
}

// 取窗口所属进程的可执行名与完整路径
static void GetProcessInfo(HWND hwnd, WindowInfo& info) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    info.pid = pid;
    if (pid == 0) return;

    HANDLE hProc = OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
        FALSE, pid);
    if (!hProc) return;

    // 进程镜像完整路径
    wchar_t pathBuf[MAX_PATH] = {0};
    DWORD pathLen = MAX_PATH;
    if (QueryFullProcessImageNameW(hProc, 0, pathBuf, &pathLen) && pathLen > 0) {
        info.processPath = std::wstring(pathBuf, pathLen);
        // 仅取文件名作为 processName
        std::wstring p(pathBuf, pathLen);
        auto pos = p.find_last_of(L"\\/");
        info.processName = (pos == std::wstring::npos) ? p : p.substr(pos + 1);
    }
    CloseHandle(hProc);
}

// ------------------------- 采集核心 -------------------------

// EnumWindows 回调：按 Z 序（最顶层先）逐个访问顶层窗口
static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    int* zCounter = (int*)lParam;

    // 1.3 过滤：跳过不可见窗口
    if (!IsWindowVisible(hwnd)) return TRUE;

    // 1.3 过滤：无标题窗口跳过（桌面/任务栏等）
    int len = GetWindowTextLengthW(hwnd);
    if (len == 0) return TRUE;

    // 1.4 取类名
    wchar_t clsBuf[256] = {0};
    GetClassNameW(hwnd, clsBuf, 256);
    std::wstring cls(clsBuf);

    // 1.3 过滤：桌面/任务栏窗口
    if (IsExcludedClass(cls)) return TRUE;

    WindowInfo info;
    info.hwnd = hwnd;
    info.zOrder = (*zCounter)++;

    // 1.2 取标题
    std::wstring title((size_t)len, L'\0');
    GetWindowTextW(hwnd, &title[0], len + 1);
    info.title = title;

    // 1.2 取坐标
    RECT rc;
    if (GetWindowRect(hwnd, &rc)) {
        info.x1 = rc.left; info.y1 = rc.top;
        info.x2 = rc.right; info.y2 = rc.bottom;
    }

    // 1.4 取类名/进程信息
    info.className = cls;
    GetProcessInfo(hwnd, info);

    // 1.4 是否激活
    info.isForeground = (hwnd == GetForegroundWindow());

    // 回写到当前采集容器
    if (g_captureTarget) g_captureTarget->push_back(info);

    return TRUE; // 继续枚举
}

// 执行一次完整窗口快照采集
static Snapshot CaptureSnapshot() {
    Snapshot snap;
    snap.timeStr = NowString();
    snap.windows.clear();

    int z = 0;
    std::vector<WindowInfo> collector;
    g_captureTarget = &collector;
    EnumWindows(EnumWindowsProc, (LPARAM)&z);
    g_captureTarget = nullptr;
    snap.windows = collector;
    return snap;
}

// 把快照写入日志（控制台 + 文件）
static void PrintSnapshot(const Snapshot& snap) {
    std::wstring header = L"[采集] " + snap.timeStr + L"  窗口数=" +
                          std::to_wstring(snap.windows.size()) + L"\n";
    LogLine(header);

    for (const auto& w : snap.windows) {
        std::wstring line = L"  #" + std::to_wstring(w.zOrder) +
            (w.isForeground ? L" *激活* " : L"        ") +
            L" \"" + w.title + L"\""
            L" 类=" + w.className +
            L" 进程=" + w.processName +
            L" 坐标=(" + std::to_wstring(w.x1) + L"," + std::to_wstring(w.y1) +
            L")-(" + std::to_wstring(w.x2) + L"," + std::to_wstring(w.y2) + L")\n";
        LogLine(line);
    }
    LogLine(L"----------------------------------------\n");
}

// 触发一次采集（供定时器与热键共用）
static void TriggerCapture() {
    Snapshot snap = CaptureSnapshot();
    PrintSnapshot(snap);
}

// ------------------------- 消息窗口（承载定时器/热键） -------------------------

static LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_TIMER:
            // 1.5 定时采集（每 5 秒，由 SetTimer 的 uElapse 控制）
            TriggerCapture();
            return 0;
        case WM_HOTKEY:
            // 1.5 手动触发：F12
            if (wParam == 1) {
                LogLine(L"[F12] 手动采集触发\n");
                TriggerCapture();
            }
            return 0;
        case WM_TRAYICON:
            // 托盘图标交互
            if (LOWORD(lParam) == WM_RBUTTONUP) {
                POINT pt; GetCursorPos(&pt);
                SetForegroundWindow(hwnd); // 确保弹出菜单能正常收起
                HMENU hMenu = CreatePopupMenu();
                AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"退出");
                TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                               pt.x, pt.y, 0, hwnd, NULL);
                DestroyMenu(hMenu);
            } else if (LOWORD(lParam) == WM_LBUTTONDBLCLK) {
                TriggerCapture(); // 双击托盘立即采集一次
            }
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == ID_TRAY_EXIT) {
                DestroyWindow(hwnd); // 触发 WM_DESTROY -> 退出
            }
            return 0;
        case WM_DESTROY:
            Shell_NotifyIconW(NIM_DELETE, &g_nid); // 移除托盘图标
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// 在系统托盘区添加图标（右键菜单可退出）
static void AddTrayIcon(HWND hwnd) {
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, _countof(g_nid.szTip), L"YO-Trace 窗口采集");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static bool CreateMsgWindow() {
    const wchar_t* CLASS_NAME = L"YOTraceMsgWindow";
    WNDCLASSW wc{};
    wc.lpfnWndProc = MsgWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = CLASS_NAME;
    if (!RegisterClassW(&wc)) return false;

    // HWND_MESSAGE：仅用于接收消息的“消息-only”窗口，无界面
    g_msgWnd = CreateWindowExW(0, CLASS_NAME, L"YO-Trace", 0,
        0, 0, 0, 0, HWND_MESSAGE, NULL, GetModuleHandle(NULL), NULL);
    return g_msgWnd != NULL;
}

// ------------------------- 入口 -------------------------

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    // 计算日志路径：exe 同目录下的 yotrace_phase1.log
    wchar_t exePath[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring exeStr(exePath);
    auto pos = exeStr.find_last_of(L"\\/");
    std::wstring exeDir = (pos == std::wstring::npos) ? L"" : exeStr.substr(0, pos + 1);
    g_logPath = W2U8(exeDir + L"yotrace_phase1.log");

    // 打开日志文件（追加写）
    g_log.open(g_logPath, std::ios::out | std::ios::app);
    LogLine(L"YO-Trace 阶段一窗口采集引擎已启动（日志：" + exeDir + L"yotrace_phase1.log）\n");
    LogLine(L"托盘常驻：右键图标可退出；F12 立即采集；每 5 秒自动采集。\n");
    LogLine(L"=== YO-Trace 启动 " + NowString() + L" ===\n");

    if (!CreateMsgWindow()) {
        std::wcout << L"创建消息窗口失败，错误码：" << GetLastError() << L"\n";
        return 1;
    }

    // 在系统托盘区添加图标（右键退出）
    AddTrayIcon(g_msgWnd);

    // 1.5 注册全局热键 F12
    if (!RegisterHotKey(g_msgWnd, 1, MOD_NOREPEAT, VK_F12)) {
        std::wcout << L"警告：注册 F12 热键失败（可能已被占用），错误码："
                   << GetLastError() << L"\n";
    }

    // 1.5 设置 5 秒定时器（uElapse 单位毫秒）
    SetTimer(g_msgWnd, 2, 5000, NULL);

    // 立即采集一次
    TriggerCapture();

    // 消息循环（定时器与热键依赖它）
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    LogLine(L"=== YO-Trace 退出 " + NowString() + L" ===\n");
    g_log.close();
    return 0;
}
