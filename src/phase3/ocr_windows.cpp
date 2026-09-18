// YO-Trace 阶段三：Windows 原生 OCR 引擎（Windows.Media.Ocr，手写 WinRT ABI）
//
// 通过手写 ABI 调用 Windows.Media.Ocr（无需 MSVC/C++/WinRT），识别整屏文字并
// 解析出带坐标的文字块。若初始化或识别任意一步失败，工厂函数会回退到 Tesseract。

#include "ocr_engine.h"
#include "winrt_ocr_abi.h"
#include <string>
#include <vector>
#include <iostream>
#include <windows.h>

// ---- 通用辅助：宽字符串 -> UTF-8 ----
static std::string W2U8_Conv(const std::wstring& s) {
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0, NULL, NULL);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n, NULL, NULL);
    return out;
}

// ---- 辅助：HBITMAP -> BGRA32 像素缓冲（Windows.Media.Ocr 需要 Bgra8 = 蓝绿红透明度顺序）----
// GetDIBits 32bpp BI_RGB 返回的字节顺序正好是 B,G,R,0（即 alpha=0），与 Bgra8 一致。
static bool HBITMAPToPixels(HBITMAP hbmp, std::vector<BYTE>& out, int& w, int& h) {
    BITMAP bm;
    if (!GetObject(hbmp, sizeof(bm), &bm)) return false;
    w = bm.bmWidth; h = bm.bmHeight;
    if (w <= 0 || h <= 0) return false;

    HDC hdc = CreateCompatibleDC(NULL);
    if (!hdc) return false;
    HBITMAP old = (HBITMAP)SelectObject(hdc, hbmp);

    BITMAPINFOHEADER bi{};
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = w;
    bi.biHeight = -h;          // top-down
    bi.biPlanes = 1;
    bi.biBitCount = 32;        // BGRA
    bi.biCompression = BI_RGB;

    out.resize((size_t)w * h * 4);
    bool ok = GetDIBits(hdc, hbmp, 0, (UINT)h, out.data(),
                        (BITMAPINFO*)&bi, DIB_RGB_COLORS) != 0;

    SelectObject(hdc, old);
    DeleteDC(hdc);
    return ok;
}

// 在 STA 下等待异步完成事件，同时泵消息（否则异步完成回调无法回派到本线程，会死锁）
static DWORD WaitWithPump(HANDLE hEvent, DWORD timeoutMs) {
    DWORD start = GetTickCount();
    while (true) {
        DWORD res = MsgWaitForMultipleObjects(1, &hEvent, FALSE, timeoutMs, QS_ALLINPUT);
        if (res == WAIT_OBJECT_0) return WAIT_OBJECT_0;
        if (res == WAIT_OBJECT_0 + 1) {
            MSG msg;
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (GetTickCount() - start >= timeoutMs) return WAIT_TIMEOUT;
            continue;
        }
        return res;
    }
}

