// YO-Trace 阶段三骨架：窗口采集 + 屏幕截取 + OCR(桩) + text_blocks 落库
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
#include <queue>
#include <functional>

#include "sqlite_storage.h"
#include "screen_capture.h"
#include "ocr_engine.h"
#include "uia_capture.h"   // 任务 3.5：UIA 控件树（需 MSVC + Windows SDK）

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
static std::wstring A2W(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
    return out;
}
static void LogLine(const std::wstring& s) {
    std::wcout << s << std::flush;
    if (g_log.is_open()) { g_log << W2U8(s); g_log.flush(); }
}

static HWND g_msgWnd = NULL;  // 提前声明，供控制台关闭处理器使用

// 阶段四：窗口事件钩子（事件驱动采集）。WINEVENT_OUTOFCONTEXT 使回调在本线程消息循环中派发，
// 故无需额外线程；WINEVENT_SKIPOWNPROCESS 忽略本进程自身事件，避免自触发。
static HWINEVENTHOOK g_winHooks[3] = { NULL, NULL, NULL };
// 阶段四 4.3：内容级触发钩子（低层键盘/鼠标，系统级）。
static HHOOK g_kbHook = NULL;
static HHOOK g_mouseHook = NULL;
// 内容级触发需要"强制落库"标志：窗口标题/矩形未变但屏幕文字可能已变，故绕过窗口签名去重。
static bool g_forceNextCapture = false;

// 阶段四：200ms 静默合并。事件驱动采集时，200ms 内的连续变化只触发一次采集；
// 任一事件都会重置该计时器，直到 200ms 内无新事件才真正执行 TriggerCapture。
// force=true 表示内容级触发（键盘/滚轮），到达触发点时应跳过窗口签名去重。
static void ScheduleCapture(bool force = false) {
    if (force) g_forceNextCapture = true;
    if (g_msgWnd) SetTimer(g_msgWnd, 3, 200, NULL);
}

// 仅当标题"整体就是一串时钟"时忽略（独立时钟挂件每分钟刷新标题的噪声），
// 形如 "14:30" / "14:30:05"；含其它文字的标题（如"会议 - 14:30"）仍正常采集。
static bool IsPureClockTitle(const std::wstring& t) {
    if (t.empty() || t.size() > 8) return false;
    bool hasColon = false;
    for (wchar_t c : t) {
        if (c == L':' || c == L'：') { hasColon = true; continue; }
        if (c < L'0' || c > L'9') return false;
    }
    return hasColon;
}

static void CALLBACK WinEventProc(HWINEVENTHOOK /*hook*/, DWORD event, HWND hwnd,
                                  LONG idObject, LONG idChild, DWORD /*dwEventThread*/,
                                  DWORD /*dwmsEventTime*/) {
    (void)event;
    // 只关心窗口对象本身（控件子元素的事件忽略）；CHILDID_SELF == 0
    if (idObject != OBJID_WINDOW || idChild != 0) return;
    if (hwnd == g_msgWnd) return;                 // 忽略自身消息窗口
    if (event == EVENT_OBJECT_NAMECHANGE) {
        wchar_t buf[256] = { 0 };
        if (GetWindowTextW(hwnd, buf, 256) && IsPureClockTitle(buf)) return;
    }
    ScheduleCapture();
}

// 阶段四 4.3：低层键盘钩子（系统级）。仅响应会"产生内容"的按键（忽略纯修饰键），
// 触发内容级采集（强制落库）。低层钩子在本线程消息循环中派发，回调需极快（仅置标志+定时器）。
static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        const KBDLLHOOKSTRUCT* k = (const KBDLLHOOKSTRUCT*)lParam;
        // 纯修饰键（Ctrl/Shift/Alt/Win/Caps/Lock/Scroll）不产生内容变化，跳过以减少噪声
        switch (k->vkCode) {
        case VK_CONTROL: case VK_SHIFT: case VK_MENU:
        case VK_LWIN: case VK_RWIN: case VK_CAPITAL:
        case VK_NUMLOCK: case VK_SCROLL:
            break;
        default:
            ScheduleCapture(true);
            break;
        }
    }
    return CallNextHookEx(g_kbHook, nCode, wParam, lParam);
}

// 阶段四 4.3：低层鼠标钩子，监听滚轮（滚动 = 内容可能变化），触发内容级采集。
static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && (wParam == WM_MOUSEWHEEL || wParam == WM_MOUSEHWHEEL)) {
        ScheduleCapture(true);
    }
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

