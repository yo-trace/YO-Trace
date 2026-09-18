// YO-Trace 阶段三：Windows.Media.Ocr 的 WinRT ABI 手写配置（MinGW 无 C++/WinRT，故手写）
//
// 说明：MinGW 无法直接编译 C++/WinRT，这里手写调用 Windows.Media.Ocr 所需的 COM/WinRT
// ABI（接口 GUID、vtable 布局、枚举值）。值采用"自 Windows 10 起稳定未变"的 ABI 约定。
// 每一步 COM 调用都做 HRESULT 检查，失败即交回上层回退到 Tesseract。
//
// 参考：windows-rs / Windows SDK IDL（IID 取自 windows-rs，vtable 顺序采用稳定 ABI）。

#pragma once
#include <windows.h>
#include <string>
#include <vector>

#ifndef WINRT_OCR_ABI_H
#define WINRT_OCR_ABI_H

// ---- 基础 WinRT 类型/函数声明（不依赖 winstring.h / roapi.h，手动声明以兼容 MinGW）----
typedef void* HSTRING;
typedef const wchar_t* PCWSTR_;

// RO_INIT 类型
#ifndef RO_INIT_MULTITHREADED
#define RO_INIT_MULTITHREADED 1
#endif
#ifndef RO_INIT_SINGLETHREADED
#define RO_INIT_SINGLETHREADED 0
#endif

extern "C" {
    HRESULT __stdcall RoInitialize(int type);
    void    __stdcall RoUninitialize();
    HRESULT __stdcall RoGetActivationFactory(HSTRING activatableClassId, REFIID iid, void** factory);
    HRESULT __stdcall WindowsCreateString(LPCWSTR sourceString, UINT32 length, HSTRING* string);
    HRESULT __stdcall WindowsDeleteString(HSTRING string);
    PCWSTR  __stdcall WindowsGetStringRawBuffer(HSTRING string, UINT32* length);
}

// ---- GUID 辅助宏 ----
#define WINRT_GUID(name, d1, d2, d3, d4, d5, d6, d7, d8, d9, d10, d11) \
    static const GUID name = { d1, d2, d3, { d4, d5, d6, d7, d8, d9, d10, d11 } }

// ---- 接口 IID（取自 windows-rs，稳定不变）----
WINRT_GUID(IID_IOcrEngine,            0x5a14bc41, 0x5b76, 0x3140, 0xb6, 0x80, 0x88, 0x25, 0x56, 0x26, 0x83, 0xac);
WINRT_GUID(IID_IOcrEngineStatics,     0x5bffa85a, 0x3384, 0x3540, 0x99, 0x40, 0x69, 0x91, 0x20, 0xd4, 0x28, 0xa8);
WINRT_GUID(IID_IOcrResult,            0x9bd235b2, 0x175b, 0x3d6a, 0x92, 0xe2, 0x38, 0x8c, 0x20, 0x6e, 0x2f, 0x63);
WINRT_GUID(IID_IOcrLine,              0x0043a16f, 0xe31f, 0x3a24, 0x89, 0x9c, 0xd4, 0x44, 0xbd, 0x08, 0x81, 0x24);
WINRT_GUID(IID_IOcrWord,              0x3c2a477a, 0x5cd9, 0x3525, 0xba, 0x2a, 0x23, 0xd1, 0xe0, 0xa6, 0x8a, 0x1d);
WINRT_GUID(IID_ISoftwareBitmapStatics,0xdf0385db, 0x672f, 0x4a9d, 0x80, 0x6e, 0xc2, 0x44, 0x2f, 0x34, 0x3e, 0x86);
WINRT_GUID(IID_ILanguageFactory,       0x9b0252ac, 0x0c27, 0x44f8, 0xb7, 0x92, 0x97, 0x93, 0xfb, 0x66, 0xc6, 0x3e);
WINRT_GUID(IID_IBufferByteAccess,     0x905a0fe0, 0xbc53, 0x11df, 0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda);

// 运行时类字符串
static const wchar_t* RUNTIME_OcrEngine  = L"Windows.Media.Ocr.OcrEngine";
static const wchar_t* RUNTIME_SoftwareBitmap = L"Windows.Graphics.Imaging.SoftwareBitmap";
static const wchar_t* RUNTIME_Language    = L"Windows.Globalization.Language";

// ---- 枚举值（稳定 ABI）----
enum BitmapPixelFormat { BpfUnknown = 0, BpfRgba16F = 1, BpfRgba8 = 2, BpfBgra8 = 3,
                         BpfPbgra8 = 4, BpfGray8 = 5, BpfGray16 = 6, BpfYuy2 = 7,
                         BpfNv12 = 8, BpfP010 = 9, BpfP016 = 10, BpfDontCare = 11 };