// ---- 异步完成处理器（用于等待 RecognizeAsync 结束）----
struct CompletedHandler {
    IUnknownVtbl lpVtbl;
    HANDLE hEvent;
    LONG ref;
};
static HRESULT STDMETHODCALLTYPE CH_QueryInterface(void* self, REFIID, void** ppv) {
    // WinRT 在 put_Completed 时会 QueryInterface 我们的 handler 以获取完成回调接口。
    // 我们的内存布局（IUnknown + Invoke）满足该接口，故对任意 IID 返回自身即可。
    if (!ppv) return E_POINTER;
    *ppv = self;
    ((IUnknownVtbl*)self)->AddRef(self);
    return S_OK;
}
static ULONG STDMETHODCALLTYPE CH_AddRef(void* self) {
    return InterlockedIncrement(&((CompletedHandler*)self)->ref);
}
static ULONG STDMETHODCALLTYPE CH_Release(void* self) {
    ULONG r = InterlockedDecrement(&((CompletedHandler*)self)->ref);
    if (r == 0) {  // 最后一个引用释放时自行回收（引用来自创建者与 async 操作本身）
        if (((CompletedHandler*)self)->hEvent) CloseHandle(((CompletedHandler*)self)->hEvent);
        CoTaskMemFree(self);
    }
    return r;
}
static HRESULT STDMETHODCALLTYPE CH_Invoke(void* self, void* /*asyncInfo*/, int /*status*/) {
    SetEvent(((CompletedHandler*)self)->hEvent);
    return S_OK;
}
static CompletedHandler* MakeCompletedHandler() {
    CompletedHandler* h = (CompletedHandler*)CoTaskMemAlloc(sizeof(CompletedHandler));
    if (!h) return NULL;
    h->lpVtbl.QueryInterface = CH_QueryInterface;
    h->lpVtbl.AddRef = CH_AddRef;
    h->lpVtbl.Release = CH_Release;
    // 第 4 个槽为 Invoke（IAsyncOperationCompletedHandler ABI）
    ((void**) (&h->lpVtbl))[3] = (void*)CH_Invoke;
    h->hEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    h->ref = 1;
    return h;
}

class WindowsMediaOcrEngine : public IOcrEngine {
    void* m_ocrEngine = NULL;   // IOcrEngine*
    std::wstring m_lang;
public:
    WindowsMediaOcrEngine() {
        // 使用 STA：让 RoGetActivationFactory 直接在本线程取得工厂（避免非 agile 工厂
        // 返回跨单元代理，导致后续方法调用在 MTA 下编组失败/段错误）。
        // 主线程已有消息循环，异步完成会通过消息队列回派，等待时需泵消息（见 WaitWithPump）。
        RoInitialize(RO_INIT_SINGLETHREADED);
        static const wchar_t* tries[] = { L"zh-Hans", L"zh-CN", L"en-US" };
        void* factory = NULL;
        HSTRING hsOcr = NULL; MakeHString(RUNTIME_OcrEngine, &hsOcr);
        if (SUCCEEDED(RoGetActivationFactory(hsOcr, IID_IOcrEngineStatics, &factory)) && factory) {
            // 优先使用用户配置语言，再依次尝试列表
            void* prof = NULL;
            if (SUCCEEDED(VT_OCRSTATICS(factory)->TryCreateFromUserProfileLanguages(factory, &prof)) && prof) {
                m_ocrEngine = prof; m_lang = L"用户配置语言";
            }
            if (!m_ocrEngine) {
                void* langFactory = NULL;
                HSTRING hsLang = NULL; MakeHString(RUNTIME_Language, &hsLang);
                if (SUCCEEDED(RoGetActivationFactory(hsLang, IID_ILanguageFactory, &langFactory)) && langFactory) {
                    for (auto t : tries) {
                        HSTRING hsTag = NULL; MakeHString(t, &hsTag);
                        void* lang = NULL;
                        if (SUCCEEDED(VT_LANG_FACT(langFactory)->CreateLanguage(langFactory, hsTag, &lang)) && lang) {
                            void* eng = NULL;
                            if (SUCCEEDED(VT_OCRSTATICS(factory)->TryCreateFromLanguage(factory, lang, &eng)) && eng) {
                                m_ocrEngine = eng; m_lang = t;
                                if (hsTag) WindowsDeleteString(hsTag);
                                break;
                            }
                            if (hsTag) WindowsDeleteString(hsTag);
                        }
                    }
                    if (hsLang) WindowsDeleteString(hsLang);
                }
            }
            if (hsOcr) WindowsDeleteString(hsOcr);
        }
    }
    ~WindowsMediaOcrEngine() {
        if (m_ocrEngine) { VT_OCRENGINE(m_ocrEngine)->Release(m_ocrEngine); m_ocrEngine = NULL; }
        RoUninitialize();
    }
    bool ready() const { return m_ocrEngine != NULL; }

    const char* name() const override {
        return "WindowsMediaOcrEngine(Windows.Media.Ocr 原生, 经手写 WinRT ABI)";
    }

