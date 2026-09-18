/*
 * sqlite3.h —— 本地兼容垫片（shim）
 *
 * 背景：sqlite.org 在沙箱内不可达，无法下载官方 amalgamation（sqlite3.c + sqlite3.h）。
 *       本机已存在可用的 sqlite3.dll（Python 自带，导出完整 sqlite3_* API），故此处
 *       提供一份“仅包含本项目所用 API 子集”的头文件，使阶段二可直接编译并链接该系统 DLL。
 *
 * 切换到官方源码（路线图任务 2.1 的标准做法）：
 *   1) 下载 https://www.sqlite.org/<年份>/sqlite-amalgamation-XXXXXXX.zip
 *   2) 将其中的 sqlite3.c 与 sqlite3.h 放到本目录，覆盖本垫片文件
 *   3) 编译时改为把 sqlite3.c 加入工程（见 .vscode/tasks.json 注释），不再链接外部 DLL
 *   届时本垫片即可删除，且 API 行为完全一致（签名与官方一致）。
 */
#ifndef SQLITE3_SHIM_H
#define SQLITE3_SHIM_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
typedef long long sqlite3_int64;
typedef void (*sqlite3_destructor_type)(void*);

#define SQLITE_OK    0
#define SQLITE_ERROR 1
#define SQLITE_DONE  101
#define SQLITE_ROW   100
#define SQLITE_TRANSIENT ((sqlite3_destructor_type)-1)

int sqlite3_open(const char *filename, sqlite3 **ppDb);
int sqlite3_close(sqlite3 *db);
int sqlite3_exec(sqlite3 *db, const char *sql,
                 int (*callback)(void*, int, char**, char**), void *arg, char **errmsg);
int sqlite3_prepare_v2(sqlite3 *db, const char *zSql, int nByte,
                       sqlite3_stmt **ppStmt, const char **pzTail);
int sqlite3_bind_text(sqlite3_stmt *stmt, int i, const char *text, int n,
                      void (*xDel)(void*));
int sqlite3_bind_int(sqlite3_stmt *stmt, int i, int val);
int sqlite3_bind_int64(sqlite3_stmt *stmt, int i, sqlite3_int64 val);
int sqlite3_bind_double(sqlite3_stmt *stmt, int i, double val);
int sqlite3_step(sqlite3_stmt *stmt);
int sqlite3_column_int(sqlite3_stmt *stmt, int iCol);
sqlite3_int64 sqlite3_column_int64(sqlite3_stmt *stmt, int iCol);
double sqlite3_column_double(sqlite3_stmt *stmt, int iCol);
const unsigned char *sqlite3_column_text(sqlite3_stmt *stmt, int iCol);
int sqlite3_column_count(sqlite3_stmt *stmt);
int sqlite3_finalize(sqlite3_stmt *stmt);
sqlite3_int64 sqlite3_last_insert_rowid(sqlite3 *db);
const char *sqlite3_errmsg(sqlite3 *db);
void sqlite3_free(void *p);
int sqlite3_reset(sqlite3_stmt *stmt);
int sqlite3_clear_bindings(sqlite3_stmt *stmt);

#ifdef __cplusplus
}
#endif
#endif /* SQLITE3_SHIM_H */
