cmake_minimum_required(VERSION 3.15)
project(YOTrace CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 源码含中文注释/字符串，确保 MSVC 以 UTF-8 解释
if(MSVC)
    add_compile_options(/utf-8)
endif()

# ---------- SQLite 链接 ----------
# 仓库里的 libsqlite3.a 是 MinGW 导入库，MSVC 不认。
# 复用仓库自带的 sqlite3.def，经 MSVC 的 lib.exe 生成导入库，零下载、复用 dist/sqlite3.dll。
set(SQLITE_DEF ${CMAKE_SOURCE_DIR}/src/phase2/third_party/sqlite/sqlite3.def)
if(MSVC)
    if(CMAKE_GENERATOR_PLATFORM STREQUAL "x64")
        set(SQLITE_MACHINE x64)
    else()
        set(SQLITE_MACHINE x86)
    endif()
    set(SQLITE_LIB ${CMAKE_BINARY_DIR}/sqlite3.lib)
    find_program(LIB_EXE NAMES lib lib.exe)
    if(LIB_EXE)
        execute_process(
            COMMAND ${LIB_EXE} /def:${SQLITE_DEF} /machine:${SQLITE_MACHINE} /out:${SQLITE_LIB}
            RESULT_VARIABLE _lib_res
        )
        if(NOT _lib_res EQUAL 0)
            message(WARNING "生成 sqlite3.lib 失败（lib.exe 返回 ${_lib_res}）。请确认在 Visual Studio 开发者命令提示符中运行 cmake，且架构(${SQLITE_MACHINE}) 与 dist/sqlite3.dll 一致。")
        endif()
    else()
        message(WARNING "未找到 lib.exe：请在 Visual Studio 开发者命令提示符中运行 cmake 以生成 sqlite3.lib。")
    endif()
else()
    # 非 MSVC（如 MinGW）直接用仓库现有导入库
    set(SQLITE_LIB ${CMAKE_SOURCE_DIR}/src/phase2/third_party/sqlite/libsqlite3.a)
endif()

set(YO_INC
    ${CMAKE_SOURCE_DIR}/src/phase2
    ${CMAKE_SOURCE_DIR}/src/phase2/third_party/sqlite
    ${CMAKE_SOURCE_DIR}/src/phase3
)

# ---------- yotrace_phase3（主程序：采集 + OCR + 控件树）----------
set(YO_PHASE3_SRCS
    src/phase2/sqlite_storage.cpp
    src/phase3/screen_capture.cpp
    src/phase3/ocr_windows.cpp
    src/phase3/ocr_tesseract.cpp
    src/phase3/main.cpp
)
# UIA 控件树抓取依赖官方 uiautomation.h（随 Windows SDK / MSVC）。
# MinGW 不含该头文件，故仅在 MSVC 下加入；MinGW 构建不包含控件树功能。
if(MSVC)
    list(APPEND YO_PHASE3_SRCS src/phase3/uia_capture.cpp)
endif()
add_executable(yotrace_phase3 ${YO_PHASE3_SRCS})
target_include_directories(yotrace_phase3 PRIVATE ${YO_INC})
target_link_libraries(yotrace_phase3
    ${SQLITE_LIB}
    gdi32 runtimeobject ole32 oleaut32 shell32 psapi
)

# ---------- yotrace_query（查询工具）----------
add_executable(yotrace_query
    src/phase2/sqlite_storage.cpp
    src/phase3/query_test.cpp
)
target_include_directories(yotrace_query PRIVATE ${YO_INC})
target_link_libraries(yotrace_query
    ${SQLITE_LIB}
    shell32
)

# ---------- 运行时：复制 sqlite3.dll 到输出目录（本地 dist/ 存在时）----------
if(EXISTS ${CMAKE_SOURCE_DIR}/dist/sqlite3.dll)
    add_custom_command(TARGET yotrace_phase3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/dist/sqlite3.dll $<TARGET_FILE_DIR:yotrace_phase3>
        COMMENT "Copy sqlite3.dll -> build output")
    add_custom_command(TARGET yotrace_query POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/dist/sqlite3.dll $<TARGET_FILE_DIR:yotrace_query>
        COMMENT "Copy sqlite3.dll -> build output")
endif()