    // 核心：像素(BGRA32) -> SoftwareBitmap -> OcrEngine.RecognizeAsync -> 解析
    bool recognizePixels(const BYTE* bgra, int w, int h,
                         const std::vector<ScreenWindow>&,
                         std::vector<TextBlock>& out) {
        out.clear();
        if (!m_ocrEngine || !bgra || w <= 0 || h <= 0) return false;

        void* sbStatics = NULL;
        HSTRING hsSb = NULL; MakeHString(RUNTIME_SoftwareBitmap, &hsSb);
        if (FAILED(RoGetActivationFactory(hsSb, IID_ISoftwareBitmapStatics, &sbStatics)) || !sbStatics) {
            if (hsSb) WindowsDeleteString(hsSb);
            std::cerr << "[WinOcr] 无法获取 SoftwareBitmap 工厂\n";
            return false;
        }
        if (hsSb) WindowsDeleteString(hsSb);

        void* swbmp = NULL;
        // Create(Bgra8, w, h, Ignore) -> ISoftwareBitmap
        HRESULT hr = VT_SBSTATICS(sbStatics)->Create(sbStatics, BpfBgra8, w, h, BamIgnore, &swbmp);
        if (FAILED(hr) || !swbmp) {
            VT_SBSTATICS(sbStatics)->Release(sbStatics);
            std::cerr << "[WinOcr] SoftwareBitmap.Create 失败 hr=" << hr << "\n";
            return false;
        }

        bool ok = false;
        void* buf = NULL;
        if (SUCCEEDED(VT_SB(swbmp)->LockBuffer(swbmp, BbamReadWrite, &buf)) && buf) {
            void* ibuf = NULL;
            if (SUCCEEDED(VT_BB(buf)->get_Buffer(buf, &ibuf)) && ibuf) {
                void* bba = NULL;
                // 通过 IBuffer 查询 IBufferByteAccess
                if (SUCCEEDED(((IUnknownVtbl*)((void**)ibuf)[0])->QueryInterface(ibuf, IID_IBufferByteAccess, &bba)) && bba) {
                    BitmapPlaneDescription pd;
                    if (SUCCEEDED(VT_BB(buf)->GetPlaneDescription(buf, 0, &pd))) {
                        UINT32 len = 0; BYTE* pBytes = NULL;
                        if (SUCCEEDED(VT_BBA(bba)->Buffer(bba, &len, &pBytes)) && pBytes) {
                            int srcStride = w * 4;
                            for (int y = 0; y < h; ++y) {
                                const BYTE* src = bgra + (size_t)y * srcStride;
                                BYTE* dst = pBytes + pd.StartIndex + (size_t)y * pd.Stride;
                                memcpy(dst, src, (size_t)srcStride);
                            }
                            ok = true;
                        }
                    }
                    ((IUnknownVtbl*)((void**)bba)[0])->Release(bba);
                }
            }
            VT_BB(buf)->Unmap(buf);
            ((IUnknownVtbl*)((void**)ibuf)[0])->Release(ibuf);
        }
        VT_SB(swbmp)->Release(swbmp);
        VT_SBSTATICS(sbStatics)->Release(sbStatics);

        if (!ok) { std::cerr << "[WinOcr] 无法写入 SoftwareBitmap 像素\n"; return false; }

        // 直接复用：LockBuffer 后 Unmap，SoftwareBitmap 仍可被 RecognizeAsync 读取
        void* async = NULL;
        hr = VT_OCRENGINE(m_ocrEngine)->RecognizeAsync(m_ocrEngine, swbmp, &async);
        if (FAILED(hr) || !async) { std::cerr << "[WinOcr] RecognizeAsync 失败 hr=" << hr << "\n"; return false; }

        // 等待完成
        CompletedHandler* ch = MakeCompletedHandler();
        bool done = false;
        if (ch) {
            VT_ASYNC(async)->put_Completed(async, ch);
            DWORD wres = WaitWithPump(ch->hEvent, 30000);
            done = (wres == WAIT_OBJECT_0);
            if (!done) std::cerr << "[WinOcr] 等待 OCR 超时\n";
        } else {
            // 回退：自旋短时
            Sleep(2000); done = true;
        }

        if (done) {
            void* result = NULL;
            if (SUCCEEDED(VT_ASYNC(async)->GetResults(async, &result)) && result) {
                ok = ParseResult(result, out);
                ((IUnknownVtbl*)((void**)result)[0])->Release(result);
            }
        }
        // 仅释放"我们的"那一个引用；async 持有的引用会随 Release(async) 一并释放并触发自回收
        if (ch) { ((IUnknownVtbl*)((void**)ch)[0])->Release(ch); }
        ((IUnknownVtbl*)((void**)async)[0])->Release(async);
        return ok;
    }

