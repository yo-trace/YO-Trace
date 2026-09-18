# YO-Trace

> 屏幕记忆搜索工具 —— 周期性采集 Windows 屏幕窗口与文字，本地 OCR 识别后存入 SQLite，并提供按内容搜索的命令行工具。

YO-Trace 在后台持续「观察」你的屏幕：每隔几秒采集一次可见窗口，对全屏做 OCR（光学字符识别），把识别出的文字块（含坐标与置信度）和窗口快照写入本地数据库。之后你可以用关键字（如「微信」「会议纪要」）检索任何曾经出现在屏幕上的文字，相当于一个**可搜索的屏幕记忆**。

## 特性

- **窗口级采集**：枚举当前可见窗口（标题、所属程序、位置），按快照入库。
- **全屏 OCR**：
  - 主引擎为 **Windows 原生 OCR**（`Windows.Media.Ocr`，WinRT）。在 MinGW 下通过**手写 WinRT ABI** 直接调用，无需 C++/WinRT 编译器支持，也**无需安装任何第三方 OCR 运行时**。
  - 当原生引擎不可用（非 Win10/11、缺少 OCR 语言包等）时，**自动回退到 Tesseract**（子进程 `tesseract.exe`）。
- **本地优先 / 隐私友好**：所有数据仅存于本机 SQLite 文件，不上传任何服务器。
- **空闲批量补处理**：高频变化时的截图先入队，在「空闲窗口」（每小时前 5 分钟或连续 60 秒无变化）于后台批量识别，避免反复 OCR 打满 CPU。
- **托盘常驻**：主程序无主窗口，常驻系统托盘，F12 立即触发一次采集，右键退出。
- **命令行检索**：`yotrace_query.exe <关键字>` 按内容模糊搜索历史文本块。

## 系统要求

- Windows 10 / 11（原生 OCR 需要系统自带 OCR 语言包；中文/英文默认可用）。
- MinGW-w64 工具链（g++ 10+，用于从源码构建）。
- （可选）Tesseract 运行时，仅当原生 OCR 不可用时作为回退。

## 构建

仓库已内置 SQLite 的导入库与头文件（`src/phase2/third_party/sqlite/`），无需额外下载即可编译。

使用 MinGW-w64 的 `g++`：

```bash
# 阶段三主程序
g++.exe -std=c++17 -O2 -static ^
  -I src/phase2 -I src/phase2/third_party/sqlite -I src/phase3 ^
  src/phase2/sqlite_storage.cpp ^
  src/phase3/screen_capture.cpp ^
  src/phase3/ocr_windows.cpp ^
  src/phase3/ocr_tesseract.cpp ^
  src/phase3/main.cpp ^
  -L src/phase2/third_party/sqlite -lsqlite3 -lgdi32 -lruntimeobject -lole32 ^
  -o build/yotrace_phase3.exe

# 查询工具
g++.exe -std=c++17 -O2 -static ^
  -I src/phase2 -I src/phase2/third_party/sqlite -I src/phase3 ^
  src/phase2/sqlite_storage.cpp ^
  src/phase3/query_test.cpp ^
  -L src/phase2/third_party/sqlite -lsqlite3 -lshell32 ^
  -o build/yotrace_query.exe
```

> 链接说明：`-lruntimeobject`（`Ro*` WinRT 运行时函数）、`-lole32`（`CoTaskMem*`）、`-lgdi32`（屏幕截取）、`-lshell32`（`CommandLineToArgvW`）。
> 项目自带的是 SQLite **导入库**（`libsqlite3.a` 指向 `sqlite3.dll`），因此运行时需与 `sqlite3.dll` 放在同一目录（见下方「发布」）。

## 使用

1. 双击 `yotrace_phase3.exe` 启动（无主窗口，托盘出现图标）。
2. 程序每 5 秒自动采集；按 **F12** 立即触发一次；右键托盘图标退出。
3. 数据写入同目录的 `yotrace_phase3.db`，运行日志写入 `yotrace_phase3.log`。
4. 打开命令行进入该程序所在目录，进行检索：

   ```bash
   yotrace_query.exe             # 列出全部已识别文本块
   yotrace_query.exe 微信        # 按关键字搜索
   ```

   > 查询工具从**自身所在目录**读取 `yotrace_phase3.db`，并依赖同目录的 `sqlite3.dll`。

## 发布 / 预编译包

本仓库只包含**源代码与文档**。编译产物（`*.exe`/`*.dll`）、Tesseract 回退运行时（`dist/tesseract/`）以及 `sqlite3.dll` 通过 **GitHub Releases** 分发，不纳入版本库（见 `.gitignore`）。

## 项目结构

```
YO-Trace/
├── src/
│   ├── phase1/                # 窗口枚举与快照采集（基础能力）
│   ├── phase2/                # SQLite 存储层 + 窗口查询
│   │   └── third_party/sqlite/  # SQLite 导入库 + 头文件（随仓库提供）
│   └── phase3/                # 屏幕截取 + OCR(原生/Tesseract) + 空闲批量处理 + 查询 CLI
├── docs/
│   └── 需求场景.md            # 需求与场景说明
├── YO-Trace 任务清单.md        # 开发任务清单
├── README.md
├── LICENSE
└── .gitignore
```

## 工作原理（简述）

1. **采集**：`ScreenCapture` 截取全屏为位图；窗口管理器枚举可见窗口并裁剪。
2. **识别**：`OcrEngine` 优先 `WindowsMediaOcrEngine`（手写 WinRT ABI 调用 `Windows.Media.Ocr`，
   单线程套间 STA + 消息泵等待异步 `RecognizeAsync`），失败时回退 `TesseractOcrEngine`（子进程）。
3. **存储**：`TraceDB`（SQLite）以事务写入 `snapshots` / `windows` / `text_blocks`，
   文本统一以 **UTF-8** 存储。
4. **检索**：`queryTextBlocksByContent` 用 `LIKE` 模糊匹配（输入参数经 UTF-16→UTF-8 正确转码，
   规避控制台 GBK 代码页导致的中文匹配/显示乱码）。

## 许可证

[MIT](LICENSE) —— 详见 `LICENSE` 文件。
