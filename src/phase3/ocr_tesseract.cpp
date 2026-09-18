// YO-Trace 阶段三：真实 OCR 引擎（Tesseract，任务 3.2）
//
// 为什么用 Tesseract：Windows.Media.Ocr 是 WinRT API，需要 MSVC + Windows SDK
// 才能编译；当前工具链是 MinGW，无法直接编译。Tesseract 是成熟的开源 OCR
// （支持中英文），这里以"子进程调用 tesseract.exe"的方式接入——这样不受我们
// 32 位 exe 的影响，且能用系统/便携版 64 位 Tesseract。后续若切换到 MSVC，
// 可改为 C++/WinRT 直接调用 Windows.Media.Ocr，接口 IOcrEngine 不变。
//
// 流程：HBITMAP -> 临时 24bit BMP 文件 -> tesseract.exe(输出 TSV) ->
//       解析 TSV(level==4 行级) 拿到文字块内容/坐标/置信度。

#include "ocr_engine.h"
#include <windows.h>
#include <string>
#include <vector>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdlib>

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

// 定位 tesseract.exe：环境变量 -> 同级 tesseract/ 目录 -> PATH
static std::string FindTesseract() {
    const char* env = std::getenv("YOTRACE_TESSERACT_EXE");
    if (env && env[0]) return env;

    wchar_t exePath[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring s(exePath);
    auto p = s.find_last_of(L"\\/");
    std::wstring dir = (p == std::wstring::npos) ? L"" : s.substr(0, p + 1);

    std::wstring cand[] = {
        dir + L"tesseract\\tesseract.exe",
        dir + L"tesseract.exe"
    };
    for (auto& c : cand) {
        if (GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES)
            return W2U8(c);
    }
    // 退回 PATH（让 CreateProcess 自己找）
    return "tesseract.exe";
}

// 探测 tessdata 目录下可用语言，组成 -l 参数（缺 chi_sim 也不至于整段失败）
static std::string DetectLangList(const std::string& exe) {
    std::string dir = exe.substr(0, exe.find_last_of("/\\"));
    std::string tessdir = dir + "\\tessdata";
    std::vector<std::string> langs;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((tessdir + "\\*.traineddata").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { langs.push_back(fd.cFileName); } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    auto has = [&](const std::string& name) {
        for (auto& l : langs) if (l == name + ".traineddata") return true;
        return false;
    };
    std::string out;
    if (has("chi_sim")) out += "chi_sim+";
    if (has("eng")) out += "eng+";
    if (!out.empty()) { out.pop_back(); return out; }
    if (!langs.empty()) return langs[0].substr(0, langs[0].size() - 12); // 去掉 .traineddata
    return "eng"; // 兜底
}

// 把 HBITMAP 存成 24bit BMP 文件（Tesseract 通过 Leptonica 读 BMP）
static bool SaveHBITMAPAsBMP(HBITMAP hbmp, const std::string& path) {
    BITMAP bm;
    if (!GetObject(hbmp, sizeof(bm), &bm)) return false;
    int w = bm.bmWidth, h = bm.bmHeight;
    if (w <= 0 || h <= 0) return false;

    HDC hdc = CreateCompatibleDC(NULL);
    if (!hdc) return false;
    HBITMAP old = (HBITMAP)SelectObject(hdc, hbmp);

    BITMAPINFOHEADER bi{};
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = w;
    bi.biHeight = -h;            //  top-down
    bi.biPlanes = 1;
    bi.biBitCount = 24;
    bi.biCompression = BI_RGB;

    int rowBytes = ((w * 3 + 3) / 4) * 4;
    std::vector<BYTE> pixels((size_t)rowBytes * h);
    bool ok = GetDIBits(hdc, hbmp, 0, (UINT)h, pixels.data(),
                        (BITMAPINFO*)&bi, DIB_RGB_COLORS) != 0;

    SelectObject(hdc, old);
    DeleteDC(hdc);
    if (!ok) return false;

    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42; // "BM"
    fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fh.bfSize = fh.bfOffBits + (DWORD)pixels.size();

    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write((char*)&fh, sizeof(fh));
    f.write((char*)&bi, sizeof(bi));
    f.write((char*)pixels.data(), (std::streamsize)pixels.size());
    return (bool)f;
}

// 解析 TSV：level==4（行）作为文字块
static bool ParseTsv(const std::string& tsvPath, std::vector<TextBlock>& out) {
    std::ifstream f(tsvPath);
    if (!f) return false;
    std::string line;
    bool header = true;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        if (header) { header = false; continue; } // 跳过表头
        std::vector<std::string> col;
        std::stringstream ss(line);
        std::string item;
        while (std::getline(ss, item, '\t')) col.push_back(item);
        if (col.size() < 12) continue;
        int level = atoi(col[0].c_str());
        if (level != 4) continue;                 // 行级
        const std::string& text = col[11];
        if (text.empty()) continue;
        TextBlock b;
        b.content = text;
        int left = atoi(col[6].c_str());
        int top = atoi(col[7].c_str());
        int w = atoi(col[8].c_str());
        int h = atoi(col[9].c_str());
        b.x1 = left; b.y1 = top; b.x2 = left + w; b.y2 = top + h;
        b.confidence = atof(col[10].c_str()) / 100.0; // TSV 置信度是 0~100
        out.push_back(b);
    }
    return true;
}

class TesseractOcrEngine : public IOcrEngine {
public:
    TesseractOcrEngine() : exe_(FindTesseract()) {}

    const char* name() const override {
        return "TesseractOcrEngine(真实OCR, 经 tesseract.exe 子进程)";
    }

    bool recognize(HBITMAP hbmp, const std::vector<ScreenWindow>&,
                   std::vector<TextBlock>& out) override {
        out.clear();
        if (!hbmp) return false;

        // 临时文件
        char tmpDir[MAX_PATH] = {0};
        GetTempPathA(MAX_PATH, tmpDir);
        char bmpPath[MAX_PATH] = {0}, outBase[MAX_PATH] = {0};
        GetTempFileNameA(tmpDir, "yo", 0, bmpPath);
        GetTempFileNameA(tmpDir, "yo", 0, outBase);
        std::string tsvPath = std::string(outBase) + ".tsv";

        if (!SaveHBITMAPAsBMP(hbmp, bmpPath)) {
            DeleteFileA(bmpPath); DeleteFileA(outBase);
            return false;
        }

        // 语言：自动探测可用语言包（优先 chi_sim+eng）
        std::string lang = DetectLangList(exe_);
        std::string cmd = "\"" + exe_ + "\" \"" + bmpPath + "\" \"" + outBase +
                          "\" -l " + lang + " --psm 3 tsv";

        STARTUPINFOA si{}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        // 隐藏 tesseract 的黑窗口
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        std::vector<char> buf(cmd.begin(), cmd.end());
        buf.push_back('\0');
        bool ran = CreateProcessA(NULL, buf.data(), NULL, NULL, FALSE,
                                   CREATE_NO_WINDOW, NULL, NULL, &si, &pi) != 0;
        if (ran) {
            WaitForSingleObject(pi.hProcess, 60000); // 最多等 60s
            DWORD ec = 0; GetExitCodeProcess(pi.hProcess, &ec);
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            if (ec == 0) ParseTsv(tsvPath, out);
            else std::cerr << "[Tesseract] 进程退出码=" << ec << "\n";
        } else {
            std::cerr << "[Tesseract] 无法启动 tesseract.exe: " << exe_
                      << " (错误 " << GetLastError() << ")\n";
        }

        DeleteFileA(bmpPath);
        DeleteFileA(outBase);
        DeleteFileA(tsvPath.c_str());
        return ran;
    }

private:
    std::string exe_;
};

// 工厂：返回真实 Tesseract 引擎（作为 Windows 原生 OCR 的回退）
IOcrEngine* CreateTesseractOcrEngine() {
    return new TesseractOcrEngine();
}