    bool recognize(HBITMAP hbmp, const std::vector<ScreenWindow>& wins,
                  std::vector<TextBlock>& out) override {
        std::vector<BYTE> px; int w = 0, h = 0;
        if (!HBITMAPToPixels(hbmp, px, w, h)) return false;
        return recognizePixels(px.data(), w, h, wins, out);
    }

private:
    bool ParseResult(void* result, std::vector<TextBlock>& out) {
        void* lines = NULL;
        if (FAILED(VT_OCRRESULT(result)->Lines(result, &lines)) || !lines) return false;
        UINT32 nLines = 0;
        if (FAILED(VT_VEC(lines)->get_Size(lines, &nLines))) { ((IUnknownVtbl*)((void**)lines)[0])->Release(lines); return false; }
        for (UINT32 i = 0; i < nLines; ++i) {
            void* line = NULL;
            if (FAILED(VT_VEC(lines)->GetAt(lines, i, &line)) || !line) continue;
            void* words = NULL;
            if (SUCCEEDED(VT_OCRLINE(line)->Words(line, &words)) && words) {
                UINT32 nWords = 0;
                if (SUCCEEDED(VT_VEC(words)->get_Size(words, &nWords))) {
                    for (UINT32 j = 0; j < nWords; ++j) {
                        void* word = NULL;
                        if (FAILED(VT_VEC(words)->GetAt(words, j, &word)) || !word) continue;
                        WinRT_Rect rc;
                        if (SUCCEEDED(VT_OCRWORD(word)->BoundingRect(word, &rc))) {
                            HSTRING ht = NULL;
                            if (SUCCEEDED(VT_OCRWORD(word)->Text(word, &ht)) && ht) {
                                std::wstring ws = HStringToWstr(ht);
                                WindowsDeleteString(ht);
                                if (!ws.empty()) {
                                    TextBlock b;
                                    b.content = W2U8_Conv(ws);
                                    b.x1 = (int)rc.X; b.y1 = (int)rc.Y;
                                    b.x2 = (int)(rc.X + rc.Width); b.y2 = (int)(rc.Y + rc.Height);
                                    b.confidence = 1.0f; // Windows OCR 不返回置信度
                                    out.push_back(b);
                                }
                            }
                        }
                        ((IUnknownVtbl*)((void**)word)[0])->Release(word);
                    }
                }
                ((IUnknownVtbl*)((void**)words)[0])->Release(words);
            }
            ((IUnknownVtbl*)((void**)line)[0])->Release(line);
        }
        ((IUnknownVtbl*)((void**)lines)[0])->Release(lines);
        return true;
    }
};

// ---- 工厂：优先 Windows 原生；失败回退 Tesseract ----
IOcrEngine* CreateOcrEngine() {
    WindowsMediaOcrEngine* w = new WindowsMediaOcrEngine();
    if (w && w->ready()) {
        std::cerr << "[OCR] 使用 Windows 原生 OCR\n";
        return w;
    }
    delete w;
    std::cerr << "[OCR] Windows 原生 OCR 不可用，回退 Tesseract\n";
    return CreateTesseractOcrEngine();
}
