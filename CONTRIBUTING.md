# Contributing to YO-Trace

Thanks for your interest in contributing! This document explains how to get involved.

## Getting started

1. Fork the repository and clone your fork:

   ```bash
   git clone https://github.com/<your-username>/YO-Trace.git
   cd YO-Trace
   ```

2. Add the upstream remote:

   ```bash
   git remote add upstream https://github.com/yo-trace/YO-Trace.git
   ```

3. Create a branch for your work:

   ```bash
   git checkout -b my-feature
   ```

## Development environment

- **OS**: Windows 10 / 11 (the native OCR engine relies on the Windows WinRT OCR API).
- **Toolchain**: MinGW-w64 with g++ 10+.
- **Optional**: Tesseract runtime, used only as a fallback OCR engine.

See [README.md](README.md#build) for the exact build commands.

## Build & run

```bash
g++.exe -std=c++17 -O2 -static ^
  -I src/phase2 -I src/phase2/third_party/sqlite -I src/phase3 ^
  src/phase2/sqlite_storage.cpp ^
  src/phase3/screen_capture.cpp ^
  src/phase3/ocr_windows.cpp ^
  src/phase3/ocr_tesseract.cpp ^
  src/phase3/main.cpp ^
  -L src/phase2/third_party/sqlite -lsqlite3 -lgdi32 -lruntimeobject -lole32 ^
  -o build/yotrace_phase3.exe
```

Run the built `yotrace_phase3.exe`; it captures in the background and writes to `yotrace_phase3.db`. Use `yotrace_query.exe <keyword>` to search.

> Note: `yotrace_phase3.db` and `yotrace_phase3.log` contain your local screen-capture data. **Do not** commit them — they are already covered by `.gitignore`.

## Code style

- C++17, prefer clear and minimal code.
- Keep Windows-specific code (WinRT ABI, GDI capture) isolated under `src/phase3/`.
- Comments and identifiers may be in Chinese or English; keep them consistent within a file.
- No unnecessary abstractions; follow the existing structure of `src/phase1..phase3`.

## Commit messages

- Use clear, imperative English commit messages (e.g. `fix: handle empty snapshot queue`, `feat: add UIA control tree`).
- Keep each commit focused on a single change.

## Pull requests

1. Rebase your branch onto the latest `upstream/main` before opening a PR.
2. Make sure the project still builds.
3. Describe what your PR does and why, referencing any related issue (e.g. `Closes #12`).
4. If your change affects OCR behavior or the on-disk database schema, call it out explicitly.

## Reporting issues

Please use the provided issue templates (Bug report / Feature request). Include your OS version, the YO-Trace commit, and which OCR engine was used. Attach `yotrace_phase3.log` (with any personal text redacted) when relevant.

## Language

The project is bilingual: the default README is English and a Chinese version lives in `README_CN.md`. Issues and pull requests may be written in **Chinese or English** — both are welcome.

## Code of Conduct

Be respectful and constructive. By participating you agree to uphold a welcoming, harassment-free community.
