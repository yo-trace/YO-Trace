// YO-Trace 阶段三：OCR 桩实现（骨架占位）
//
/// 正式开发阶段（需 VSCode + MSVC + C++/WinRT）将用 Windows.Media.Ocr 替换本文件：
///   调用 Windows::Media::Ocr::OcrEngine::TryCreateFromUserProfile() 得到引擎，
///   把 ScreenBitmap 的 HBITMAP 转成 SoftwareBitmap 后识别，返回每个文字块的内容/坐标/置信度。
///
/// 当前桩实现：不调用真实 OCR，而是把"当前窗口标题"当作文本块放到对应窗口区域，
/// 目的是让整条流水线（截屏 -> OCR -> text_blocks 落库 -> 查询）先跑通、可验证。
// 真实引擎接入后，只需新增一个 OcrEngine 实现并在 main 中替换 CreateXxxOcrEngine 即可。

#include "ocr_engine.h"
#include <string>

static std::string W2U8(const std::wstring& s) {
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0, NULL, NULL);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n, NULL, NULL);
    return out;
}

class StubOcrEngine : public IOcrEngine {
public:
    const char* name() const override { return "StubOcrEngine(占位-待替换为Windows.Media.Ocr)"; }

    bool recognize(HBITMAP, const std::vector<ScreenWindow>& wins,
                   std::vector<TextBlock>& out) override {
        out.clear();
        for (const auto& w : wins) {
            if (w.title.empty()) continue;
            TextBlock b;
            b.content = W2U8(w.title);
            // 近似放在窗口标题栏区域（真实 OCR 会给精确包围盒）
            b.x1 = w.x1;
            b.y1 = w.y1;
            b.x2 = w.x2;
            b.y2 = w.y1 + 30;
            b.confidence = 1.0;  // 桩固定值；真实引擎由模型给出
            out.push_back(b);
        }
        return true;
    }
};

// 工厂：正式阶段改成 CreateWindowsMediaOcrEngine() 等即可
IOcrEngine* CreateOcrEngine() {
    return new StubOcrEngine();
}
