// YO-Trace 阶段二：SQLite 存储层实现
#include "sqlite_storage.h"
#include <chrono>

bool TraceDB::open(const std::string& path) {
    int rc = sqlite3_open(path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        last_err_ = sqlite3_errmsg(db_);
        return false;
    }
    last_err_ = nullptr;
    return true;
}

void TraceDB::close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool TraceDB::createSchema() {
    const char* schema =
        "CREATE TABLE IF NOT EXISTS snapshots("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  timestamp TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS windows("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  snapshot_id INTEGER NOT NULL,"
        "  title TEXT,"
        "  app_name TEXT,"
        "  x1 INTEGER, y1 INTEGER, x2 INTEGER, y2 INTEGER,"
        "  FOREIGN KEY(snapshot_id) REFERENCES snapshots(id));"
        "CREATE TABLE IF NOT EXISTS text_blocks("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  snapshot_id INTEGER,"
        "  content TEXT,"
        "  confidence REAL,"
        "  x1 INTEGER, y1 INTEGER, x2 INTEGER, y2 INTEGER,"
        "  FOREIGN KEY(snapshot_id) REFERENCES snapshots(id));"
        "CREATE INDEX IF NOT EXISTS idx_windows_snapshot ON windows(snapshot_id);"
        "CREATE INDEX IF NOT EXISTS idx_windows_title ON windows(title);";
    char* err = nullptr;
    int rc = sqlite3_exec(db_, schema, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        last_err_ = err ? err : "createSchema failed";
        sqlite3_free(err);
        return false;
    }
    last_err_ = nullptr;
    return true;
}

bool TraceDB::insertSnapshot(const std::string& timestamp,
                             const std::vector<WindowRow>& wins,
                             double& elapsedMs) {
    auto t0 = std::chrono::high_resolution_clock::now();

    // 开事务，保证整批原子写入并大幅提升性能
    if (sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr) != SQLITE_OK) {
        last_err_ = "BEGIN failed";
        return false;
    }

    const char* sqlSnap = "INSERT INTO snapshots(timestamp) VALUES(?)";
    sqlite3_stmt* stmtSnap = nullptr;
    int rc = sqlite3_prepare_v2(db_, sqlSnap, -1, &stmtSnap, nullptr);
    if (rc != SQLITE_OK) {
        last_err_ = sqlite3_errmsg(db_);
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }
    sqlite3_bind_text(stmtSnap, 1, timestamp.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmtSnap);
    sqlite3_finalize(stmtSnap);
    if (rc != SQLITE_DONE) {
        last_err_ = sqlite3_errmsg(db_);
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }
    long long snapId = sqlite3_last_insert_rowid(db_);

    const char* sqlWin =
        "INSERT INTO windows(snapshot_id,title,app_name,x1,y1,x2,y2) "
        "VALUES(?,?,?,?,?,?,?)";
    sqlite3_stmt* stmtWin = nullptr;
    rc = sqlite3_prepare_v2(db_, sqlWin, -1, &stmtWin, nullptr);
    if (rc != SQLITE_OK) {
        last_err_ = sqlite3_errmsg(db_);
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }
    for (const auto& w : wins) {
        sqlite3_bind_int64(stmtWin, 1, snapId);
        sqlite3_bind_text(stmtWin, 2, w.title.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmtWin, 3, w.app_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmtWin, 4, w.x1);
        sqlite3_bind_int(stmtWin, 5, w.y1);
        sqlite3_bind_int(stmtWin, 6, w.x2);
        sqlite3_bind_int(stmtWin, 7, w.y2);
        if (sqlite3_step(stmtWin) != SQLITE_DONE) {
            last_err_ = sqlite3_errmsg(db_);
            sqlite3_finalize(stmtWin);
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            return false;
        }
        sqlite3_reset(stmtWin);
        sqlite3_clear_bindings(stmtWin);
    }
    sqlite3_finalize(stmtWin);

    if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        last_err_ = "COMMIT failed";
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    elapsedMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    last_snapshot_id_ = snapId;
    last_err_ = nullptr;
    return true;
}