enum BitmapAlphaMode  { BamUnknown = 0, BamPremultiplied = 1, BamStraight = 2, BamIgnore = 3 };
enum BitmapBufferAccessMode { BbamRead = 0, BbamReadWrite = 1 };

// ---- 结构体 ----
struct WinRT_Rect { float X, Y, Width, Height; };
struct BitmapPlaneDescription { INT32 StartIndex, Width, Height, Stride; };

// ---- vtable 基类 ----
struct IUnknownVtbl {
    HRESULT (STDMETHODCALLTYPE* QueryInterface)(void* self, REFIID riid, void** ppv);
    ULONG  (STDMETHODCALLTYPE* AddRef)(void* self);
    ULONG  (STDMETHODCALLTYPE* Release)(void* self);
};
struct IInspectableVtbl : IUnknownVtbl {
    HRESULT (STDMETHODCALLTYPE* GetIids)(void* self, UINT32* count, IID** iids);
    HRESULT (STDMETHODCALLTYPE* GetRuntimeClassName)(void* self, HSTRING* name);
    HRESULT (STDMETHODCALLTYPE* GetTrustLevel)(void* self, int* level);
};

// ---- 各接口 vtable（稳定 ABI 顺序）----
struct IOcrEngineStatics_Vtbl : IInspectableVtbl {
    HRESULT (STDMETHODCALLTYPE* MaxImageDimension)(void* self, UINT32* out);
    HRESULT (STDMETHODCALLTYPE* AvailableRecognizerLanguages)(void* self, void** out);
    HRESULT (STDMETHODCALLTYPE* IsLanguageSupported)(void* self, void* language, bool* out);
    HRESULT (STDMETHODCALLTYPE* TryCreateFromLanguage)(void* self, void* language, void** outEngine);
    HRESULT (STDMETHODCALLTYPE* TryCreateFromUserProfileLanguages)(void* self, void** outEngine);
};
struct IOcrEngine_Vtbl : IInspectableVtbl {
    HRESULT (STDMETHODCALLTYPE* RecognizeAsync)(void* self, void* bitmap, void** outAsync);
    HRESULT (STDMETHODCALLTYPE* RecognizerLanguage)(void* self, void** outLanguage);
};
struct IOcrResult_Vtbl : IInspectableVtbl {
    HRESULT (STDMETHODCALLTYPE* Lines)(void* self, void** out);
    HRESULT (STDMETHODCALLTYPE* TextAngle)(void* self, void** out);
    HRESULT (STDMETHODCALLTYPE* Text)(void* self, HSTRING* out);
};
struct IOcrLine_Vtbl : IInspectableVtbl {
    HRESULT (STDMETHODCALLTYPE* Words)(void* self, void** out);
    HRESULT (STDMETHODCALLTYPE* Text)(void* self, HSTRING* out);
};
struct IOcrWord_Vtbl : IInspectableVtbl {
    HRESULT (STDMETHODCALLTYPE* BoundingRect)(void* self, WinRT_Rect* out);
    HRESULT (STDMETHODCALLTYPE* Text)(void* self, HSTRING* out);
};
struct ISoftwareBitmapStatics_Vtbl : IInspectableVtbl {
    HRESULT (STDMETHODCALLTYPE* Create)(void* self, INT32 pixelFormat, INT32 width, INT32 height,
                                        INT32 alphaMode, void** outBitmap);
    HRESULT (STDMETHODCALLTYPE* CreateWithAlpha)(void* self, INT32 pixelFormat, INT32 width, INT32 height,
                                                 INT32 alphaMode, void** outBitmap);
    HRESULT (STDMETHODCALLTYPE* Copy)(void* self, void* source, void** outBitmap);
    HRESULT (STDMETHODCALLTYPE* Convert)(void* self, void* source, INT32 format, void** outBitmap);
    HRESULT (STDMETHODCALLTYPE* ConvertWithAlpha)(void* self, void* source, INT32 format, INT32 alpha, void** outBitmap);
    HRESULT (STDMETHODCALLTYPE* CreateCopyFromBuffer)(void* self, void* buffer, INT32 format, INT32 width, INT32 height, void** outBitmap);
};
struct ISoftwareBitmap_Vtbl : IInspectableVtbl {
    HRESULT (STDMETHODCALLTYPE* BitmapPixelFormat)(void* self, INT32* out);
    HRESULT (STDMETHODCALLTYPE* BitmapAlphaMode)(void* self, INT32* out);
    HRESULT (STDMETHODCALLTYPE* PixelWidth)(void* self, INT32* out);
    HRESULT (STDMETHODCALLTYPE* PixelHeight)(void* self, INT32* out);
    HRESULT (STDMETHODCALLTYPE* IsReadOnly)(void* self, bool* out);
    HRESULT (STDMETHODCALLTYPE* SetDpiX)(void* self, double v);
    HRESULT (STDMETHODCALLTYPE* DpiX)(void* self, double* out);
    HRESULT (STDMETHODCALLTYPE* SetDpiY)(void* self, double v);
    HRESULT (STDMETHODCALLTYPE* DpiY)(void* self, double* out);
    HRESULT (STDMETHODCALLTYPE* LockBuffer)(void* self, INT32 mode, void** outBuffer);
    // 其余方法省略（CopyTo 等），不影响索引（通过实际 vtable 偏移访问）
};
struct IBitmapBuffer_Vtbl : IInspectableVtbl {
    HRESULT (STDMETHODCALLTYPE* get_Buffer)(void* self, void** outIBuffer);
    HRESULT (STDMETHODCALLTYPE* get_Capacity)(void* self, UINT32* out);
    HRESULT (STDMETHODCALLTYPE* get_Length)(void* self, UINT32* out);
    HRESULT (STDMETHODCALLTYPE* Unmap)(void* self);
    HRESULT (STDMETHODCALLTYPE* GetPlaneCount)(void* self, INT32* out);
    HRESULT (STDMETHODCALLTYPE* GetPlaneDescription)(void* self, INT32 index, BitmapPlaneDescription* out);
};
struct ILanguageFactory_Vtbl : IInspectableVtbl {
    HRESULT (STDMETHODCALLTYPE* CreateLanguage)(void* self, HSTRING tag, void** outLanguage);
};
struct IAsyncOperationOcrResult_Vtbl : IInspectableVtbl {
    HRESULT (STDMETHODCALLTYPE* get_Completed)(void* self, void** outHandler);
    HRESULT (STDMETHODCALLTYPE* put_Completed)(void* self, void* handler);
    HRESULT (STDMETHODCALLTYPE* GetResults)(void* self, void** outResult);
};
struct IVectorView_Vtbl : IInspectableVtbl {
    HRESULT (STDMETHODCALLTYPE* get_Size)(void* self, UINT32* out);
    HRESULT (STDMETHODCALLTYPE* GetAt)(void* self, UINT32 index, void** outItem);
    HRESULT (STDMETHODCALLTYPE* IndexOf)(void* self, void* value, UINT32* index, bool* found);
    HRESULT (STDMETHODCALLTYPE* GetMany)(void* self, UINT32 start, UINT32 capacity, void** items, UINT32* actual);
};
struct IBufferByteAccess_Vtbl : IUnknownVtbl {
    HRESULT (STDMETHODCALLTYPE* Buffer)(void* self, UINT32* length, BYTE** value);
};

