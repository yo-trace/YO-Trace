// YO-Trace 阶段二：读取查询命令行测试（2.4）
// 用法（在 build/ 目录运行，sqlite 文件同目录）：
//   query_test.exe                       -> 显示记录总数 + 最近 10 条
//   query_test.exe --time "2026-08-26 19:00:00" "2026-08-26 20:00:00"
//   query_test.exe --title "合同"
// 编码说明：DB 中文字均以 UTF-8 存储。以 UTF-16 取得参数再转 UTF-8，并把控制台输出
// 代码页设为 UTF-8，避免中文参数/结果乱码与匹配失效。
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <iostream>
#include <fstream>
#include "sqlite_storage.h"

static std::string W2U8(const std::wstring& s) {
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0, NULL, NULL);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n, NULL, NULL);
    return out;
}
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
    SetConsoleOutputCP(CP_UTF8);
    auto args = GetUtf8Args();
    int argc = (int)args.size();

    TraceDB db;
    std::string path = AppDir() + "yotrace_phase2.db";
    if (!db.open(path)) {
        std::cerr << "无法打开数据库: " << path << "\n";
        return 1;
    }

    std::vector<WindowRow> rows;
    if (argc >= 2 && args[1] == "--time" && argc >= 4) {
        rows = db.queryByTimeRange(args[2], args[3]);
        std::cout << "[按时间范围查询] " << args[2] << " ~ " << args[3] << "\n";
    } else if (argc >= 3 && args[1] == "--title") {
        rows = db.queryByTitleLike(args[2]);
        std::cout << "[按标题模糊查询] 关键字=\"" << args[2] << "\"\n";
    } else {
        // 默认：最近 10 条
        rows = db.queryByTimeRange("0000-01-01 00:00:00", "9999-12-31 23:59:59");
        if (rows.size() > 10) rows.erase(rows.begin() + 10, rows.end());
        std::cout << "[默认] 最近 " << rows.size() << " 条窗口记录\n";
    }

    for (const auto& r : rows) {
        std::cout << "  #" << r.id << " snap=" << r.snapshot_id
                  << " \"" << r.title << "\""
                  << " 进程=" << r.app_name
                  << " 坐标=(" << r.x1 << "," << r.y1 << ")-(" << r.x2 << "," << r.y2 << ")\n";
    }
    std::cout << "共 " << rows.size() << " 条\n";
    return 0;
}