std::vector<WindowRow> TraceDB::queryByTimeRange(const std::string& t0,
                                                 const std::string& t1) {
    std::vector<WindowRow> out;
    const char* sql =
        "SELECT w.id,w.snapshot_id,w.title,w.app_name,w.x1,w.y1,w.x2,w.y2 "
        "FROM windows w JOIN snapshots s ON w.snapshot_id=s.id "
        "WHERE s.timestamp BETWEEN ? AND ? ORDER BY s.id DESC, w.id ASC";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_text(stmt, 1, t0.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, t1.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        WindowRow r;
        r.id = sqlite3_column_int64(stmt, 0);
        r.snapshot_id = sqlite3_column_int64(stmt, 1);
        const unsigned char* t = sqlite3_column_text(stmt, 2);
        const unsigned char* a = sqlite3_column_text(stmt, 3);
        r.title = t ? (const char*)t : "";
        r.app_name = a ? (const char*)a : "";
        r.x1 = sqlite3_column_int(stmt, 4);
        r.y1 = sqlite3_column_int(stmt, 5);
        r.x2 = sqlite3_column_int(stmt, 6);
        r.y2 = sqlite3_column_int(stmt, 7);
        out.push_back(r);
    }
    sqlite3_finalize(stmt);
    return out;
}

std::vector<WindowRow> TraceDB::queryByTitleLike(const std::string& keyword) {
    std::vector<WindowRow> out;
    std::string like = "%" + keyword + "%";
    const char* sql =
        "SELECT id,snapshot_id,title,app_name,x1,y1,x2,y2 "
        "FROM windows WHERE title LIKE ? ORDER BY id DESC";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_text(stmt, 1, like.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        WindowRow r;
        r.id = sqlite3_column_int64(stmt, 0);
        r.snapshot_id = sqlite3_column_int64(stmt, 1);
        const unsigned char* t = sqlite3_column_text(stmt, 2);
        const unsigned char* a = sqlite3_column_text(stmt, 3);
        r.title = t ? (const char*)t : "";
        r.app_name = a ? (const char*)a : "";
        r.x1 = sqlite3_column_int(stmt, 4);
        r.y1 = sqlite3_column_int(stmt, 5);
        r.x2 = sqlite3_column_int(stmt, 6);
        r.y2 = sqlite3_column_int(stmt, 7);
        out.push_back(r);
    }
    sqlite3_finalize(stmt);
    return out;
}

bool TraceDB::insertTextBlocks(long long snapshotId,
                               const std::vector<TextBlock>& blocks,
                               double& elapsedMs) {
    auto t0 = std::chrono::high_resolution_clock::now();
    if (sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr) != SQLITE_OK) {
        last_err_ = "BEGIN failed";
        return false;
    }
    const char* sql =
        "INSERT INTO text_blocks(snapshot_id,content,confidence,x1,y1,x2,y2) "
        "VALUES(?,?,?,?,?,?,?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        last_err_ = sqlite3_errmsg(db_);
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }
    for (const auto& b : blocks) {
        sqlite3_bind_int64(stmt, 1, snapshotId);
        sqlite3_bind_text(stmt, 2, b.content.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 3, b.confidence);
        sqlite3_bind_int(stmt, 4, b.x1);
        sqlite3_bind_int(stmt, 5, b.y1);
        sqlite3_bind_int(stmt, 6, b.x2);
        sqlite3_bind_int(stmt, 7, b.y2);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            last_err_ = sqlite3_errmsg(db_);
            sqlite3_finalize(stmt);
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            return false;
        }
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
    }
    sqlite3_finalize(stmt);
    if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        last_err_ = "COMMIT failed";
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    elapsedMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    last_err_ = nullptr;
    return true;
}

std::vector<TextBlock> TraceDB::queryTextBlocksByContent(const std::string& keyword) {
    std::vector<TextBlock> out;
    std::string like = "%" + keyword + "%";
    const char* sql =
        "SELECT id,snapshot_id,content,confidence,x1,y1,x2,y2 "
        "FROM text_blocks WHERE content LIKE ? ORDER BY id DESC";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_text(stmt, 1, like.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        TextBlock b;
        b.content = (const char*)sqlite3_column_text(stmt, 2);
        b.confidence = sqlite3_column_double(stmt, 3);
        b.x1 = sqlite3_column_int(stmt, 4);
        b.y1 = sqlite3_column_int(stmt, 5);
        b.x2 = sqlite3_column_int(stmt, 6);
        b.y2 = sqlite3_column_int(stmt, 7);
        out.push_back(b);
    }
    sqlite3_finalize(stmt);
    return out;
}