// 控制台关闭时清理托盘图标，避免残留
static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrl) {
    if (ctrl == CTRL_C_EVENT || ctrl == CTRL_CLOSE_EVENT) {
        if (g_msgWnd) DestroyWindow(g_msgWnd);
        return TRUE;
    }
    return FALSE;
}

// ------------------------- 窗口采集（沿用阶段一/二引擎） -------------------------
struct WindowInfo {
    HWND hwnd;
    std::wstring title;
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
    if (IsExcludedClass(std::wstring(clsBuf))) return TRUE;

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

// ------------------------- 数据库 / OCR / 去重 -------------------------
static TraceDB g_db;
static IOcrEngine* g_ocr = nullptr;
static NOTIFYICONDATAW g_nid = {0};
static std::string g_lastSig;
static bool g_uiaEnabled = true;   // UIA 控件树抓取开关（COM 处于 STA 时为 true）

// ------------------------- 未处理截屏队列 + 空闲窗口补处理 -------------------------
// 设计（用户确认：实时 + 空闲补处理；叠加视图 = 后台批量处理，无界面仅记录日志）：
//   * 实时：每次窗口变化都做 OCR 并立即落库（主路径）。
//   * 高频/失败延后：当变化属于高频突发（距上次变化 < BURST_WINDOW_MS）或实时 OCR 失败，
//     截屏不立即识别，而是入队等待空闲窗口批量补处理（"未处理截屏的叠加视图"）。
//   * 空闲窗口：每小时前 IDLE_MINUTE_THRESHOLD 分钟，或连续 IDLE_QUIET_MS 无变化，
//     触发后台批量 OCR：依次识别队列中截屏、落库、释放位图，仅写日志不打扰用户。
struct PendingOCR {
    HBITMAP hbmp = NULL;                 // 截屏内存位图（所有权随队列）
    long long snapId = 0;                // 已写入 DB 的快照 id
    std::vector<ScreenWindow> wins;      // 该截图对应的窗口线索
    std::wstring timeStr;                // 采集时间（仅日志）
};
static std::queue<PendingOCR> g_pending;
static const size_t PENDING_MAX = 60;                 // 队列上限，超出丢弃最旧
static const long long BURST_WINDOW_MS = 4000;        // 变化间隔 < 4s 视为高频，延后 OCR
static const long long IDLE_QUIET_MS   = 60000;       // 连续 60s 无变化视为空闲
static const int IDLE_MINUTE_THRESHOLD = 5;           // 每小时前 5 分钟为强制空闲窗口
static std::chrono::steady_clock::time_point g_lastChangeTime;
static bool g_hasPriorChange = false;                 // 是否已发生过一次变化（首捕不应判为高频）

// 入队（转移位图所有权）；队列满则丢弃最旧并释放其位图
static void EnqueuePending(HBITMAP hbmp, long long snapId,
                           const std::vector<ScreenWindow>& wins, const std::wstring& ts) {
    if (g_pending.size() >= PENDING_MAX) {
        PendingOCR old = g_pending.front(); g_pending.pop();
        if (old.hbmp) DeleteObject(old.hbmp);
    }
    PendingOCR p;
    p.hbmp = hbmp; p.snapId = snapId; p.wins = wins; p.timeStr = ts;
    g_pending.push(std::move(p));
}

// 是否处于空闲窗口（小时初 或 长时间无变化）
static bool IsIdleWindow() {
    auto now = std::chrono::steady_clock::now();
    long long sinceChange = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - g_lastChangeTime).count();
    SYSTEMTIME st; GetLocalTime(&st);
    return (st.wMinute < IDLE_MINUTE_THRESHOLD) || (sinceChange > IDLE_QUIET_MS);
}

