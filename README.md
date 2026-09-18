English | [简体中文](README_CN.md)

# YO-Trace

> A screen-memory search tool — periodically captures your Windows screen windows and text, runs local OCR, stores results in SQLite, and provides a command-line tool to search by content.

YO-Trace keeps "watching" your screen in the background: every few seconds it captures the visible windows, runs OCR (Optical Character Recognition) over the full screen, and writes the recognized text blocks (with coordinates and confidence) together with window snapshots into a local database. Later you can search for any text that has ever appeared on screen using keywords (e.g. "WeChat", "meeting notes") — essentially a **searchable screen memory**.

## Features

- **Window-level capture**: enumerates visible windows (title, owning process, position) and stores them per snapshot.
- **Full-screen OCR**:
  - The primary engine is **Windows native OCR** (`Windows.Media.Ocr`, WinRT). On MinGW it is invoked through a **hand-written WinRT ABI**, with no C++/WinRT compiler required and **no third-party OCR runtime to install**.
  - When the native engine is unavailable (non-Win10/11, missing OCR language pack, etc.), it **automatically falls back to Tesseract** (the `tesseract.exe` subprocess).
- **Local-first / privacy-friendly**: all data lives only in a local SQLite file; nothing is uploaded to any server.
- **Idle-window batch processing**: screenshots from high-frequency changes are queued first and recognized in the background during "idle windows" (first 5 minutes of every hour, or 60 seconds without change) to avoid pegging the CPU with repeated OCR.
- **System-tray resident**: the main program has no main window; it lives in the system tray. F12 triggers an immediate capture, right-click the tray icon to exit.
- **Command-line search**: `yotrace_query.exe <keyword>` fuzzy-searches historical text blocks by content.

## Requirements

- Windows 10 / 11 (native OCR needs the system's built-in OCR language pack; Chinese/English are available by default).
- MinGW-w64 toolchain (g++ 10+, to build from source).
- (Optional) Tesseract runtime, used only as a fallback when native OCR is unavailable.

## Build

The repository ships SQLite's import library and headers (`src/phase2/third_party/sqlite/`), so no extra download is needed to compile.

Using MinGW-w64 `g++`:

```bash
# Phase 3 main program
g++.exe -std=c++17 -O2 -static ^
  -I src/phase2 -I src/phase2/third_party/sqlite -I src/phase3 ^
  src/phase2/sqlite_storage.cpp ^
  src/phase3/screen_capture.cpp ^
  src/phase3/ocr_windows.cpp ^
  src/phase3/ocr_tesseract.cpp ^
  src/phase3/main.cpp ^
  -L src/phase2/third_party/sqlite -lsqlite3 -lgdi32 -lruntimeobject -lole32 ^
  -o build/yotrace_phase3.exe

# Query tool
g++.exe -std=c++17 -O2 -static ^
  -I src/phase2 -I src/phase2/third_party/sqlite -I src/phase3 ^
  src/phase2/sqlite_storage.cpp ^
  src/phase3/query_test.cpp ^
  -L src/phase2/third_party/sqlite -lsqlite3 -lshell32 ^
  -o build/yotrace_query.exe
```

> Linking notes: `-lruntimeobject` (the `Ro*` WinRT runtime functions), `-lole32` (`CoTaskMem*`), `-lgdi32` (screen capture), `-lshell32` (`CommandLineToArgvW`).
> The project's SQLite is an **import library** (`libsqlite3.a` pointing at `sqlite3.dll`), so at runtime `sqlite3.dll` must sit in the same directory (see Releases below).

## Usage

1. Double-click `yotrace_phase3.exe` to start (no main window; a tray icon appears).
2. The program captures automatically every 5 seconds; press **F12** for an immediate capture; right-click the tray icon to exit.
3. Data is written to `yotrace_phase3.db` in the same directory; the run log goes to `yotrace_phase3.log`.
4. Open a command line in that directory and search:

   ```bash
   yotrace_query.exe             # list all recognized text blocks
   yotrace_query.exe WeChat      # search by keyword
   ```

   > The query tool reads `yotrace_phase3.db` from **its own directory** and depends on `sqlite3.dll` in the same directory.

## Releases / Prebuilt binaries

This repository contains **source code and docs only**. Build artifacts (`*.exe`/`*.dll`), the Tesseract fallback runtime (`dist/tesseract/`), and `sqlite3.dll` are distributed via **GitHub Releases** and are not tracked in the repository (see `.gitignore`).

## Project structure

```
YO-Trace/
├── src/
│   ├── phase1/                # window enumeration & snapshot capture (basics)
│   ├── phase2/                # SQLite storage layer + window queries
│   │   └── third_party/sqlite/  # SQLite import lib + headers (shipped with repo)
│   └── phase3/                # screen capture + OCR (native/Tesseract) + idle batch + query CLI
├── docs/
│   └── 需求场景.md            # requirements & scenarios (Chinese)
├── YO-Trace 任务清单.md        # development task list (Chinese)
├── README.md
├── README_CN.md
├── LICENSE
└── .gitignore
```

## How it works (brief)

1. **Capture**: `ScreenCapture` grabs the full screen into a bitmap; the window manager enumerates visible windows and crops them.
2. **Recognize**: `OcrEngine` prefers `WindowsMediaOcrEngine` (hand-written WinRT ABI calling `Windows.Media.Ocr`, single-threaded apartment STA + message-pump wait for the async `RecognizeAsync`), falling back to `TesseractOcrEngine` (subprocess) on failure.
3. **Store**: `TraceDB` (SQLite) writes `snapshots` / `windows` / `text_blocks` in a transaction, with all text stored as **UTF-8**.
4. **Search**: `queryTextBlocksByContent` uses `LIKE` fuzzy matching (input is correctly transcoded UTF-16 → UTF-8, avoiding the mojibake / match failures caused by the console's GBK codepage).

## License

[MIT](LICENSE) — see the `LICENSE` file for details.
