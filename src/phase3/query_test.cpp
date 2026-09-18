// YO-Trace 阶段三：text_blocks 查询 CLI 测试
// 用法：yotrace_query.exe [关键字]
// 编码说明：DB 中文字均以 UTF-8 存储。本程序以 UTF-16 取得命令行参数再转 UTF-8，
// 并把控制台输出代码页设为 UTF-8，彻底避免中文参数/结果的乱码与匹配失效。
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <iostream>
#include <stdio.h>
#include <io.h>
#include "sqlite_storage.h"

static std::string W2U8(const std::wstring& s) {
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0, NULL, NULL);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n, NULL, NULL);
    return out;
}
// 以 UTF-16 取得命令行参数再转为 UTF-8，规避控制台代码页(GBK)导致的中文参数乱码
static std::vector<std::string> GetUtf8Args() {
    int argc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> out;
    if (wargv) {
        out.reserve(argc);
        for (int i = 0; i < argc; ++i) out.push_back(W2U8(wargv[i]));
        LocalFree(wargv);
    }
    return out;
}

static std::string AppDir() {
    wchar_t exePath[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring s(exePath);
    auto p = s.find_last_of(L"\\/");
    return (p == std::wstring::npos) ? "" : std::string(s.begin(), s.end()).substr(0, p + 1);
}

int main() {
    SetConsoleOutputCP(CP_UTF8);   // 让控制台以 UTF-8 解释输出的中文（DB 中存的是 UTF-8）
    auto args = GetUtf8Args();
    int argc = (int)args.size();

    TraceDB db;
    std::string path = AppDir() + "yotrace_phase3.db";
    if (!db.open(path)) { std::cerr << "无法打开数据库: " << path << "\n"; return 1; }

    std::vector<TextBlock> blocks;
    std::string kw = (argc >= 2) ? args[1] : "";
    if (kw.empty()) {
        // 统计全部文本块
        blocks = db.queryTextBlocksByContent(""); // 空关键字 -> LIKE '%%' 匹配全部
        std::cout << "[默认] text_blocks 总数=" << blocks.size() << "\n";
    } else {
        blocks = db.queryTextBlocksByContent(kw);
        std::cout << "[按内容查询] 关键字=\"" << kw << "\" 命中=" << blocks.size() << "\n";
    }
    for (size_t i = 0; i < blocks.size() && i < 20; ++i) {
        const auto& b = blocks[i];
        std::cout << "  #" << (i + 1) << " \"" << b.content << "\""
                  << " 坐标=(" << b.x1 << "," << b.y1 << ")-(" << b.x2 << "," << b.y2 << ")"
                  << " 置信=" << b.confidence << "\n";
    }
    // 双击运行时控制台会瞬间关闭，仅在真正连着控制台时暂停，便于查看
    if (_isatty(_fileno(stdin))) {
        std::cout << "\n[完成] 按回车退出...";
        std::cin.get();
    }
    return 0;
}