// 空闲窗口批量补处理：依次识别队列截屏并落库（无界面，仅日志）
static void DrainPendingBatch() {
    if (g_pending.empty()) return;
    LogLine(L"[空闲补处理] 开始处理未处理截屏队列，共 " + std::to_wstring(g_pending.size()) +
            L" 张（后台批量，无界面，仅记录日志）\n");
    int processed = 0, totalBlocks = 0;
    while (!g_pending.empty()) {
        PendingOCR p = g_pending.front(); g_pending.pop();
        if (p.hbmp && g_ocr) {
            std::vector<TextBlock> blocks;
            if (g_ocr->recognize(p.hbmp, p.wins, blocks)) {
                double tb = 0;
                g_db.insertTextBlocks(p.snapId, blocks, tb);
                totalBlocks += (int)blocks.size();
                ++processed;
            }
        }
        if (p.hbmp) DeleteObject(p.hbmp);
    }
    LogLine(L"[空闲补处理] 完成：处理 " + std::to_wstring(processed) + L" 张，新增文本块 " +
            std::to_wstring(totalBlocks) + L"\n");
}

// 统计控件树节点总数（用于日志）
static int CountNodes(const std::vector<ControlNode>& roots) {
    int n = 0;
    std::function<void(const ControlNode&)> walk = [&](const ControlNode& node) {
        ++n;
        for (const auto& c : node.children) walk(c);
    };
    for (const auto& r : roots) walk(r);
    return n;
}

static std::string ComputeSignature(const Snapshot& snap) {
    std::string sig;
    for (const auto& w : snap.windows) {
        sig += W2U8(w.title) + "|" +
               std::to_string(w.x1) + "," + std::to_string(w.y1) + "," +
               std::to_string(w.x2) + "," + std::to_string(w.y2) + "\n";
    }
    return sig;
}

// 一次采集：窗口快照 + 截屏 + OCR + 落库
// force=true（内容级触发）时跳过窗口签名去重，因为键盘/滚轮可能改变了屏幕文字而窗口标题/矩形未变。
static void TriggerCapture(bool force = false) {
    Snapshot snap = CaptureSnapshot();
    std::string sig = ComputeSignature(snap);
    if (!force && sig == g_lastSig) {
        LogLine(L"[采集] " + snap.timeStr + L" 窗口无变化，跳过写入（窗口数=" +
                std::to_wstring(snap.windows.size()) + L"）\n");
        return;
    }
    g_lastSig = sig;

    // windows 行
    std::vector<WindowRow> rows;
    std::vector<ScreenWindow> screenWins;
    rows.reserve(snap.windows.size());
    screenWins.reserve(snap.windows.size());
    for (const auto& w : snap.windows) {
        WindowRow r;
        r.title = W2U8(w.title);
        r.app_name = W2U8(w.processName);
        r.x1 = w.x1; r.y1 = w.y1; r.x2 = w.x2; r.y2 = w.y2;
        rows.push_back(r);
        ScreenWindow sw; sw.title = w.title; sw.x1 = w.x1; sw.y1 = w.y1; sw.x2 = w.x2; sw.y2 = w.y2;
        screenWins.push_back(sw);
    }

    double tw = 0;
    std::vector<long long> windowIds;
    if (!g_db.insertSnapshot(W2U8(snap.timeStr), rows, tw, windowIds)) {
        LogLine(L"[错误] 写入 windows 失败：" + A2W(g_db.lastError() ? std::string(g_db.lastError()) : "") + L"\n");
        return;
    }
    long long snapId = g_db.lastSnapshotId();

    // 任务 3.5：UIA 控件树抓取（仅当 COM 处于 STA 单元时可用）。
    // 与 windowIds 一一对应（insertSnapshot 按 rows 顺序回填），逐窗口抓取控件树并落库。
    // 内容级触发（force）时跳过：控件结构未变，且可避免打字/滚动高频期间反复遍历 UIA 的开销。
    int ctrlNodes = 0;
    double totalCtrlMs = 0;
    if (g_uiaEnabled && !force) {
        for (size_t i = 0; i < snap.windows.size() && i < windowIds.size(); ++i) {
            const auto& w = snap.windows[i];
            // 过滤极小窗口（工具提示/零尺寸），避免对不重要窗口做昂贵遍历
            if ((w.x2 - w.x1) < 40 || (w.y2 - w.y1) < 40) continue;
            std::vector<ControlNode> roots;
            double cm = 0;
            if (CaptureControlTree(w.hwnd, roots)) {
                if (g_db.insertControls(windowIds[i], roots, cm)) {
                    ctrlNodes += CountNodes(roots);
                    totalCtrlMs += cm;
                }
            }
        }
    }

    // 截屏（内存）
    ScreenBitmap bmp{};
    bool captured = CaptureScreen(bmp);
    double ocrMs = 0;
    int nBlocks = 0;          // <0 表示已延后入队，待空闲窗口补处理
    if (captured) {
        auto now = std::chrono::steady_clock::now();
        long long sinceLast = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - g_lastChangeTime).count();
        bool highFreq = (g_hasPriorChange && sinceLast >= 0 && sinceLast < BURST_WINDOW_MS);

        if (highFreq) {
            // 高频突发：延后 OCR，入队等待空闲窗口批量补处理（叠加视图）
            EnqueuePending(bmp.hbmp, snapId, screenWins, snap.timeStr);
            bmp.hbmp = NULL;        // 位图所有权移交队列
            nBlocks = -1;           // 待处理
        } else {
            std::vector<TextBlock> blocks;
            if (g_ocr && g_ocr->recognize(bmp.hbmp, screenWins, blocks)) {
                double tb = 0;
                g_db.insertTextBlocks(snapId, blocks, tb);
                nBlocks = (int)blocks.size();
                ocrMs = tb;
            } else if (g_ocr) {
                // 实时 OCR 失败：转入队列稍后补处理
                EnqueuePending(bmp.hbmp, snapId, screenWins, snap.timeStr);
                bmp.hbmp = NULL;
                nBlocks = -1;
            }
        }
        g_lastChangeTime = now;
        g_hasPriorChange = true;
        if (bmp.hbmp) FreeScreenBitmap(bmp);
    }

    std::wstring nBlocksStr = (nBlocks < 0) ? L"待处理(已入队)" : std::to_wstring(nBlocks);
    std::wstring ctrlStr = g_uiaEnabled
        ? (L" 控件=" + std::to_wstring(ctrlNodes) +
           L" 控件耗时=" + std::to_wstring((long long)(totalCtrlMs * 1000)) + L"us")
        : L" 控件=跳过(COM非STA)";
    std::wstring msg = L"[采集] " + snap.timeStr + L" snapshot_id=" +
        std::to_wstring(snapId) + L" 窗口=" + std::to_wstring(rows.size()) +
        L" 文本块=" + nBlocksStr +
        L" 截屏=" + (captured ? L"OK" : L"失败") +
        L" 文本块耗时=" + std::to_wstring((long long)(ocrMs * 1000)) + L"us" +
        ctrlStr + L"\n";
    LogLine(msg);
}