// ---- vtable 访问宏 ----
#define VT_OCRSTATICS(p)  ((IOcrEngineStatics_Vtbl*)((void**)(p))[0])
#define VT_OCRENGINE(p)   ((IOcrEngine_Vtbl*)((void**)(p))[0])
#define VT_OCRRESULT(p)   ((IOcrResult_Vtbl*)((void**)(p))[0])
#define VT_OCRLINE(p)     ((IOcrLine_Vtbl*)((void**)(p))[0])
#define VT_OCRWORD(p)     ((IOcrWord_Vtbl*)((void**)(p))[0])
#define VT_SBSTATICS(p)   ((ISoftwareBitmapStatics_Vtbl*)((void**)(p))[0])
#define VT_SB(p)          ((ISoftwareBitmap_Vtbl*)((void**)(p))[0])
#define VT_BB(p)          ((IBitmapBuffer_Vtbl*)((void**)(p))[0])
#define VT_LANG_FACT(p)   ((ILanguageFactory_Vtbl*)((void**)(p))[0])
#define VT_ASYNC(p)       ((IAsyncOperationOcrResult_Vtbl*)((void**)(p))[0])
#define VT_VEC(p)         ((IVectorView_Vtbl*)((void**)(p))[0])
#define VT_BBA(p)         ((IBufferByteAccess_Vtbl*)((void**)(p))[0])

// HSTRING 辅助
static inline std::wstring HStringToWstr(HSTRING h) {
    if (!h) return L"";
    UINT32 len = 0;
    PCWSTR_ p = WindowsGetStringRawBuffer(h, &len);
    return (p && len) ? std::wstring(p, len) : std::wstring();
}
static inline HRESULT MakeHString(const std::wstring& s, HSTRING* out) {
    return WindowsCreateString(s.c_str(), (UINT32)s.size(), out);
}

#endif // WINRT_OCR_ABI_H
