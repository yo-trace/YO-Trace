// YO-Trace 阶段二：SQLite 存储层封装
#pragma once
#include <string>
#include <vector>
#include "sqlite3.h"
#include "text_block.h"

struct WindowRow {
    long long id = 0;
    long long snapshot_id = 0;
    std::string title;      // UTF-8
    std::string app_name;   // UTF-8
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
};

struct SnapshotRow {
    long long id = 0;
    std::string timestamp;  // 文本时间戳
};

class TraceDB {
public:
    ~TraceDB() { close(); }

    // 打开（不存在则创建）数据库文件
    bool open(const std::string& path);

    // 关闭
    void close();

    // 建表：snapshots / windows / text_blocks(预留)
    bool createSchema();

    // 写入一个快照（一条 snapshots + N 条 windows），整体包在事务里。
    // elapsedMs 返回耗时（毫秒）。成功返回 true。
    bool insertSnapshot(const std::string& timestamp,
                        const std::vector<WindowRow>& wins,
                        double& elapsedMs);

    // 2.4 按时间范围查询窗口记录（timestamp 在 [t0, t1] 之间）
    std::vector<WindowRow> queryByTimeRange(const std::string& t0,
                                            const std::string& t1);

    // 2.4 按窗口标题模糊查询（LIKE %kw%）
    std::vector<WindowRow> queryByTitleLike(const std::string& keyword);

    // 阶段三：写入一个快照对应的文本块（text_blocks 表）
    bool insertTextBlocks(long long snapshotId,
                          const std::vector<TextBlock>& blocks,
                          double& elapsedMs);

    // 阶段三：按文本内容模糊查询 text_blocks
    std::vector<TextBlock> queryTextBlocksByContent(const std::string& keyword);

    // 最近一次写入的 snapshot id（用于去重/调试）
    long long lastSnapshotId() const { return last_snapshot_id_; }

    const char* lastError() const { return last_err_; }

private:
    sqlite3* db_ = nullptr;
    long long last_snapshot_id_ = 0;
    const char* last_err_ = nullptr;
};