// ------------------------- 托盘 / 消息窗口 -------------------------
static LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_TIMER:
            if (wParam == 2) {
                // 5 秒轮询兜底：捕捉窗口内文字变化等无窗口事件的变化
                if (IsIdleWindow()) DrainPendingBatch();   // 空闲窗口：后台批量补处理未处理截屏
                TriggerCapture();
            } else if (wParam == 3) {
                // 阶段四：事件驱动采集的 200ms 静默合并计时器到点，落实一次采集
                // （内容级触发设置了 g_forceNextCapture，则跳过窗口签名去重）
                KillTimer(g_msgWnd, 3);
                TriggerCapture(g_forceNextCapture);
                g_forceNextCapture = false;
            }
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
            for (HWINEVENTHOOK h : g_winHooks) if (h) UnhookWinEvent(h);  // 阶段四：卸载窗口事件钩子
            if (g_kbHook) UnhookWindowsHookEx(g_kbHook);                   // 阶段四 4.3：卸载键盘钩子
            if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook);            // 阶段四 4.3：卸载鼠标钩子
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}
static bool CreateMsgWindow() {
    const wchar_t* CLASS_NAME = L"YOTracePhase3MsgWindow";
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
    wcscpy_s(g_nid.szTip, _countof(g_nid.szTip), L"YO-Trace 采集+OCR");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

// ------------------------- 入口 -------------------------
int main() {
    // 让控制台正确显示 UTF-8 / 中文（wcout 走宽字符，这里仅作保底）
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    // COM 初始化为 STA：UIA 控件树与 Windows.Media.Ocr 都要求 STA 单元。
    // 先于此初始化，可保证即使 OCR 回退到 Tesseract（不调用 RoInitialize），UIA 仍可工作。
    // 若 COM 已被其他组件以 MTA 初始化（RPC_E_CHANGED_MODE），则关闭 UIA 抓取以免死锁。
    HRESULT hrCo = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (hrCo == RPC_E_CHANGED_MODE) {
        g_uiaEnabled = false;
        std::wcerr << L"[警告] COM 已以 MTA 模式初始化（冲突），UIA 控件树抓取将跳过。\n";
    }

    wchar_t exePath[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring exeStr(exePath);
    auto pos = exeStr.find_last_of(L"\\/");
    std::wstring exeDir = (pos == std::wstring::npos) ? L"" : exeStr.substr(0, pos + 1);
    g_logPath = W2U8(exeDir + L"yotrace_phase3.log");
    std::string dbPath = W2U8(exeDir + L"yotrace_phase3.db");

    g_log.open(g_logPath, std::ios::out | std::ios::app);
    LogLine(L"=== YO-Trace 阶段三骨架启动 " + NowString() + L" ===\n");

    g_ocr = CreateOcrEngine();
    LogLine(L"OCR 引擎：" + A2W(g_ocr ? g_ocr->name() : "") + L"\n");

    if (!g_db.open(dbPath)) {
        LogLine(L"[错误] 无法打开数据库：" + A2W(g_db.lastError() ? std::string(g_db.lastError()) : "") + L"\n");
        return 1;
    }
    if (!g_db.createSchema()) {
        LogLine(L"[错误] 建表失败：" + A2W(g_db.lastError() ? std::string(g_db.lastError()) : "") + L"\n");
        return 1;
    }
    LogLine(L"数据库就绪：" + exeDir + L"yotrace_phase3.db\n");
    LogLine(L"托盘常驻：右键退出；F12 立即采集。\n");
    LogLine(L"采集触发：窗口事件驱动（创建/销毁/显隐/聚焦/标题变化/前台切换）+ 内容级触发"
            L"（键盘输入/鼠标滚轮）+ 200ms 静默合并；另保留每 5 秒轮询兜底。"
            L"窗口级触发按标题/矩形去重，内容级触发强制落库（文字可能已变）。\n");
    LogLine(L"OCR 策略：变化时实时识别；空闲窗口（每小时前 " + std::to_wstring(IDLE_MINUTE_THRESHOLD) +
            L" 分钟 / 连续 " + std::to_wstring(IDLE_QUIET_MS / 1000) +
            L"s 无变化）后台批量补处理未处理截屏。\n");

    g_lastChangeTime = std::chrono::steady_clock::now();

    if (!CreateMsgWindow()) { std::wcout << L"创建消息窗口失败：" << GetLastError() << L"\n"; return 1; }
    AddTrayIcon(g_msgWnd);

    // 阶段四：注册窗口事件钩子，实现事件驱动采集。
    // 用三组范围监听，刻意避开 EVENT_OBJECT_LOCATIONCHANGE（光标闪烁/动画每帧都会触发，噪声过大）。
    g_winHooks[0] = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_STATECHANGE,
                                    NULL, WinEventProc, 0, 0,
                                    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    g_winHooks[1] = SetWinEventHook(EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE,
                                    NULL, WinEventProc, 0, 0,
                                    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    g_winHooks[2] = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                    NULL, WinEventProc, 0, 0,
                                    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!g_winHooks[0] && !g_winHooks[1] && !g_winHooks[2]) {
        LogLine(L"[警告] SetWinEventHook 全部失败（错误码 " + std::to_wstring(GetLastError()) +
                L"），将仅依赖 5 秒轮询采集。\n");
    }

    // 阶段四 4.3：内容级触发 —— 低层键盘/鼠标滚轮钩子（系统级）。
    // 回调仅置标志 + 定时器（极快），不阻塞输入；消息循环在本线程派发。
    g_kbHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandle(NULL), 0);
    g_mouseHook = SetWindowsHookEx(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandle(NULL), 0);
    if (!g_kbHook || !g_mouseHook) {
        LogLine(L"[警告] 键盘/鼠标钩子注册失败（错误码 " + std::to_wstring(GetLastError()) +
                L"），内容级触发将不可用（仍可用窗口事件与 5 秒轮询）。\n");
    }

    if (!RegisterHotKey(g_msgWnd, 1, MOD_NOREPEAT, VK_F12)) {
        LogLine(L"[警告] 注册 F12 失败，错误码 " + std::to_wstring(GetLastError()) + L"\n");
    }
    SetTimer(g_msgWnd, 2, 5000, NULL);
    TriggerCapture();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }

    LogLine(L"=== YO-Trace 阶段三骨架退出 " + NowString() + L" ===\n");
    delete g_ocr;
    g_log.close();
    return 0;
}
