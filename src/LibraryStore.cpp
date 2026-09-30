/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/File.h"
#include "base/Win.h"

#include "sqlite3.h"

#include "LibraryStore.h"
#include "SumatraLog.h"

struct LibraryStore {
    sqlite3* db = nullptr;
    Str error;
};

static void SetError(LibraryStore* store, Str context) {
    if (!store) {
        return;
    }
    const char* msg = store->db ? sqlite3_errmsg(store->db) : "database is not open";
    str::ReplaceWithCopy(&store->error, fmt("%s: %s", context, Str(msg)));
    logf("LibraryStore: %s\n", store->error);
}

static bool Exec(LibraryStore* store, const char* sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(store->db, sql, nullptr, nullptr, &err);
    if (rc == SQLITE_OK) {
        return true;
    }
    Str detail = err ? Str(err) : Str(sqlite3_errmsg(store->db));
    str::ReplaceWithCopy(&store->error, detail);
    logf("LibraryStore SQL error: %s\n", detail);
    sqlite3_free(err);
    return false;
}

static TempStr NormalizePathTemp(Str path) {
    return path::NormalizeTemp(path);
}

static Str PathKey(Str path) {
    TempStr normalized = NormalizePathTemp(path);
    return str::ToLower(normalized);
}

static void BindText(sqlite3_stmt* stmt, int col, Str value) {
    sqlite3_bind_text(stmt, col, value.s, len(value), SQLITE_TRANSIENT);
}

static sqlite3_stmt* Prepare(LibraryStore* store, const char* sql) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        SetError(store, StrL("prepare"));
        return nullptr;
    }
    return stmt;
}

static Str ColumnTextDup(sqlite3_stmt* stmt, int col) {
    const char* s = (const char*)sqlite3_column_text(stmt, col);
    int n = sqlite3_column_bytes(stmt, col);
    return s ? str::Dup(Str(s, n)) : Str();
}

static i64 NextSortPos(LibraryStore* store, i64 collectionId);
static int CmpManualOrder(LibraryBook* a, LibraryBook* b);

static LibraryBook* ReadBook(sqlite3_stmt* stmt) {
    auto* book = new LibraryBook();
    book->id = sqlite3_column_int64(stmt, 0);
    book->path = ColumnTextDup(stmt, 1);
    book->title = ColumnTextDup(stmt, 2);
    book->openCount = sqlite3_column_int64(stmt, 3);
    book->readingSeconds = sqlite3_column_int64(stmt, 4);
    book->lastReadMs = sqlite3_column_int64(stmt, 5);
    book->sortPos = sqlite3_column_count(stmt) > 6 ? sqlite3_column_int64(stmt, 6) : 0;
    book->bgColor = sqlite3_column_count(stmt) > 7 ? (u32)sqlite3_column_int64(stmt, 7) : 0;
    book->notebooklm = sqlite3_column_count(stmt) > 8 ? ColumnTextDup(stmt, 8) : Str{};
    if (sqlite3_column_count(stmt) > 9) {
        int k = sqlite3_column_int(stmt, 9);
        book->kind = k == (int)LibraryBookKind::Web ? LibraryBookKind::Web : LibraryBookKind::Pdf;
    }
    book->url = sqlite3_column_count(stmt) > 10 ? ColumnTextDup(stmt, 10) : Str{};
    return book;
}

void DeleteLibraryBook(LibraryBook* book) {
    if (!book) {
        return;
    }
    str::Free(book->path);
    str::Free(book->title);
    str::Free(book->url);
    str::Free(book->notebooklm);
    delete book;
}

void DeleteLibraryBooks(Vec<LibraryBook*>& books) {
    for (LibraryBook* book : books) {
        DeleteLibraryBook(book);
    }
    books.Reset();
}

void DeleteLibraryCollection(LibraryCollection* collection) {
    if (!collection) {
        return;
    }
    str::Free(collection->name);
    delete collection;
}

void DeleteLibraryCollections(Vec<LibraryCollection*>& collections) {
    for (LibraryCollection* collection : collections) {
        DeleteLibraryCollection(collection);
    }
    collections.Reset();
}

void DeleteLibraryPathChange(LibraryPathChange* change) {
    if (!change) {
        return;
    }
    str::Free(change->oldPath);
    str::Free(change->newPath);
    delete change;
}

void DeleteLibraryPathChanges(Vec<LibraryPathChange*>& changes) {
    for (LibraryPathChange* change : changes) {
        DeleteLibraryPathChange(change);
    }
    changes.Reset();
}

static bool CreateSchema(LibraryStore* store) {
    const char* sql = R"sql(
BEGIN IMMEDIATE;
CREATE TABLE IF NOT EXISTS books (
  id INTEGER PRIMARY KEY,
  path TEXT NOT NULL,
  path_key TEXT NOT NULL UNIQUE,
  title TEXT NOT NULL,
  open_count INTEGER NOT NULL DEFAULT 0 CHECK(open_count >= 0),
  reading_seconds INTEGER NOT NULL DEFAULT 0 CHECK(reading_seconds >= 0),
  last_read_ms INTEGER NOT NULL DEFAULT 0,
  created_ms INTEGER NOT NULL,
  updated_ms INTEGER NOT NULL,
  bg_color INTEGER NOT NULL DEFAULT 0,
  notebooklm TEXT NOT NULL DEFAULT '',
  kind INTEGER NOT NULL DEFAULT 0 CHECK(kind IN (0, 1)),
  url TEXT NOT NULL DEFAULT ''
);
CREATE TABLE IF NOT EXISTS collections (
  id INTEGER PRIMARY KEY,
  parent_id INTEGER REFERENCES collections(id) ON DELETE CASCADE,
  kind INTEGER NOT NULL CHECK(kind IN (1, 2)),
  name TEXT NOT NULL,
  created_ms INTEGER NOT NULL,
  bg_color INTEGER NOT NULL DEFAULT 0,
  sort_pos INTEGER NOT NULL DEFAULT 0,
  UNIQUE(parent_id, name COLLATE NOCASE)
);
CREATE TABLE IF NOT EXISTS book_collections (
  book_id INTEGER NOT NULL REFERENCES books(id) ON DELETE CASCADE,
  collection_id INTEGER NOT NULL REFERENCES collections(id) ON DELETE CASCADE,
  added_ms INTEGER NOT NULL,
  sort_pos INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY(book_id, collection_id)
);
CREATE TABLE IF NOT EXISTS desk_books (
  book_id INTEGER PRIMARY KEY REFERENCES books(id) ON DELETE CASCADE,
  added_ms INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS manual_books (
  book_id INTEGER PRIMARY KEY REFERENCES books(id) ON DELETE CASCADE,
  added_ms INTEGER NOT NULL,
  sort_pos INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_collections_parent ON collections(parent_id);
CREATE INDEX IF NOT EXISTS idx_book_collections_collection ON book_collections(collection_id);
CREATE UNIQUE INDEX IF NOT EXISTS idx_collections_parent_name
  ON collections(COALESCE(parent_id, 0), name COLLATE NOCASE);
PRAGMA user_version = 8;
COMMIT;
)sql";
    return Exec(store, sql);
}

static bool TableHasColumn(LibraryStore* store, const char* table, const char* column) {
    TempStr sql = fmt("PRAGMA table_info(%s)", Str(table));
    sqlite3_stmt* stmt = Prepare(store, CStrTemp(sql));
    if (!stmt) {
        return false;
    }
    bool found = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* name = (const char*)sqlite3_column_text(stmt, 1);
        if (name && str::EqI(Str(name), Str(column))) {
            found = true;
            break;
        }
    }
    sqlite3_finalize(stmt);
    return found;
}

static bool MigrateToV4(LibraryStore* store) {
    bool needBackfill = false;
    if (!TableHasColumn(store, "book_collections", "sort_pos")) {
        if (!Exec(store, "ALTER TABLE book_collections ADD COLUMN sort_pos INTEGER NOT NULL DEFAULT 0")) {
            return false;
        }
        needBackfill = true;
    }
    if (!TableHasColumn(store, "manual_books", "sort_pos")) {
        if (!Exec(store, "ALTER TABLE manual_books ADD COLUMN sort_pos INTEGER NOT NULL DEFAULT 0")) {
            return false;
        }
        needBackfill = true;
    }
    if (needBackfill) {
        // Preserve alphabetical order as the initial manual order so existing
        // libraries do not reshuffle until the user starts dragging.
        if (!Exec(store, R"sql(
UPDATE book_collections
SET sort_pos = (
  SELECT COUNT(*)
  FROM book_collections other
  JOIN books b_other ON b_other.id = other.book_id
  JOIN books b_self ON b_self.id = book_collections.book_id
  WHERE other.collection_id = book_collections.collection_id
    AND (
      b_other.title COLLATE NOCASE < b_self.title COLLATE NOCASE
      OR (b_other.title COLLATE NOCASE = b_self.title COLLATE NOCASE AND other.book_id < book_collections.book_id)
    )
);
UPDATE manual_books
SET sort_pos = (
  SELECT COUNT(*)
  FROM manual_books other
  JOIN books b_other ON b_other.id = other.book_id
  JOIN books b_self ON b_self.id = manual_books.book_id
  WHERE
    b_other.title COLLATE NOCASE < b_self.title COLLATE NOCASE
    OR (b_other.title COLLATE NOCASE = b_self.title COLLATE NOCASE AND other.book_id < manual_books.book_id)
);
)sql")) {
            return false;
        }
    }
    return Exec(store, "PRAGMA user_version = 4");
}

static bool MigrateToV5(LibraryStore* store) {
    if (!TableHasColumn(store, "books", "bg_color")) {
        if (!Exec(store, "ALTER TABLE books ADD COLUMN bg_color INTEGER NOT NULL DEFAULT 0")) {
            return false;
        }
    }
    if (!TableHasColumn(store, "collections", "bg_color")) {
        if (!Exec(store, "ALTER TABLE collections ADD COLUMN bg_color INTEGER NOT NULL DEFAULT 0")) {
            return false;
        }
    }
    return Exec(store, "PRAGMA user_version = 5");
}

static bool MigrateToV6(LibraryStore* store) {
    if (!TableHasColumn(store, "books", "notebooklm")) {
        if (!Exec(store, "ALTER TABLE books ADD COLUMN notebooklm TEXT NOT NULL DEFAULT ''")) {
            return false;
        }
    }
    return Exec(store, "PRAGMA user_version = 6");
}

static bool MigrateToV7(LibraryStore* store) {
    if (!TableHasColumn(store, "books", "kind")) {
        if (!Exec(store, "ALTER TABLE books ADD COLUMN kind INTEGER NOT NULL DEFAULT 0")) {
            return false;
        }
    }
    if (!TableHasColumn(store, "books", "url")) {
        if (!Exec(store, "ALTER TABLE books ADD COLUMN url TEXT NOT NULL DEFAULT ''")) {
            return false;
        }
    }
    return Exec(store, "PRAGMA user_version = 7");
}

static bool MigrateToV8(LibraryStore* store) {
    bool needBackfill = false;
    if (!TableHasColumn(store, "collections", "sort_pos")) {
        if (!Exec(store, "ALTER TABLE collections ADD COLUMN sort_pos INTEGER NOT NULL DEFAULT 0")) {
            return false;
        }
        needBackfill = true;
    }
    if (needBackfill) {
        // Keep current alphabetical sibling order as the initial manual order.
        if (!Exec(store, R"sql(
UPDATE collections
SET sort_pos = (
  SELECT COUNT(*)
  FROM collections other
  WHERE COALESCE(other.parent_id, 0) = COALESCE(collections.parent_id, 0)
    AND (
      other.name COLLATE NOCASE < collections.name COLLATE NOCASE
      OR (other.name COLLATE NOCASE = collections.name COLLATE NOCASE AND other.id < collections.id)
    )
);
)sql")) {
            return false;
        }
    }
    return Exec(store, "PRAGMA user_version = 8");
}

static int SchemaVersion(LibraryStore* store) {
    sqlite3_stmt* stmt = Prepare(store, "PRAGMA user_version");
    if (!stmt) {
        return -1;
    }
    int version = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : -1;
    sqlite3_finalize(stmt);
    return version;
}

LibraryStore* LibraryStoreOpen(Str dbPath) {
    auto* store = new LibraryStore();
    TempStr normalized = NormalizePathTemp(dbPath);
    if (!dir::CreateForFile(normalized)) {
        str::ReplaceWithCopy(&store->error, StrL("cannot create the database directory"));
        return store;
    }
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(CStrTemp(normalized), &store->db, flags, nullptr) != SQLITE_OK) {
        SetError(store, StrL("open"));
        if (store->db) {
            sqlite3_close(store->db);
            store->db = nullptr;
        }
        return store;
    }
    sqlite3_busy_timeout(store->db, 2000);
    if (!Exec(store, "PRAGMA foreign_keys=ON; PRAGMA journal_mode=DELETE; PRAGMA synchronous=NORMAL;")) {
        sqlite3_close(store->db);
        store->db = nullptr;
        return store;
    }
    int version = SchemaVersion(store);
    if (version < 0 || version > 8) {
        str::ReplaceWithCopy(&store->error, fmt("unsupported library database version: %d", version));
        sqlite3_close(store->db);
        store->db = nullptr;
        return store;
    }
    if (version < 8) {
        logf("LibraryStore migrating schema: v%d -> v8\n", version);
    }
    if (!CreateSchema(store)) {
        sqlite3_close(store->db);
        store->db = nullptr;
        return store;
    }
    // CreateSchema is idempotent for brand-new DBs. Existing tables need ALTER
    // because CREATE TABLE IF NOT EXISTS will not add new columns.
    if (!MigrateToV4(store)) {
        sqlite3_close(store->db);
        store->db = nullptr;
        return store;
    }
    if (!MigrateToV5(store)) {
        sqlite3_close(store->db);
        store->db = nullptr;
        return store;
    }
    if (!MigrateToV6(store)) {
        sqlite3_close(store->db);
        store->db = nullptr;
        return store;
    }
    if (!MigrateToV7(store)) {
        sqlite3_close(store->db);
        store->db = nullptr;
        return store;
    }
    if (!MigrateToV8(store)) {
        sqlite3_close(store->db);
        store->db = nullptr;
        return store;
    }
    logf("LibraryStore opened: '%s' (sqlite %s)\n", normalized, Str(sqlite3_libversion()));
    return store;
}

void LibraryStoreClose(LibraryStore* store) {
    if (!store) {
        return;
    }
    if (store->db) {
        sqlite3_close(store->db);
    }
    str::Free(store->error);
    delete store;
}

bool LibraryStoreIsOpen(LibraryStore* store) {
    return store && store->db;
}

Str LibraryStoreError(LibraryStore* store) {
    return store ? store->error : Str();
}

LibraryBook* LibraryStoreRecordOpen(LibraryStore* store, Str path, Str title, i64 nowMs, bool* placedAtRoot) {
    if (placedAtRoot) {
        *placedAtRoot = false;
    }
    if (!LibraryStoreIsOpen(store) || !path) {
        return nullptr;
    }
    TempStr normalized = NormalizePathTemp(path);
    Str key = PathKey(normalized);
    TempStr fallbackTitle = path::GetBaseNameTemp(normalized);
    if (!title) {
        title = fallbackTitle;
    }
    if (!Exec(store, "BEGIN IMMEDIATE")) {
        str::Free(key);
        return nullptr;
    }
    sqlite3_stmt* rootCheck = Prepare(store, R"sql(
SELECT NOT EXISTS(SELECT 1 FROM manual_books m WHERE m.book_id=b.id)
   AND NOT EXISTS(SELECT 1 FROM book_collections bc WHERE bc.book_id=b.id)
FROM books b WHERE b.path_key=?1;
)sql");
    if (!rootCheck) {
        Exec(store, "ROLLBACK");
        str::Free(key);
        return nullptr;
    }
    BindText(rootCheck, 1, key);
    int rootStep = sqlite3_step(rootCheck);
    if (rootStep != SQLITE_ROW && rootStep != SQLITE_DONE) {
        SetError(store, StrL("check opened book placement"));
        sqlite3_finalize(rootCheck);
        Exec(store, "ROLLBACK");
        str::Free(key);
        return nullptr;
    }
    // A new book belongs at the Library root. An existing unplaced book is
    // repaired there, while an organized book keeps its current location.
    bool addToRoot = rootStep == SQLITE_DONE || sqlite3_column_int(rootCheck, 0) != 0;
    sqlite3_finalize(rootCheck);
    const char* sql = R"sql(
INSERT INTO books(path, path_key, title, open_count, reading_seconds, last_read_ms, created_ms, updated_ms)
VALUES(?1, ?2, ?3, 1, 0, ?4, ?4, ?4)
ON CONFLICT(path_key) DO UPDATE SET
  path=excluded.path, title=excluded.title, open_count=books.open_count+1,
  last_read_ms=excluded.last_read_ms, updated_ms=excluded.updated_ms
RETURNING id, path, title, open_count, reading_seconds, last_read_ms, 0, bg_color, notebooklm, kind, url;
)sql";
    sqlite3_stmt* stmt = Prepare(store, sql);
    if (!stmt) {
        str::Free(key);
        return nullptr;
    }
    BindText(stmt, 1, normalized);
    BindText(stmt, 2, key);
    BindText(stmt, 3, title);
    sqlite3_bind_int64(stmt, 4, nowMs);
    LibraryBook* book = nullptr;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        book = ReadBook(stmt);
    } else {
        SetError(store, StrL("record open"));
    }
    sqlite3_finalize(stmt);
    if (book && addToRoot) {
        i64 sortPos = NextSortPos(store, 0);
        stmt = Prepare(store, "INSERT OR IGNORE INTO manual_books(book_id,added_ms,sort_pos) VALUES(?1,?2,?3)");
        if (stmt) {
            sqlite3_bind_int64(stmt, 1, book->id);
            sqlite3_bind_int64(stmt, 2, nowMs);
            sqlite3_bind_int64(stmt, 3, sortPos);
            if (sqlite3_step(stmt) != SQLITE_DONE) {
                SetError(store, StrL("place opened book at library root"));
                DeleteLibraryBook(book);
                book = nullptr;
            } else if (placedAtRoot) {
                *placedAtRoot = sqlite3_changes(store->db) > 0;
            }
            sqlite3_finalize(stmt);
        } else {
            DeleteLibraryBook(book);
            book = nullptr;
        }
    }
    if (!book || !Exec(store, "COMMIT")) {
        Exec(store, "ROLLBACK");
        DeleteLibraryBook(book);
        book = nullptr;
    }
    str::Free(key);
    return book;
}

LibraryBook* LibraryStoreAddBook(LibraryStore* store, Str path, Str title, i64 nowMs) {
    if (!LibraryStoreIsOpen(store) || !path) {
        return nullptr;
    }
    TempStr normalized = NormalizePathTemp(path);
    Str key = PathKey(normalized);
    TempStr fallbackTitle = path::GetBaseNameTemp(normalized);
    if (!title) {
        title = fallbackTitle;
    }
    if (!Exec(store, "BEGIN IMMEDIATE")) {
        str::Free(key);
        return nullptr;
    }
    const char* sql = R"sql(
INSERT INTO books(path, path_key, title, open_count, reading_seconds, last_read_ms, created_ms, updated_ms)
VALUES(?1, ?2, ?3, 0, 0, 0, ?4, ?4)
ON CONFLICT(path_key) DO UPDATE SET path=excluded.path,title=excluded.title,updated_ms=excluded.updated_ms
RETURNING id, path, title, open_count, reading_seconds, last_read_ms, 0, bg_color, notebooklm, kind, url;
)sql";
    sqlite3_stmt* stmt = Prepare(store, sql);
    if (!stmt) {
        Exec(store, "ROLLBACK");
        str::Free(key);
        return nullptr;
    }
    BindText(stmt, 1, normalized);
    BindText(stmt, 2, key);
    BindText(stmt, 3, title);
    sqlite3_bind_int64(stmt, 4, nowMs);
    LibraryBook* book = nullptr;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        book = ReadBook(stmt);
    } else {
        SetError(store, StrL("add book"));
    }
    sqlite3_finalize(stmt);
    if (book) {
        i64 sortPos = NextSortPos(store, 0);
        stmt = Prepare(store, "INSERT OR IGNORE INTO manual_books(book_id,added_ms,sort_pos) VALUES(?1,?2,?3)");
        if (stmt) {
            sqlite3_bind_int64(stmt, 1, book->id);
            sqlite3_bind_int64(stmt, 2, nowMs);
            sqlite3_bind_int64(stmt, 3, sortPos);
            if (sqlite3_step(stmt) != SQLITE_DONE) {
                SetError(store, StrL("mark manual book"));
                DeleteLibraryBook(book);
                book = nullptr;
            }
            sqlite3_finalize(stmt);
        } else {
            DeleteLibraryBook(book);
            book = nullptr;
        }
    }
    if (!book || !Exec(store, "COMMIT")) {
        Exec(store, "ROLLBACK");
        DeleteLibraryBook(book);
        book = nullptr;
    }
    str::Free(key);
    return book;
}

LibraryBook* LibraryStoreImportBook(LibraryStore* store, Str path, Str title, i64 openCount, i64 nowMs) {
    if (!LibraryStoreIsOpen(store) || !path) {
        return nullptr;
    }
    TempStr normalized = NormalizePathTemp(path);
    Str key = PathKey(normalized);
    TempStr fallbackTitle = path::GetBaseNameTemp(normalized);
    if (!title) {
        title = fallbackTitle;
    }
    openCount = std::max<i64>(openCount, 0);
    const char* sql = R"sql(
INSERT INTO books(path, path_key, title, open_count, reading_seconds, last_read_ms, created_ms, updated_ms)
VALUES(?1, ?2, ?3, ?4, 0, 0, ?5, ?5)
ON CONFLICT(path_key) DO UPDATE SET
  path=excluded.path, open_count=MAX(books.open_count, excluded.open_count), updated_ms=excluded.updated_ms
RETURNING id, path, title, open_count, reading_seconds, last_read_ms, 0, bg_color, notebooklm, kind, url;
)sql";
    sqlite3_stmt* stmt = Prepare(store, sql);
    if (!stmt) {
        str::Free(key);
        return nullptr;
    }
    BindText(stmt, 1, normalized);
    BindText(stmt, 2, key);
    BindText(stmt, 3, title);
    sqlite3_bind_int64(stmt, 4, openCount);
    sqlite3_bind_int64(stmt, 5, nowMs);
    LibraryBook* book = nullptr;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        book = ReadBook(stmt);
    } else {
        SetError(store, StrL("import book"));
    }
    sqlite3_finalize(stmt);
    str::Free(key);
    return book;
}

LibraryBook* LibraryStoreAddWebBook(LibraryStore* store, Str url, Str title, i64 nowMs) {
    if (!LibraryStoreIsOpen(store) || !url || url.len == 0) {
        return nullptr;
    }
    TempStr trimmedUrl = str::DupTemp(url);
    if (!title || title.len == 0) {
        // Full URL until the browser document.title arrives (library always follows tab title).
        title = trimmedUrl;
    }
    if (!Exec(store, "BEGIN IMMEDIATE")) {
        return nullptr;
    }
    // Temporary unique path until we know the row id for web://{id}.
    TempStr pendingPath = fmt("web://pending-%lld", nowMs);
    Str pendingKey = PathKey(pendingPath);
    const char* sql = R"sql(
INSERT INTO books(path, path_key, title, open_count, reading_seconds, last_read_ms, created_ms, updated_ms, kind, url)
VALUES(?1, ?2, ?3, 0, 0, 0, ?4, ?4, 1, ?5)
RETURNING id, path, title, open_count, reading_seconds, last_read_ms, 0, bg_color, notebooklm, kind, url;
)sql";
    sqlite3_stmt* stmt = Prepare(store, sql);
    if (!stmt) {
        Exec(store, "ROLLBACK");
        str::Free(pendingKey);
        return nullptr;
    }
    BindText(stmt, 1, pendingPath);
    BindText(stmt, 2, pendingKey);
    BindText(stmt, 3, title);
    sqlite3_bind_int64(stmt, 4, nowMs);
    BindText(stmt, 5, trimmedUrl);
    LibraryBook* book = nullptr;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        book = ReadBook(stmt);
    } else {
        SetError(store, StrL("add web book"));
    }
    sqlite3_finalize(stmt);
    str::Free(pendingKey);
    if (book) {
        TempStr finalPath = fmt("web://%lld", book->id);
        Str finalKey = PathKey(finalPath);
        stmt = Prepare(store, "UPDATE books SET path=?1,path_key=?2,updated_ms=?3 WHERE id=?4");
        bool pathOk = stmt != nullptr;
        if (pathOk) {
            BindText(stmt, 1, finalPath);
            BindText(stmt, 2, finalKey);
            sqlite3_bind_int64(stmt, 3, nowMs);
            sqlite3_bind_int64(stmt, 4, book->id);
            pathOk = sqlite3_step(stmt) == SQLITE_DONE;
            sqlite3_finalize(stmt);
        }
        str::Free(finalKey);
        if (!pathOk) {
            SetError(store, StrL("finalize web book path"));
            DeleteLibraryBook(book);
            book = nullptr;
        } else {
            str::Free(book->path);
            book->path = str::Dup(finalPath);
            i64 sortPos = NextSortPos(store, 0);
            stmt = Prepare(store, "INSERT OR IGNORE INTO manual_books(book_id,added_ms,sort_pos) VALUES(?1,?2,?3)");
            if (stmt) {
                sqlite3_bind_int64(stmt, 1, book->id);
                sqlite3_bind_int64(stmt, 2, nowMs);
                sqlite3_bind_int64(stmt, 3, sortPos);
                if (sqlite3_step(stmt) != SQLITE_DONE) {
                    SetError(store, StrL("mark web book at root"));
                    DeleteLibraryBook(book);
                    book = nullptr;
                }
                sqlite3_finalize(stmt);
            } else {
                DeleteLibraryBook(book);
                book = nullptr;
            }
        }
    }
    if (!book || !Exec(store, "COMMIT")) {
        Exec(store, "ROLLBACK");
        DeleteLibraryBook(book);
        book = nullptr;
    }
    return book;
}

bool LibraryStoreAddReadingTime(LibraryStore* store, Str path, i64 seconds, i64 nowMs) {
    if (!LibraryStoreIsOpen(store) || seconds <= 0) {
        return false;
    }
    Str key = PathKey(path);
    // Skip web books: reading tracker is PDF-only.
    sqlite3_stmt* stmt = Prepare(
        store,
        "UPDATE books SET reading_seconds=reading_seconds+?1,last_read_ms=?2,updated_ms=?2 "
        "WHERE path_key=?3 AND kind=0");
    if (!stmt) {
        str::Free(key);
        return false;
    }
    sqlite3_bind_int64(stmt, 1, seconds);
    sqlite3_bind_int64(stmt, 2, nowMs);
    BindText(stmt, 3, key);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(store->db) == 1;
    if (!ok) {
        SetError(store, StrL("add reading time"));
    }
    sqlite3_finalize(stmt);
    str::Free(key);
    return ok;
}

static const char* SortSql(LibrarySort sort, LibraryBookScope scope) {
    switch (sort) {
        case LibrarySort::ReadingTime:
            return "b.reading_seconds DESC, b.title COLLATE NOCASE, b.path COLLATE NOCASE";
        case LibrarySort::Recent:
            return "b.last_read_ms DESC, b.title COLLATE NOCASE, b.path COLLATE NOCASE";
        case LibrarySort::Title:
            return "b.title COLLATE NOCASE, b.path COLLATE NOCASE";
        case LibrarySort::Manual:
            if (scope == LibraryBookScope::Collection) {
                return "bc.sort_pos ASC, b.title COLLATE NOCASE, b.path COLLATE NOCASE";
            }
            if (scope == LibraryBookScope::ManualRoot) {
                return "m.sort_pos ASC, b.title COLLATE NOCASE, b.path COLLATE NOCASE";
            }
            return "b.title COLLATE NOCASE, b.path COLLATE NOCASE";
        default:
            return "b.open_count DESC, b.title COLLATE NOCASE, b.path COLLATE NOCASE";
    }
}

static i64 NextSortPos(LibraryStore* store, i64 collectionId) {
    sqlite3_stmt* stmt =
        collectionId == 0 ? Prepare(store, "SELECT COALESCE(MAX(sort_pos), -1) + 1 FROM manual_books")
                          : Prepare(store, "SELECT COALESCE(MAX(sort_pos), -1) + 1 FROM book_collections WHERE "
                                           "collection_id=?1");
    if (!stmt) {
        return 0;
    }
    if (collectionId > 0) {
        sqlite3_bind_int64(stmt, 1, collectionId);
    }
    i64 pos = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        pos = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return pos;
}

static Str EscapeLikePattern(Str filter) {
    str::Builder escaped;
    for (int i = 0; i < len(filter); i++) {
        char c = filter.s[i];
        if (c == '\\' || c == '%' || c == '_') {
            escaped.AppendChar('\\');
        }
        escaped.AppendChar(c);
    }
    return escaped.TakeStr();
}

Vec<LibraryBook*> LibraryStoreGetBooks(LibraryStore* store, LibraryBookScope scope, i64 collectionId, LibrarySort sort,
                                       Str filter) {
    Vec<LibraryBook*> books;
    if (!LibraryStoreIsOpen(store)) {
        return books;
    }
    str::Builder sql;
    bool withSortPos = sort == LibrarySort::Manual &&
                       (scope == LibraryBookScope::Collection || scope == LibraryBookScope::ManualRoot);
    sql.Append(withSortPos ? "SELECT b.id,b.path,b.title,b.open_count,b.reading_seconds,b.last_read_ms,"
                             : "SELECT b.id,b.path,b.title,b.open_count,b.reading_seconds,b.last_read_ms,0,");
    if (withSortPos && scope == LibraryBookScope::Collection) {
        sql.Append("bc.sort_pos,b.bg_color,b.notebooklm,b.kind,b.url FROM books b ");
    } else if (withSortPos && scope == LibraryBookScope::ManualRoot) {
        sql.Append("m.sort_pos,b.bg_color,b.notebooklm,b.kind,b.url FROM books b ");
    } else {
        sql.Append("b.bg_color,b.notebooklm,b.kind,b.url FROM books b ");
    }
    if (scope == LibraryBookScope::Desk) {
        sql.Append("JOIN desk_books d ON d.book_id=b.id ");
    } else if (scope == LibraryBookScope::Collection) {
        sql.Append("JOIN book_collections bc ON bc.book_id=b.id ");
    } else if (scope == LibraryBookScope::ManualRoot) {
        sql.Append("JOIN manual_books m ON m.book_id=b.id ");
    }
    sql.Append("WHERE 1=1 ");
    if (scope == LibraryBookScope::Unclassified) {
        sql.Append("AND NOT EXISTS(SELECT 1 FROM book_collections x WHERE x.book_id=b.id) ");
    } else if (scope == LibraryBookScope::Collection) {
        sql.Append("AND bc.collection_id=?1 ");
    }
    if (filter) {
        sql.Append(scope == LibraryBookScope::Collection
                       ? "AND (b.title LIKE ?2 ESCAPE '\\' OR b.path LIKE ?2 ESCAPE '\\' OR b.url LIKE ?2 ESCAPE '\\') "
                       : "AND (b.title LIKE ?1 ESCAPE '\\' OR b.path LIKE ?1 ESCAPE '\\' OR b.url LIKE ?1 ESCAPE '\\') ");
    }
    sql.Append(fmt("ORDER BY %s", Str(SortSql(sort, scope))));
    sqlite3_stmt* stmt = Prepare(store, CStrTemp(ToStr(sql)));
    if (!stmt) {
        return books;
    }
    int bind = 1;
    if (scope == LibraryBookScope::Collection) {
        sqlite3_bind_int64(stmt, bind++, collectionId);
    }
    if (filter) {
        Str escaped = EscapeLikePattern(filter);
        TempStr like = fmt("%%%s%%", escaped);
        BindText(stmt, bind, like);
        str::Free(escaped);
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        books.Append(ReadBook(stmt));
    }
    sqlite3_finalize(stmt);
    if (sort == LibrarySort::Title) {
        VecSort(books, [](LibraryBook* const* a, LibraryBook* const* b) -> int {
            int n = str::CmpNatural((*a)->title, (*b)->title);
            if (n != 0) return n;
            n = str::CmpNatural((*a)->path, (*b)->path);
            if (n != 0) return n;
            return ((*a)->id > (*b)->id) - ((*a)->id < (*b)->id);
        });
    } else if (sort == LibrarySort::Manual) {
        VecSort(books, [](LibraryBook* const* a, LibraryBook* const* b) -> int { return CmpManualOrder(*a, *b); });
    }
    return books;
}

static int CmpManualOrder(LibraryBook* a, LibraryBook* b) {
    if (a->sortPos != b->sortPos) {
        return (a->sortPos > b->sortPos) - (a->sortPos < b->sortPos);
    }
    int n = str::CmpNatural(a->title, b->title);
    if (n != 0) return n;
    n = str::CmpNatural(a->path, b->path);
    if (n != 0) return n;
    return (a->id > b->id) - (a->id < b->id);
}

Vec<LibraryBook*> LibraryStoreGetPlacedBooks(LibraryStore* store, Str filter) {
    Vec<LibraryBook*> books;
    if (!LibraryStoreIsOpen(store)) {
        return books;
    }
    str::Builder sql;
    sql.Append(
        "SELECT b.id,b.path,b.title,b.open_count,b.reading_seconds,b.last_read_ms,p.sort_pos,b.bg_color,NULL,b.kind,"
        "b.url,p.cid FROM (SELECT book_id,collection_id AS cid,sort_pos FROM book_collections "
        "UNION ALL SELECT book_id,0 AS cid,sort_pos FROM manual_books) p JOIN books b ON b.id=p.book_id ");
    if (filter) {
        sql.Append("WHERE b.title LIKE ?1 ESCAPE '\\' OR b.path LIKE ?1 ESCAPE '\\' OR b.url LIKE ?1 ESCAPE '\\' ");
    }
    sqlite3_stmt* stmt = Prepare(store, CStrTemp(ToStr(sql)));
    if (!stmt) {
        return books;
    }
    if (filter) {
        Str escaped = EscapeLikePattern(filter);
        TempStr like = fmt("%%%s%%", escaped);
        BindText(stmt, 1, like);
        str::Free(escaped);
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        LibraryBook* book = ReadBook(stmt);
        book->placementId = sqlite3_column_int64(stmt, 11);
        books.Append(book);
    }
    sqlite3_finalize(stmt);
    VecSort(books, [](LibraryBook* const* a, LibraryBook* const* b) -> int {
        if ((*a)->placementId != (*b)->placementId) {
            return ((*a)->placementId > (*b)->placementId) - ((*a)->placementId < (*b)->placementId);
        }
        return CmpManualOrder(*a, *b);
    });
    return books;
}

Vec<LibraryCollection*> LibraryStoreGetCollections(LibraryStore* store) {
    Vec<LibraryCollection*> collections;
    if (!LibraryStoreIsOpen(store)) {
        return collections;
    }
    sqlite3_stmt* stmt = Prepare(
        store,
        "SELECT id,COALESCE(parent_id,0),kind,name,bg_color,sort_pos FROM collections "
        "ORDER BY COALESCE(parent_id,0), sort_pos ASC, id ASC");
    if (!stmt) {
        return collections;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        auto* collection = new LibraryCollection();
        collection->id = sqlite3_column_int64(stmt, 0);
        collection->parentId = sqlite3_column_int64(stmt, 1);
        collection->isShelf = sqlite3_column_int(stmt, 2) == 1;
        collection->name = ColumnTextDup(stmt, 3);
        collection->bgColor = (u32)sqlite3_column_int64(stmt, 4);
        collection->sortPos = sqlite3_column_int64(stmt, 5);
        collections.Append(collection);
    }
    sqlite3_finalize(stmt);
    return collections;
}

bool LibraryStoreSeedForTesting(LibraryStore* store, int nFolders, int booksPerFolder, int rootBooks) {
    if (!LibraryStoreIsOpen(store) || nFolders < 0 || booksPerFolder < 0 || rootBooks < 0) {
        return false;
    }
    if (!Exec(store, "BEGIN IMMEDIATE")) {
        return false;
    }
    sqlite3_stmt* addCol = Prepare(
        store, "INSERT INTO collections(parent_id,kind,name,created_ms,sort_pos) VALUES(NULL,2,?1,0,?2) RETURNING id");
    sqlite3_stmt* addBook = Prepare(store,
                                    "INSERT INTO books(path,path_key,title,created_ms,updated_ms) "
                                    "VALUES(?1,?2,?3,0,0) RETURNING id");
    sqlite3_stmt* inCol =
        Prepare(store, "INSERT INTO book_collections(book_id,collection_id,added_ms,sort_pos) VALUES(?1,?2,0,?3)");
    sqlite3_stmt* atRoot = Prepare(store, "INSERT INTO manual_books(book_id,added_ms,sort_pos) VALUES(?1,0,?2)");
    bool ok = addCol && addBook && inCol && atRoot;
    int serial = 0;
    auto insertBook = [&](int idx) -> i64 {
        TempStr path = fmt("C:\\seed\\f%03d\\book-%06d-some-long-title-for-width.pdf", idx / 1000, serial);
        Str key = str::ToLower(path);
        TempStr title = path::GetBaseNameTemp(path);
        serial++;
        sqlite3_reset(addBook);
        BindText(addBook, 1, path);
        BindText(addBook, 2, key);
        BindText(addBook, 3, title);
        i64 id = 0;
        if (sqlite3_step(addBook) == SQLITE_ROW) {
            id = sqlite3_column_int64(addBook, 0);
        }
        str::Free(key);
        return id;
    };
    for (int f = 0; ok && f < nFolders; f++) {
        sqlite3_reset(addCol);
        BindText(addCol, 1, fmt("seed-folder-%04d", f));
        sqlite3_bind_int64(addCol, 2, f);
        if (sqlite3_step(addCol) != SQLITE_ROW) {
            ok = false;
            break;
        }
        i64 colId = sqlite3_column_int64(addCol, 0);
        for (int i = 0; ok && i < booksPerFolder; i++) {
            i64 bookId = insertBook(f);
            sqlite3_reset(inCol);
            sqlite3_bind_int64(inCol, 1, bookId);
            sqlite3_bind_int64(inCol, 2, colId);
            sqlite3_bind_int64(inCol, 3, i);
            ok = bookId > 0 && sqlite3_step(inCol) == SQLITE_DONE;
        }
    }
    for (int i = 0; ok && i < rootBooks; i++) {
        i64 bookId = insertBook(nFolders);
        sqlite3_reset(atRoot);
        sqlite3_bind_int64(atRoot, 1, bookId);
        sqlite3_bind_int64(atRoot, 2, i);
        ok = bookId > 0 && sqlite3_step(atRoot) == SQLITE_DONE;
    }
    if (!ok) {
        SetError(store, StrL("seed"));
    }
    sqlite3_finalize(addCol);
    sqlite3_finalize(addBook);
    sqlite3_finalize(inCol);
    sqlite3_finalize(atRoot);
    if (!ok || !Exec(store, "COMMIT")) {
        Exec(store, "ROLLBACK");
        return false;
    }
    return true;
}

static int CollectionKind(LibraryStore* store, i64 id) {
    if (!store || id <= 0) {
        return 0;
    }
    sqlite3_stmt* stmt = Prepare(store, "SELECT kind FROM collections WHERE id=?1");
    if (!stmt) {
        return 0;
    }
    sqlite3_bind_int64(stmt, 1, id);
    int kind = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        kind = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return kind;
}

static i64 NextCollectionSortPos(LibraryStore* store, i64 parentId) {
    sqlite3_stmt* stmt =
        Prepare(store, "SELECT COALESCE(MAX(sort_pos), -1) + 1 FROM collections WHERE COALESCE(parent_id, 0)=?1");
    if (!stmt) {
        return 0;
    }
    sqlite3_bind_int64(stmt, 1, parentId);
    i64 pos = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        pos = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return pos;
}

LibraryCollection* LibraryStoreCreateCollection(LibraryStore* store, i64 parentId, bool isShelf, Str name) {
    if (!LibraryStoreIsOpen(store) || !name) {
        return nullptr;
    }
    // Shelves (kind 1) only at root. Folders/tags (kind 2) may nest under a shelf,
    // another folder, or sit at the library root — unlimited depth ("a/b/c").
    if (isShelf) {
        if (parentId != 0) {
            return nullptr;
        }
    } else if (parentId != 0) {
        int parentKind = CollectionKind(store, parentId);
        if (parentKind != 1 && parentKind != 2) {
            return nullptr;
        }
    }
    i64 sortPos = NextCollectionSortPos(store, parentId);
    sqlite3_stmt* stmt = Prepare(
        store, "INSERT INTO collections(parent_id,kind,name,created_ms,sort_pos) VALUES(?1,?2,?3,?4,?5) RETURNING id");
    if (!stmt) {
        return nullptr;
    }
    if (parentId)
        sqlite3_bind_int64(stmt, 1, parentId);
    else
        sqlite3_bind_null(stmt, 1);
    sqlite3_bind_int(stmt, 2, isShelf ? 1 : 2);
    BindText(stmt, 3, name);
    sqlite3_bind_int64(stmt, 4, UnixTimeMsNow());
    sqlite3_bind_int64(stmt, 5, sortPos);
    LibraryCollection* result = nullptr;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        result = new LibraryCollection();
        result->id = sqlite3_column_int64(stmt, 0);
        result->parentId = parentId;
        result->isShelf = isShelf;
        result->name = str::Dup(name);
        result->sortPos = sortPos;
    } else {
        SetError(store, StrL("create collection"));
    }
    sqlite3_finalize(stmt);
    return result;
}

bool LibraryStoreDeleteCollection(LibraryStore* store, i64 collectionId) {
    if (!LibraryStoreIsOpen(store) || collectionId <= 0) return false;
    if (!Exec(store, "BEGIN IMMEDIATE")) return false;
    sqlite3_stmt* stmt = Prepare(store, R"sql(
WITH RECURSIVE subtree(id) AS (
  SELECT id FROM collections WHERE id=?1
  UNION ALL
  SELECT c.id FROM collections c JOIN subtree s ON c.parent_id=s.id
)
INSERT OR IGNORE INTO manual_books(book_id,added_ms,sort_pos)
SELECT DISTINCT bc.book_id,?2,
  (SELECT COALESCE(MAX(m.sort_pos), -1) + 1 FROM manual_books m)
  + ROW_NUMBER() OVER (ORDER BY bc.book_id)
  - 1
FROM book_collections bc JOIN subtree s ON s.id=bc.collection_id
WHERE NOT EXISTS(
  SELECT 1 FROM book_collections other
  WHERE other.book_id=bc.book_id AND other.collection_id NOT IN (SELECT id FROM subtree)
)
AND NOT EXISTS(SELECT 1 FROM manual_books m WHERE m.book_id=bc.book_id);
)sql");
    if (!stmt) {
        Exec(store, "ROLLBACK");
        return false;
    }
    sqlite3_bind_int64(stmt, 1, collectionId);
    sqlite3_bind_int64(stmt, 2, UnixTimeMsNow());
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (ok) {
        stmt = Prepare(store, "DELETE FROM collections WHERE id=?1");
        if (stmt) {
            sqlite3_bind_int64(stmt, 1, collectionId);
            ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(store->db) == 1;
            sqlite3_finalize(stmt);
        } else {
            ok = false;
        }
    }
    if (!ok || !Exec(store, "COMMIT")) {
        Exec(store, "ROLLBACK");
        return false;
    }
    return ok;
}

bool LibraryStoreRenameCollection(LibraryStore* store, i64 collectionId, Str name) {
    if (!LibraryStoreIsOpen(store) || collectionId <= 0 || !name) return false;
    sqlite3_stmt* stmt = Prepare(store, "UPDATE collections SET name=?1 WHERE id=?2");
    if (!stmt) return false;
    BindText(stmt, 1, name);
    sqlite3_bind_int64(stmt, 2, collectionId);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(store->db) == 1;
    if (!ok) SetError(store, StrL("rename collection"));
    sqlite3_finalize(stmt);
    return ok;
}

bool LibraryStoreMoveCollection(LibraryStore* store, i64 collectionId, i64 newParentId) {
    if (!LibraryStoreIsOpen(store) || collectionId <= 0 || collectionId == newParentId) {
        return false;
    }
    int sourceKind = CollectionKind(store, collectionId);
    if (sourceKind != 1 && sourceKind != 2) {
        return false;
    }
    // Shelves stay at root only. Folders may move under shelf/folder/root.
    if (sourceKind == 1) {
        if (newParentId != 0) {
            return false;
        }
    } else if (newParentId != 0) {
        int parentKind = CollectionKind(store, newParentId);
        if (parentKind != 1 && parentKind != 2) {
            return false;
        }
    }
    sqlite3_stmt* check = Prepare(store, R"sql(
WITH RECURSIVE descendants(id) AS (
  SELECT id FROM collections WHERE id=?1
  UNION ALL
  SELECT c.id FROM collections c JOIN descendants d ON c.parent_id=d.id
)
SELECT 1 FROM descendants WHERE id=?2;
)sql");
    if (!check) {
        return false;
    }
    sqlite3_bind_int64(check, 1, collectionId);
    sqlite3_bind_int64(check, 2, newParentId);
    bool cycle = sqlite3_step(check) == SQLITE_ROW;
    sqlite3_finalize(check);
    if (cycle) {
        str::ReplaceWithCopy(&store->error, StrL("moving the collection would create a cycle"));
        return false;
    }
    // Preserve kind: folders remain folders even at root (tags at top level).
    // Append at the end of the destination sibling list.
    i64 sortPos = NextCollectionSortPos(store, newParentId);
    sqlite3_stmt* stmt = Prepare(store, "UPDATE collections SET parent_id=?1, sort_pos=?2 WHERE id=?3");
    if (!stmt) {
        return false;
    }
    if (newParentId > 0) {
        sqlite3_bind_int64(stmt, 1, newParentId);
    } else {
        sqlite3_bind_null(stmt, 1);
    }
    sqlite3_bind_int64(stmt, 2, sortPos);
    sqlite3_bind_int64(stmt, 3, collectionId);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(store->db) == 1;
    if (!ok) {
        SetError(store, StrL("move collection"));
    }
    sqlite3_finalize(stmt);
    return ok;
}

static bool StepDone(sqlite3_stmt* stmt) {
    if (!stmt) {
        return false;
    }
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

static bool MoveOrMergeCollection(LibraryStore* store, i64 srcId, i64 dstId, int depth);

static bool MergeCollectionInto(LibraryStore* store, i64 srcId, i64 dstId, int depth) {
    if (depth > 64) {
        return false;
    }
    // Books keep their order, appended after the target's existing books.
    i64 base = NextSortPos(store, dstId);
    sqlite3_stmt* stmt = Prepare(store, R"sql(
INSERT OR IGNORE INTO book_collections(book_id,collection_id,added_ms,sort_pos)
SELECT book_id, ?2, added_ms, ?3 + ROW_NUMBER() OVER (ORDER BY sort_pos, book_id) - 1
FROM book_collections WHERE collection_id=?1;
)sql");
    if (stmt) {
        sqlite3_bind_int64(stmt, 1, srcId);
        sqlite3_bind_int64(stmt, 2, dstId);
        sqlite3_bind_int64(stmt, 3, base);
    }
    if (!StepDone(stmt)) {
        return false;
    }
    stmt = Prepare(store, "DELETE FROM book_collections WHERE collection_id=?1");
    if (stmt) {
        sqlite3_bind_int64(stmt, 1, srcId);
    }
    if (!StepDone(stmt)) {
        return false;
    }

    Vec<LibraryCollection*> all = LibraryStoreGetCollections(store);
    Vec<i64> children;
    for (LibraryCollection* c : all) {
        if (c->parentId == srcId) {
            children.Append(c->id);
        }
    }
    DeleteLibraryCollections(all);
    for (i64 childId : children) {
        if (!MoveOrMergeCollection(store, childId, dstId, depth + 1)) {
            return false;
        }
    }
    stmt = Prepare(store, "DELETE FROM collections WHERE id=?1");
    if (stmt) {
        sqlite3_bind_int64(stmt, 1, srcId);
    }
    return StepDone(stmt);
}

// Re-parent srcId under dstId (0 = library root); a same-named folder already there
// (names are unique per parent, case-insensitive) absorbs it instead.
static bool MoveOrMergeCollection(LibraryStore* store, i64 srcId, i64 dstId, int depth) {
    Vec<LibraryCollection*> all = LibraryStoreGetCollections(store);
    Str srcName = {};
    for (LibraryCollection* c : all) {
        if (c->id == srcId) {
            srcName = c->name;
        }
    }
    i64 sameId = 0;
    for (LibraryCollection* c : all) {
        if (c->parentId == dstId && c->id != srcId && str::EqI(c->name, srcName)) {
            sameId = c->id;
            break;
        }
    }
    DeleteLibraryCollections(all);
    if (sameId > 0) {
        return MergeCollectionInto(store, srcId, sameId, depth);
    }
    i64 sortPos = NextCollectionSortPos(store, dstId);
    sqlite3_stmt* stmt = Prepare(store, "UPDATE collections SET parent_id=?1, sort_pos=?2 WHERE id=?3");
    if (stmt) {
        if (dstId > 0) {
            sqlite3_bind_int64(stmt, 1, dstId);
        } else {
            sqlite3_bind_null(stmt, 1);
        }
        sqlite3_bind_int64(stmt, 2, sortPos);
        sqlite3_bind_int64(stmt, 3, srcId);
    }
    return StepDone(stmt);
}

bool LibraryStoreRetagCollection(LibraryStore* store, i64 srcId, i64 dstId) {
    if (!LibraryStoreIsOpen(store) || srcId <= 0 || dstId < 0) {
        return false;
    }
    if (srcId == dstId) {
        return true;
    }
    int srcKind = CollectionKind(store, srcId);
    int dstKind = dstId == 0 ? 2 : CollectionKind(store, dstId);
    if (srcKind != 2 || (dstKind != 1 && dstKind != 2)) {
        str::ReplaceWithCopy(&store->error, StrL("only folders can be retagged, into a folder or shelf"));
        return false;
    }
    Vec<LibraryCollection*> all = LibraryStoreGetCollections(store);
    bool alreadyThere = false;
    for (LibraryCollection* c : all) {
        if (c->id == srcId) {
            alreadyThere = c->parentId == dstId;
        }
    }
    DeleteLibraryCollections(all);
    if (alreadyThere) {
        return true;
    }
    sqlite3_stmt* check = Prepare(store, R"sql(
WITH RECURSIVE descendants(id) AS (
  SELECT id FROM collections WHERE id=?1
  UNION ALL
  SELECT c.id FROM collections c JOIN descendants d ON c.parent_id=d.id
)
SELECT 1 FROM descendants WHERE id=?2;
)sql");
    if (!check) {
        return false;
    }
    sqlite3_bind_int64(check, 1, srcId);
    sqlite3_bind_int64(check, 2, dstId);
    bool cycle = sqlite3_step(check) == SQLITE_ROW;
    sqlite3_finalize(check);
    if (cycle) {
        str::ReplaceWithCopy(&store->error, StrL("target tag is inside the folder being retagged"));
        return false;
    }
    if (!Exec(store, "BEGIN IMMEDIATE")) {
        return false;
    }
    bool ok = MoveOrMergeCollection(store, srcId, dstId, 0);
    if (!ok || !Exec(store, "COMMIT")) {
        SetError(store, StrL("retag collection"));
        Exec(store, "ROLLBACK");
        return false;
    }
    return true;
}

bool LibraryStoreReorderCollection(LibraryStore* store, i64 collectionId, i64 parentId, i64 targetCollectionId,
                                   bool insertAfter) {
    if (!LibraryStoreIsOpen(store) || collectionId <= 0 || targetCollectionId <= 0 ||
        collectionId == targetCollectionId || parentId < 0) {
        return false;
    }
    Vec<LibraryCollection*> all = LibraryStoreGetCollections(store);
    Vec<LibraryCollection*> siblings;
    for (LibraryCollection* c : all) {
        if (c->parentId == parentId) {
            siblings.Append(c);
        }
    }
    int sourceIdx = -1;
    int targetIdx = -1;
    for (int i = 0; i < len(siblings); i++) {
        if (siblings[i]->id == collectionId) {
            sourceIdx = i;
        }
        if (siblings[i]->id == targetCollectionId) {
            targetIdx = i;
        }
    }
    if (sourceIdx < 0 || targetIdx < 0) {
        DeleteLibraryCollections(all);
        return false;
    }
    LibraryCollection* moving = siblings[sourceIdx];
    siblings.RemoveAt(sourceIdx);
    if (sourceIdx < targetIdx) {
        targetIdx--;
    }
    int insertIdx = insertAfter ? targetIdx + 1 : targetIdx;
    if (insertIdx < 0) {
        insertIdx = 0;
    }
    if (insertIdx > len(siblings)) {
        insertIdx = len(siblings);
    }
    siblings.InsertAt(insertIdx, moving);

    if (!Exec(store, "BEGIN IMMEDIATE")) {
        DeleteLibraryCollections(all);
        return false;
    }
    sqlite3_stmt* stmt = Prepare(store, "UPDATE collections SET sort_pos=?1 WHERE id=?2");
    bool ok = stmt != nullptr;
    for (int i = 0; ok && i < len(siblings); i++) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_int64(stmt, 1, i);
        sqlite3_bind_int64(stmt, 2, siblings[i]->id);
        ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(store->db) == 1;
        if (!ok) {
            SetError(store, StrL("reorder collection"));
        }
    }
    if (stmt) {
        sqlite3_finalize(stmt);
    }
    if (!ok || !Exec(store, "COMMIT")) {
        Exec(store, "ROLLBACK");
        DeleteLibraryCollections(all);
        return false;
    }
    DeleteLibraryCollections(all);
    return true;
}

bool LibraryStoreAddBookToCollection(LibraryStore* store, i64 bookId, i64 collectionId) {
    if (!LibraryStoreIsOpen(store)) return false;
    i64 sortPos = NextSortPos(store, collectionId);
    sqlite3_stmt* stmt = Prepare(
        store, "INSERT OR IGNORE INTO book_collections(book_id,collection_id,added_ms,sort_pos) VALUES(?1,?2,?3,?4)");
    if (!stmt) return false;
    sqlite3_bind_int64(stmt, 1, bookId);
    sqlite3_bind_int64(stmt, 2, collectionId);
    sqlite3_bind_int64(stmt, 3, UnixTimeMsNow());
    sqlite3_bind_int64(stmt, 4, sortPos);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) SetError(store, StrL("add book to collection"));
    sqlite3_finalize(stmt);
    return ok;
}

Vec<i64> LibraryStoreGetBookCollectionIds(LibraryStore* store, i64 bookId) {
    Vec<i64> ids;
    if (!LibraryStoreIsOpen(store) || bookId <= 0) return ids;
    sqlite3_stmt* stmt = Prepare(store, "SELECT collection_id FROM book_collections WHERE book_id=?1");
    if (!stmt) return ids;
    sqlite3_bind_int64(stmt, 1, bookId);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ids.Append(sqlite3_column_int64(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return ids;
}

bool LibraryStoreAddBookTag(LibraryStore* store, i64 bookId, i64 collectionId) {
    if (!LibraryStoreIsOpen(store) || bookId <= 0 || collectionId <= 0) return false;
    if (!Exec(store, "BEGIN IMMEDIATE")) return false;
    bool hadTags = false;
    sqlite3_stmt* stmt = Prepare(store, "SELECT 1 FROM book_collections WHERE book_id=?1 LIMIT 1");
    bool ok = stmt != nullptr;
    if (stmt) {
        sqlite3_bind_int64(stmt, 1, bookId);
        hadTags = sqlite3_step(stmt) == SQLITE_ROW;
        sqlite3_finalize(stmt);
    }
    if (ok) {
        i64 sortPos = NextSortPos(store, collectionId);
        stmt = Prepare(store,
                       "INSERT OR IGNORE INTO book_collections(book_id,collection_id,added_ms,sort_pos) "
                       "VALUES(?1,?2,?3,?4)");
        ok = stmt != nullptr;
        if (stmt) {
            sqlite3_bind_int64(stmt, 1, bookId);
            sqlite3_bind_int64(stmt, 2, collectionId);
            sqlite3_bind_int64(stmt, 3, UnixTimeMsNow());
            sqlite3_bind_int64(stmt, 4, sortPos);
            ok = sqlite3_step(stmt) == SQLITE_DONE;
            sqlite3_finalize(stmt);
        }
    }
    if (ok && !hadTags) {
        // first tag: the book moves out of the library root into the tag folder
        stmt = Prepare(store, "DELETE FROM manual_books WHERE book_id=?1");
        ok = stmt != nullptr;
        if (stmt) {
            sqlite3_bind_int64(stmt, 1, bookId);
            ok = sqlite3_step(stmt) == SQLITE_DONE;
            sqlite3_finalize(stmt);
        }
    }
    if (!ok || !Exec(store, "COMMIT")) {
        SetError(store, StrL("add book tag"));
        Exec(store, "ROLLBACK");
        return false;
    }
    return true;
}

static sqlite3_stmt* PrepareBookMembership(LibraryStore* store, i64 collectionId, bool insert) {
    if (collectionId == 0) {
        return Prepare(store, insert ? "INSERT OR IGNORE INTO manual_books(book_id,added_ms,sort_pos) VALUES(?1,?2,?3)"
                                     : "DELETE FROM manual_books WHERE book_id=?1");
    }
    return Prepare(store, insert ? "INSERT OR IGNORE INTO book_collections(book_id,collection_id,added_ms,sort_pos) "
                                   "VALUES(?1,?2,?3,?4)"
                                 : "DELETE FROM book_collections WHERE book_id=?1 AND collection_id=?2");
}

bool LibraryStorePlaceBook(LibraryStore* store, i64 bookId, i64 sourceCollectionId, i64 targetCollectionId, bool copy) {
    if (!LibraryStoreIsOpen(store) || bookId <= 0 || sourceCollectionId < 0 || targetCollectionId < 0 ||
        sourceCollectionId == targetCollectionId) {
        return false;
    }
    if (!Exec(store, "BEGIN IMMEDIATE")) return false;

    sqlite3_stmt* source = Prepare(store, sourceCollectionId == 0
                                              ? "SELECT 1 FROM manual_books WHERE book_id=?1"
                                              : "SELECT 1 FROM book_collections WHERE book_id=?1 AND collection_id=?2");
    if (!source) {
        Exec(store, "ROLLBACK");
        return false;
    }
    sqlite3_bind_int64(source, 1, bookId);
    if (sourceCollectionId > 0) sqlite3_bind_int64(source, 2, sourceCollectionId);
    bool sourceExists = sqlite3_step(source) == SQLITE_ROW;
    sqlite3_finalize(source);
    if (!sourceExists) {
        Exec(store, "ROLLBACK");
        return false;
    }

    i64 sortPos = NextSortPos(store, targetCollectionId);
    sqlite3_stmt* target = PrepareBookMembership(store, targetCollectionId, true);
    bool ok = target != nullptr;
    if (target) {
        sqlite3_bind_int64(target, 1, bookId);
        if (targetCollectionId == 0) {
            sqlite3_bind_int64(target, 2, UnixTimeMsNow());
            sqlite3_bind_int64(target, 3, sortPos);
        } else {
            sqlite3_bind_int64(target, 2, targetCollectionId);
            sqlite3_bind_int64(target, 3, UnixTimeMsNow());
            sqlite3_bind_int64(target, 4, sortPos);
        }
        ok = sqlite3_step(target) == SQLITE_DONE;
        if (!ok) SetError(store, StrL("copy book membership"));
        sqlite3_finalize(target);
    }
    if (ok && !copy) {
        source = PrepareBookMembership(store, sourceCollectionId, false);
        ok = source != nullptr;
        if (source) {
            sqlite3_bind_int64(source, 1, bookId);
            if (sourceCollectionId > 0) sqlite3_bind_int64(source, 2, sourceCollectionId);
            ok = sqlite3_step(source) == SQLITE_DONE && sqlite3_changes(store->db) == 1;
            if (!ok) SetError(store, StrL("move book membership"));
            sqlite3_finalize(source);
        }
    }
    if (!ok || !Exec(store, "COMMIT")) {
        Exec(store, "ROLLBACK");
        return false;
    }
    return true;
}

bool LibraryStoreReorderBook(LibraryStore* store, i64 bookId, i64 collectionId, i64 targetBookId, bool insertAfter) {
    if (!LibraryStoreIsOpen(store) || bookId <= 0 || targetBookId <= 0 || bookId == targetBookId || collectionId < 0) {
        return false;
    }
    LibraryBookScope scope = collectionId == 0 ? LibraryBookScope::ManualRoot : LibraryBookScope::Collection;
    Vec<LibraryBook*> books = LibraryStoreGetBooks(store, scope, collectionId, LibrarySort::Manual, Str());
    int sourceIdx = -1;
    int targetIdx = -1;
    for (int i = 0; i < len(books); i++) {
        if (books[i]->id == bookId) {
            sourceIdx = i;
        }
        if (books[i]->id == targetBookId) {
            targetIdx = i;
        }
    }
    if (sourceIdx < 0 || targetIdx < 0) {
        DeleteLibraryBooks(books);
        return false;
    }
    LibraryBook* moving = books[sourceIdx];
    books.RemoveAt(sourceIdx);
    if (sourceIdx < targetIdx) {
        targetIdx--;
    }
    int insertIdx = insertAfter ? targetIdx + 1 : targetIdx;
    if (insertIdx < 0) {
        insertIdx = 0;
    }
    if (insertIdx > len(books)) {
        insertIdx = len(books);
    }
    books.InsertAt(insertIdx, moving);

    if (!Exec(store, "BEGIN IMMEDIATE")) {
        DeleteLibraryBooks(books);
        return false;
    }
    const char* sql = collectionId == 0 ? "UPDATE manual_books SET sort_pos=?1 WHERE book_id=?2"
                                        : "UPDATE book_collections SET sort_pos=?1 WHERE book_id=?2 AND collection_id=?3";
    sqlite3_stmt* stmt = Prepare(store, sql);
    bool ok = stmt != nullptr;
    for (int i = 0; ok && i < len(books); i++) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_int64(stmt, 1, i);
        sqlite3_bind_int64(stmt, 2, books[i]->id);
        if (collectionId > 0) {
            sqlite3_bind_int64(stmt, 3, collectionId);
        }
        ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(store->db) == 1;
        if (!ok) {
            SetError(store, StrL("reorder book"));
        }
    }
    if (stmt) {
        sqlite3_finalize(stmt);
    }
    if (!ok || !Exec(store, "COMMIT")) {
        Exec(store, "ROLLBACK");
        DeleteLibraryBooks(books);
        return false;
    }
    DeleteLibraryBooks(books);
    return true;
}

bool LibraryStoreSetBookOnDesk(LibraryStore* store, i64 bookId, bool onDesk) {
    if (!LibraryStoreIsOpen(store)) return false;
    const char* sql = onDesk ? "INSERT OR IGNORE INTO desk_books(book_id,added_ms) VALUES(?1,?2)"
                             : "DELETE FROM desk_books WHERE book_id=?1";
    sqlite3_stmt* stmt = Prepare(store, sql);
    if (!stmt) return false;
    sqlite3_bind_int64(stmt, 1, bookId);
    if (onDesk) sqlite3_bind_int64(stmt, 2, UnixTimeMsNow());
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) SetError(store, StrL("update desk"));
    sqlite3_finalize(stmt);
    return ok;
}

bool LibraryStoreRemoveBook(LibraryStore* store, i64 bookId) {
    if (!LibraryStoreIsOpen(store)) return false;
    sqlite3_stmt* stmt = Prepare(store, "DELETE FROM books WHERE id=?1");
    if (!stmt) return false;
    sqlite3_bind_int64(stmt, 1, bookId);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(store->db) == 1;
    sqlite3_finalize(stmt);
    return ok;
}

bool LibraryStoreSetBookBgColor(LibraryStore* store, i64 bookId, u32 bgColor) {
    if (!LibraryStoreIsOpen(store) || bookId <= 0) {
        return false;
    }
    sqlite3_stmt* stmt = Prepare(store, "UPDATE books SET bg_color=?1,updated_ms=?2 WHERE id=?3");
    if (!stmt) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, (i64)bgColor);
    sqlite3_bind_int64(stmt, 2, UnixTimeMsNow());
    sqlite3_bind_int64(stmt, 3, bookId);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(store->db) == 1;
    if (!ok) {
        SetError(store, StrL("set book background color"));
    }
    sqlite3_finalize(stmt);
    return ok;
}

bool LibraryStoreSetCollectionBgColor(LibraryStore* store, i64 collectionId, u32 bgColor) {
    if (!LibraryStoreIsOpen(store) || collectionId <= 0) {
        return false;
    }
    sqlite3_stmt* stmt = Prepare(store, "UPDATE collections SET bg_color=?1 WHERE id=?2");
    if (!stmt) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, (i64)bgColor);
    sqlite3_bind_int64(stmt, 2, collectionId);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(store->db) == 1;
    if (!ok) {
        SetError(store, StrL("set collection background color"));
    }
    sqlite3_finalize(stmt);
    return ok;
}

bool LibraryStoreSetBookNotebookLm(LibraryStore* store, i64 bookId, Str notebooklmJson) {
    if (!LibraryStoreIsOpen(store) || bookId <= 0) {
        return false;
    }
    sqlite3_stmt* stmt = Prepare(store, "UPDATE books SET notebooklm=?1,updated_ms=?2 WHERE id=?3");
    if (!stmt) {
        return false;
    }
    BindText(stmt, 1, notebooklmJson ? notebooklmJson : StrL(""));
    sqlite3_bind_int64(stmt, 2, UnixTimeMsNow());
    sqlite3_bind_int64(stmt, 3, bookId);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(store->db) == 1;
    if (!ok) {
        SetError(store, StrL("set book notebooklm"));
    }
    sqlite3_finalize(stmt);
    return ok;
}

Str LibraryStoreGetBookNotebookLm(LibraryStore* store, i64 bookId) {
    if (!LibraryStoreIsOpen(store) || bookId <= 0) {
        return {};
    }
    sqlite3_stmt* stmt = Prepare(store, "SELECT notebooklm FROM books WHERE id=?1");
    if (!stmt) {
        return {};
    }
    sqlite3_bind_int64(stmt, 1, bookId);
    Str out = {};
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out = ColumnTextDup(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return out;
}

LibraryBook* LibraryStoreFindBookByPath(LibraryStore* store, Str path) {
    if (!LibraryStoreIsOpen(store) || !path) {
        return nullptr;
    }
    Str key = PathKey(NormalizePathTemp(path));
    sqlite3_stmt* stmt =
        Prepare(store, "SELECT id,path,title,open_count,reading_seconds,last_read_ms,0,bg_color,notebooklm,kind,url "
                       "FROM books WHERE path_key=?1");
    if (!stmt) {
        str::Free(key);
        return nullptr;
    }
    BindText(stmt, 1, key);
    str::Free(key);
    LibraryBook* book = nullptr;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        book = ReadBook(stmt);
    }
    sqlite3_finalize(stmt);
    return book;
}

LibraryBook* LibraryStoreFindBookById(LibraryStore* store, i64 bookId) {
    if (!LibraryStoreIsOpen(store) || bookId <= 0) {
        return nullptr;
    }
    sqlite3_stmt* stmt =
        Prepare(store, "SELECT id,path,title,open_count,reading_seconds,last_read_ms,0,bg_color,notebooklm,kind,url "
                       "FROM books WHERE id=?1");
    if (!stmt) {
        return nullptr;
    }
    sqlite3_bind_int64(stmt, 1, bookId);
    LibraryBook* book = nullptr;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        book = ReadBook(stmt);
    }
    sqlite3_finalize(stmt);
    return book;
}

bool LibraryStoreSetBookTitle(LibraryStore* store, i64 bookId, Str title) {
    if (!LibraryStoreIsOpen(store) || bookId <= 0 || !title || title.len == 0) {
        return false;
    }
    sqlite3_stmt* stmt = Prepare(store, "UPDATE books SET title=?1,updated_ms=?2 WHERE id=?3");
    if (!stmt) {
        return false;
    }
    BindText(stmt, 1, title);
    sqlite3_bind_int64(stmt, 2, UnixTimeMsNow());
    sqlite3_bind_int64(stmt, 3, bookId);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(store->db) == 1;
    if (!ok) {
        SetError(store, StrL("set book title"));
    }
    sqlite3_finalize(stmt);
    return ok;
}

bool LibraryStoreSetBookUrl(LibraryStore* store, i64 bookId, Str url) {
    if (!LibraryStoreIsOpen(store) || bookId <= 0 || !url) {
        return false;
    }
    LibraryBook* book = LibraryStoreFindBookById(store, bookId);
    if (!book || book->kind != LibraryBookKind::Web) {
        DeleteLibraryBook(book);
        str::ReplaceWithCopy(&store->error, StrL("not a web book"));
        return false;
    }
    DeleteLibraryBook(book);
    sqlite3_stmt* stmt = Prepare(store, "UPDATE books SET url=?1,updated_ms=?2 WHERE id=?3 AND kind=1");
    if (!stmt) {
        return false;
    }
    BindText(stmt, 1, url);
    sqlite3_bind_int64(stmt, 2, UnixTimeMsNow());
    sqlite3_bind_int64(stmt, 3, bookId);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(store->db) == 1;
    if (!ok) {
        SetError(store, StrL("set book url"));
    }
    sqlite3_finalize(stmt);
    return ok;
}

bool LibraryStoreSetBookPath(LibraryStore* store, i64 bookId, Str newPath) {
    if (!LibraryStoreIsOpen(store) || bookId <= 0 || !newPath || newPath.len == 0) {
        return false;
    }
    LibraryBook* book = LibraryStoreFindBookById(store, bookId);
    if (!book || !book->path) {
        DeleteLibraryBook(book);
        return false;
    }
    if (book->kind == LibraryBookKind::Web) {
        str::ReplaceWithCopy(&store->error, StrL("web books have no filesystem path"));
        DeleteLibraryBook(book);
        return false;
    }
    TempStr normalized = NormalizePathTemp(newPath);
    if (str::EqI(book->path, normalized)) {
        DeleteLibraryBook(book);
        return true;
    }
    Str key = PathKey(normalized);
    sqlite3_stmt* clash = Prepare(store, "SELECT 1 FROM books WHERE path_key=?1 AND id<>?2");
    bool conflict = false;
    if (clash) {
        BindText(clash, 1, key);
        sqlite3_bind_int64(clash, 2, bookId);
        conflict = sqlite3_step(clash) == SQLITE_ROW;
        sqlite3_finalize(clash);
    }
    if (conflict) {
        str::Free(key);
        str::ReplaceWithCopy(&store->error, StrL("path already in library"));
        DeleteLibraryBook(book);
        return false;
    }
    // Path-only update: keep title and reading stats.
    sqlite3_stmt* stmt = Prepare(store, "UPDATE books SET path=?1,path_key=?2,updated_ms=?3 WHERE id=?4");
    bool ok = stmt != nullptr;
    if (ok) {
        BindText(stmt, 1, normalized);
        BindText(stmt, 2, key);
        sqlite3_bind_int64(stmt, 3, UnixTimeMsNow());
        sqlite3_bind_int64(stmt, 4, bookId);
        ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(store->db) == 1;
        sqlite3_finalize(stmt);
    }
    str::Free(key);
    DeleteLibraryBook(book);
    if (!ok) {
        SetError(store, StrL("set book path"));
    }
    return ok;
}

bool LibraryStoreTouchBookOpen(LibraryStore* store, i64 bookId, i64 nowMs) {
    if (!LibraryStoreIsOpen(store) || bookId <= 0) {
        return false;
    }
    sqlite3_stmt* stmt = Prepare(
        store, "UPDATE books SET open_count=open_count+1,last_read_ms=?1,updated_ms=?1 WHERE id=?2");
    if (!stmt) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, nowMs);
    sqlite3_bind_int64(stmt, 2, bookId);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(store->db) == 1;
    if (!ok) {
        SetError(store, StrL("touch book open"));
    }
    sqlite3_finalize(stmt);
    return ok;
}

bool LibraryStoreRenameBookFile(LibraryStore* store, i64 bookId, Str newBaseName, Str* outNewPath) {
    if (outNewPath) {
        *outNewPath = {};
    }
    if (!LibraryStoreIsOpen(store) || bookId <= 0 || !newBaseName || newBaseName.len == 0) {
        return false;
    }
    // Reject path separators in the new name.
    if (str::IndexOfChar(newBaseName, '\\') >= 0 || str::IndexOfChar(newBaseName, '/') >= 0 ||
        str::IndexOfChar(newBaseName, ':') >= 0) {
        str::ReplaceWithCopy(&store->error, StrL("invalid file name"));
        return false;
    }
    LibraryBook* book = LibraryStoreFindBookById(store, bookId);
    if (!book || !book->path) {
        DeleteLibraryBook(book);
        return false;
    }
    if (book->kind == LibraryBookKind::Web) {
        str::ReplaceWithCopy(&store->error, StrL("web books cannot be renamed on disk"));
        DeleteLibraryBook(book);
        return false;
    }
    TempStr dir = path::GetDirTemp(book->path);
    TempStr newPath = path::NormalizeTemp(path::JoinTemp(dir, newBaseName));
    if (str::EqI(book->path, newPath)) {
        DeleteLibraryBook(book);
        if (outNewPath) {
            *outNewPath = str::Dup(newPath);
        }
        return true;
    }
    if (file::Exists(newPath)) {
        str::ReplaceWithCopy(&store->error, StrL("target file already exists"));
        DeleteLibraryBook(book);
        return false;
    }
    Str key = PathKey(newPath);
    sqlite3_stmt* clash = Prepare(store, "SELECT 1 FROM books WHERE path_key=?1 AND id<>?2");
    bool conflict = false;
    if (clash) {
        BindText(clash, 1, key);
        sqlite3_bind_int64(clash, 2, bookId);
        conflict = sqlite3_step(clash) == SQLITE_ROW;
        sqlite3_finalize(clash);
    }
    if (conflict) {
        str::Free(key);
        str::ReplaceWithCopy(&store->error, StrL("path already in library"));
        DeleteLibraryBook(book);
        return false;
    }
    if (!file::Rename(newPath, book->path)) {
        str::Free(key);
        str::ReplaceWithCopy(&store->error, StrL("file rename failed"));
        DeleteLibraryBook(book);
        return false;
    }
    TempStr title = newBaseName;
    sqlite3_stmt* stmt =
        Prepare(store, "UPDATE books SET path=?1,path_key=?2,title=?3,updated_ms=?4 WHERE id=?5");
    bool ok = stmt != nullptr;
    if (ok) {
        BindText(stmt, 1, newPath);
        BindText(stmt, 2, key);
        BindText(stmt, 3, title);
        sqlite3_bind_int64(stmt, 4, UnixTimeMsNow());
        sqlite3_bind_int64(stmt, 5, bookId);
        ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(store->db) == 1;
        sqlite3_finalize(stmt);
    }
    str::Free(key);
    DeleteLibraryBook(book);
    if (!ok) {
        str::ReplaceWithCopy(&store->error, StrL("database update failed after rename"));
        return false;
    }
    if (outNewPath) {
        *outNewPath = str::Dup(newPath);
    }
    return true;
}

static bool HasPrefixBoundary(Str path, Str prefix) {
    if (!str::StartsWithI(path, prefix)) return false;
    if (len(path) == len(prefix) || path.s[len(prefix) - 1] == '\\' || path.s[len(prefix) - 1] == '/') return true;
    char c = path.s[len(prefix)];
    return c == '\\' || c == '/';
}

Vec<LibraryPathChange*> LibraryStorePreviewPathReplace(LibraryStore* store, Str oldPrefix, Str newPrefix) {
    Vec<LibraryPathChange*> changes;
    if (!LibraryStoreIsOpen(store)) return changes;
    TempStr oldNorm = NormalizePathTemp(oldPrefix);
    TempStr newNorm = NormalizePathTemp(newPrefix);
    Vec<LibraryBook*> books = LibraryStoreGetBooks(store, LibraryBookScope::All, 0, LibrarySort::Title, Str());
    for (LibraryBook* book : books) {
        if (book->kind == LibraryBookKind::Web) {
            continue;
        }
        if (!HasPrefixBoundary(book->path, oldNorm)) continue;
        Str tail(book->path.s + len(oldNorm), len(book->path) - len(oldNorm));
        TempStr replaced = str::JoinTemp(newNorm, tail);
        auto* change = new LibraryPathChange();
        change->bookId = book->id;
        change->oldPath = str::Dup(book->path);
        change->newPath = str::Dup(NormalizePathTemp(replaced));
        change->targetExists = file::Exists(change->newPath);
        Str key = PathKey(change->newPath);
        sqlite3_stmt* stmt = Prepare(store, "SELECT 1 FROM books WHERE path_key=?1 AND id<>?2");
        if (stmt) {
            BindText(stmt, 1, key);
            sqlite3_bind_int64(stmt, 2, book->id);
            change->conflict = sqlite3_step(stmt) == SQLITE_ROW;
            sqlite3_finalize(stmt);
        }
        str::Free(key);
        changes.Append(change);
    }
    DeleteLibraryBooks(books);
    return changes;
}

bool LibraryStoreApplyPathReplace(LibraryStore* store, Vec<LibraryPathChange*>& changes) {
    if (!LibraryStoreIsOpen(store) || len(changes) == 0) return false;
    for (LibraryPathChange* change : changes) {
        if (change->conflict) return false;
    }
    if (!Exec(store, "BEGIN IMMEDIATE")) return false;
    sqlite3_stmt* stmt = Prepare(store, "UPDATE books SET path=?1,path_key=?2,updated_ms=?3 WHERE id=?4 AND path=?5");
    bool ok = stmt != nullptr;
    for (LibraryPathChange* change : changes) {
        if (!ok) break;
        Str key = PathKey(change->newPath);
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        BindText(stmt, 1, change->newPath);
        BindText(stmt, 2, key);
        sqlite3_bind_int64(stmt, 3, UnixTimeMsNow());
        sqlite3_bind_int64(stmt, 4, change->bookId);
        BindText(stmt, 5, change->oldPath);
        ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(store->db) == 1;
        str::Free(key);
    }
    sqlite3_finalize(stmt);
    if (ok)
        ok = Exec(store, "COMMIT");
    else
        Exec(store, "ROLLBACK");
    return ok;
}

#if defined(DEBUG)
#include "base/UtAssert.h"

static void AssertBookPaths(Vec<LibraryBook*>& books, const char* first, const char* second, const char* third) {
    utassert(len(books) == 3);
    utassert(str::EqI(books[0]->path, Str(first)));
    utassert(str::EqI(books[1]->path, Str(second)));
    utassert(str::EqI(books[2]->path, Str(third)));
    DeleteLibraryBooks(books);
}

static void TestLibraryStableSorts() {
    TempStr dir = path::JoinTemp(GetTempDirTemp(), StrL("sumatra-library-test"));
    TempStr dbPath = path::JoinTemp(dir, fmt("sort-%lld.db", UnixTimeMsNow()));
    LibraryStore* store = LibraryStoreOpen(dbPath);
    utassert(LibraryStoreIsOpen(store));

    DeleteLibraryBook(LibraryStoreAddBook(store, StrL("C:\\Sort\\B.pdf"), StrL("Same"), 100));
    DeleteLibraryBook(LibraryStoreAddBook(store, StrL("C:\\Sort\\A.pdf"), StrL("Same"), 200));
    DeleteLibraryBook(LibraryStoreAddBook(store, StrL("C:\\Sort\\C.pdf"), StrL("Zed"), 300));
    DeleteLibraryBook(LibraryStoreRecordOpen(store, StrL("C:\\Sort\\B.pdf"), StrL("Same"), 1000));
    DeleteLibraryBook(LibraryStoreRecordOpen(store, StrL("C:\\Sort\\C.pdf"), StrL("Zed"), 2000));
    DeleteLibraryBook(LibraryStoreRecordOpen(store, StrL("C:\\Sort\\C.pdf"), StrL("Zed"), 3000));
    utassert(LibraryStoreAddReadingTime(store, StrL("C:\\Sort\\A.pdf"), 30, 4000));
    utassert(LibraryStoreAddReadingTime(store, StrL("C:\\Sort\\B.pdf"), 10, 5000));
    utassert(LibraryStoreAddReadingTime(store, StrL("C:\\Sort\\C.pdf"), 20, 6000));

    Vec<LibraryBook*> books = LibraryStoreGetBooks(store, LibraryBookScope::All, 0, LibrarySort::OpenCount, Str());
    AssertBookPaths(books, "C:\\Sort\\C.pdf", "C:\\Sort\\B.pdf", "C:\\Sort\\A.pdf");
    books = LibraryStoreGetBooks(store, LibraryBookScope::All, 0, LibrarySort::ReadingTime, Str());
    AssertBookPaths(books, "C:\\Sort\\A.pdf", "C:\\Sort\\C.pdf", "C:\\Sort\\B.pdf");
    books = LibraryStoreGetBooks(store, LibraryBookScope::All, 0, LibrarySort::Recent, Str());
    AssertBookPaths(books, "C:\\Sort\\C.pdf", "C:\\Sort\\B.pdf", "C:\\Sort\\A.pdf");
    books = LibraryStoreGetBooks(store, LibraryBookScope::All, 0, LibrarySort::Title, Str());
    AssertBookPaths(books, "C:\\Sort\\A.pdf", "C:\\Sort\\B.pdf", "C:\\Sort\\C.pdf");

    LibraryStoreClose(store);
    utassert(file::Delete(dbPath));
}

static void TestLibraryNaturalTitleSort() {
    TempStr dir = path::JoinTemp(GetTempDirTemp(), StrL("sumatra-library-test"));
    TempStr dbPath = path::JoinTemp(dir, fmt("natural-sort-%lld.db", UnixTimeMsNow()));
    LibraryStore* store = LibraryStoreOpen(dbPath);
    utassert(LibraryStoreIsOpen(store));

    DeleteLibraryBook(LibraryStoreAddBook(store, StrL("C:\\Sort\\A.pdf"), StrL("Book 10"), 100));
    DeleteLibraryBook(LibraryStoreAddBook(store, StrL("C:\\Sort\\B.pdf"), StrL("Book 2"), 100));
    DeleteLibraryBook(LibraryStoreAddBook(store, StrL("C:\\Sort\\C.pdf"), StrL("Book 1"), 100));
    Vec<LibraryBook*> books = LibraryStoreGetBooks(store, LibraryBookScope::All, 0, LibrarySort::Title, Str());
    AssertBookPaths(books, "C:\\Sort\\C.pdf", "C:\\Sort\\B.pdf", "C:\\Sort\\A.pdf");

    LibraryStoreClose(store);
    utassert(file::Delete(dbPath));
}

static void TestLibraryManualReorder() {
    TempStr dir = path::JoinTemp(GetTempDirTemp(), StrL("sumatra-library-test"));
    TempStr dbPath = path::JoinTemp(dir, fmt("reorder-%lld.db", UnixTimeMsNow()));
    LibraryStore* store = LibraryStoreOpen(dbPath);
    utassert(LibraryStoreIsOpen(store));

    LibraryBook* a = LibraryStoreAddBook(store, StrL("C:\\Reorder\\A.pdf"), StrL("A"), 100);
    LibraryBook* b = LibraryStoreAddBook(store, StrL("C:\\Reorder\\B.pdf"), StrL("B"), 100);
    LibraryBook* c = LibraryStoreAddBook(store, StrL("C:\\Reorder\\C.pdf"), StrL("C"), 100);
    utassert(a && b && c);
    i64 idA = a->id, idB = b->id, idC = c->id;
    DeleteLibraryBook(a);
    DeleteLibraryBook(b);
    DeleteLibraryBook(c);

    Vec<LibraryBook*> books = LibraryStoreGetBooks(store, LibraryBookScope::ManualRoot, 0, LibrarySort::Manual, Str());
    AssertBookPaths(books, "C:\\Reorder\\A.pdf", "C:\\Reorder\\B.pdf", "C:\\Reorder\\C.pdf");

    utassert(LibraryStoreReorderBook(store, idC, 0, idA, false));
    books = LibraryStoreGetBooks(store, LibraryBookScope::ManualRoot, 0, LibrarySort::Manual, Str());
    AssertBookPaths(books, "C:\\Reorder\\C.pdf", "C:\\Reorder\\A.pdf", "C:\\Reorder\\B.pdf");

    utassert(LibraryStoreReorderBook(store, idA, 0, idB, true));
    books = LibraryStoreGetBooks(store, LibraryBookScope::ManualRoot, 0, LibrarySort::Manual, Str());
    AssertBookPaths(books, "C:\\Reorder\\C.pdf", "C:\\Reorder\\B.pdf", "C:\\Reorder\\A.pdf");

    LibraryCollection* shelf = LibraryStoreCreateCollection(store, 0, true, StrL("Shelf"));
    LibraryCollection* cat = LibraryStoreCreateCollection(store, shelf->id, false, StrL("Cat"));
    utassert(LibraryStorePlaceBook(store, idA, 0, cat->id, false));
    utassert(LibraryStorePlaceBook(store, idB, 0, cat->id, false));
    utassert(LibraryStorePlaceBook(store, idC, 0, cat->id, false));
    books = LibraryStoreGetBooks(store, LibraryBookScope::Collection, cat->id, LibrarySort::Manual, Str());
    AssertBookPaths(books, "C:\\Reorder\\A.pdf", "C:\\Reorder\\B.pdf", "C:\\Reorder\\C.pdf");
    utassert(LibraryStoreReorderBook(store, idB, cat->id, idC, true));
    books = LibraryStoreGetBooks(store, LibraryBookScope::Collection, cat->id, LibrarySort::Manual, Str());
    AssertBookPaths(books, "C:\\Reorder\\A.pdf", "C:\\Reorder\\C.pdf", "C:\\Reorder\\B.pdf");

    DeleteLibraryCollection(cat);
    DeleteLibraryCollection(shelf);

    LibraryCollection* shelf2 = LibraryStoreCreateCollection(store, 0, true, StrL("书架"));
    LibraryCollection* web = LibraryStoreCreateCollection(store, 0, false, StrL("网页"));
    utassert(shelf2 && web);
    i64 idShelf = shelf2->id, idWeb = web->id;
    DeleteLibraryCollection(shelf2);
    DeleteLibraryCollection(web);
    utassert(LibraryStoreReorderCollection(store, idWeb, 0, idShelf, false));
    Vec<LibraryCollection*> cols = LibraryStoreGetCollections(store);
    utassert(len(cols) >= 2);
    utassert(cols[0]->id == idWeb && cols[0]->parentId == 0);
    utassert(cols[1]->id == idShelf && cols[1]->parentId == 0);
    DeleteLibraryCollections(cols);

    LibraryStoreClose(store);
    utassert(file::Delete(dbPath));
}

static void TestLibraryPathReplaceAndPersistence() {
    TempStr dir = path::JoinTemp(GetTempDirTemp(), StrL("sumatra-library-test"));
    TempStr dbPath = path::JoinTemp(dir, fmt("paths-%lld.db", UnixTimeMsNow()));
    LibraryStore* store = LibraryStoreOpen(dbPath);
    utassert(LibraryStoreIsOpen(store));

    LibraryBook* a = LibraryStoreAddBook(store, StrL("C:\\Old\\A.pdf"), StrL("A"), 100);
    LibraryBook* b = LibraryStoreAddBook(store, StrL("C:\\Old\\B.pdf"), StrL("B"), 200);
    LibraryBook* existing = LibraryStoreAddBook(store, StrL("D:\\Dest\\A.pdf"), StrL("Existing"), 300);
    LibraryBook* chinese = LibraryStoreAddBook(store, StrL("C:\\旧书\\中文.pdf"), StrL("中文"), 400);
    utassert(a && b && existing && chinese);

    Vec<LibraryPathChange*> changes = LibraryStorePreviewPathReplace(store, StrL("C:\\Ol"), StrL("D:\\Wrong"));
    utassert(len(changes) == 0);
    DeleteLibraryPathChanges(changes);
    changes = LibraryStorePreviewPathReplace(store, StrL("c:\\OLD"), StrL("D:\\Dest"));
    utassert(len(changes) == 2 && changes[0]->conflict && !changes[1]->conflict);
    utassert(!LibraryStoreApplyPathReplace(store, changes));
    DeleteLibraryPathChanges(changes);

    changes = LibraryStorePreviewPathReplace(store, StrL("C:\\Old"), StrL("E:\\Moved"));
    utassert(len(changes) == 2 && !changes[0]->conflict && !changes[1]->conflict);
    str::ReplaceWithCopy(&changes[1]->oldPath, StrL("C:\\Old\\Missing.pdf"));
    utassert(!LibraryStoreApplyPathReplace(store, changes));
    DeleteLibraryPathChanges(changes);
    changes = LibraryStorePreviewPathReplace(store, StrL("C:\\Old"), StrL("E:\\Moved"));
    utassert(len(changes) == 2);
    DeleteLibraryPathChanges(changes);

    changes = LibraryStorePreviewPathReplace(store, StrL("c:\\旧书"), StrL("D:\\新书"));
    utassert(len(changes) == 1 && str::EqI(changes[0]->newPath, StrL("D:\\新书\\中文.pdf")));
    DeleteLibraryPathChanges(changes);

    LibraryCollection* shelf = LibraryStoreCreateCollection(store, 0, true, StrL("Shelf"));
    LibraryCollection* child = LibraryStoreCreateCollection(store, shelf->id, false, StrL("Child"));
    utassert(shelf && child);
    utassert(LibraryStoreRenameCollection(store, child->id, StrL("Renamed")));
    utassert(!LibraryStoreRenameCollection(store, child->id, StrL("")));
    utassert(LibraryStoreAddBookToCollection(store, a->id, child->id));
    utassert(LibraryStoreSetBookOnDesk(store, a->id, true));
    utassert(LibraryStoreDeleteCollection(store, shelf->id));
    Vec<LibraryCollection*> collections = LibraryStoreGetCollections(store);
    utassert(len(collections) == 0);
    DeleteLibraryCollections(collections);
    Vec<LibraryBook*> unclassified =
        LibraryStoreGetBooks(store, LibraryBookScope::Unclassified, 0, LibrarySort::Title, Str());
    utassert(len(unclassified) == 4);
    DeleteLibraryBooks(unclassified);

    DeleteLibraryCollection(shelf);
    DeleteLibraryCollection(child);
    DeleteLibraryBook(a);
    DeleteLibraryBook(b);
    DeleteLibraryBook(existing);
    DeleteLibraryBook(chinese);
    LibraryStoreClose(store);
    store = LibraryStoreOpen(dbPath);
    utassert(LibraryStoreIsOpen(store));
    Vec<LibraryBook*> desk = LibraryStoreGetBooks(store, LibraryBookScope::Desk, 0, LibrarySort::Title, Str());
    utassert(len(desk) == 1 && str::EqI(desk[0]->path, StrL("C:\\Old\\A.pdf")));
    DeleteLibraryBooks(desk);
    LibraryStoreClose(store);
    utassert(file::Delete(dbPath));
}

void LibraryStore_UnitTests() {
    TempStr dir = path::JoinTemp(GetTempDirTemp(), StrL("sumatra-library-test"));
    dir::CreateAll(dir);
    TempStr dbPath = path::JoinTemp(dir, fmt("library-%lld.db", UnixTimeMsNow()));
    LibraryStore* store = LibraryStoreOpen(dbPath);
    utassert(LibraryStoreIsOpen(store));
    LibraryBook* a = LibraryStoreRecordOpen(store, StrL("C:\\Books\\Alpha.pdf"), StrL("Alpha"), 1000);
    LibraryBook* b = LibraryStoreRecordOpen(store, StrL("C:\\Books\\Beta.pdf"), StrL("Beta"), 2000);
    utassert(a && b && a->id != b->id);
    DeleteLibraryBook(LibraryStoreRecordOpen(store, StrL("c:\\books\\alpha.pdf"), StrL("Alpha"), 3000));
    LibraryBook* imported = LibraryStoreAddBook(store, StrL("C:\\Books\\100%_Guide.pdf"), StrL("100%_Guide"), 3500);
    utassert(imported && imported->openCount == 0);
    DeleteLibraryBook(LibraryStoreAddBook(store, imported->path, imported->title, 3600));
    LibraryBook* legacy = LibraryStoreImportBook(store, StrL("C:\\Books\\Legacy.pdf"), StrL("Legacy"), 12, 3700);
    utassert(legacy && legacy->openCount == 12);
    DeleteLibraryBook(legacy);
    legacy = LibraryStoreImportBook(store, StrL("c:\\books\\legacy.pdf"), StrL("Legacy"), 12, 3800);
    utassert(legacy && legacy->openCount == 12);
    DeleteLibraryBook(legacy);
    legacy = LibraryStoreImportBook(store, StrL("C:\\Books\\Legacy.pdf"), StrL("Legacy"), 8, 3900);
    utassert(legacy && legacy->openCount == 12);
    DeleteLibraryBook(legacy);
    Vec<LibraryBook*> manualRoot =
        LibraryStoreGetBooks(store, LibraryBookScope::ManualRoot, 0, LibrarySort::Title, Str());
    utassert(len(manualRoot) == 3 && manualRoot[0]->id == imported->id && manualRoot[1]->id == a->id &&
             manualRoot[2]->id == b->id);
    DeleteLibraryBooks(manualRoot);
    utassert(LibraryStoreAddReadingTime(store, a->path, 90, 4000));
    LibraryCollection* shelf = LibraryStoreCreateCollection(store, 0, true, StrL("Work"));
    LibraryCollection* category = LibraryStoreCreateCollection(store, shelf->id, false, StrL("Architecture"));
    utassert(shelf && category);
    // Normal drag moves from the root. Reopening an organized book updates its
    // statistics without putting it back at the root.
    utassert(LibraryStorePlaceBook(store, a->id, 0, category->id, false));
    DeleteLibraryBook(LibraryStoreRecordOpen(store, a->path, a->title, 4100));
    manualRoot = LibraryStoreGetBooks(store, LibraryBookScope::ManualRoot, 0, LibrarySort::Title, Str());
    utassert(len(manualRoot) == 2 && manualRoot[0]->id == imported->id && manualRoot[1]->id == b->id);
    DeleteLibraryBooks(manualRoot);
    // Nested folders/tags: category under category is allowed (unlimited depth).
    LibraryCollection* nested = LibraryStoreCreateCollection(store, category->id, false, StrL("Nested"));
    utassert(nested);
    DeleteLibraryCollection(nested);
    LibraryCollection* rootFolder = LibraryStoreCreateCollection(store, 0, false, StrL("RootTag"));
    utassert(rootFolder);
    DeleteLibraryCollection(rootFolder);
    LibraryCollection* other = LibraryStoreCreateCollection(store, shelf->id, false, StrL("Other"));
    utassert(other);
    // Ctrl-drag copies. A later normal drag to the same destination removes
    // only the source membership and keeps the existing destination.
    utassert(LibraryStorePlaceBook(store, a->id, category->id, other->id, true));
    utassert(LibraryStorePlaceBook(store, a->id, category->id, other->id, false));
    Vec<LibraryBook*> categoryBooks =
        LibraryStoreGetBooks(store, LibraryBookScope::Collection, category->id, LibrarySort::Title, Str());
    Vec<LibraryBook*> otherBooks =
        LibraryStoreGetBooks(store, LibraryBookScope::Collection, other->id, LibrarySort::Title, Str());
    utassert(len(categoryBooks) == 0 && len(otherBooks) == 1 && otherBooks[0]->id == a->id);
    DeleteLibraryBooks(categoryBooks);
    DeleteLibraryBooks(otherBooks);
    // Copy and then move a root book proves that root is a real membership,
    // not a derived "unclassified" view.
    utassert(LibraryStorePlaceBook(store, b->id, 0, category->id, true));
    manualRoot = LibraryStoreGetBooks(store, LibraryBookScope::ManualRoot, 0, LibrarySort::Title, Str());
    utassert(len(manualRoot) == 2);
    DeleteLibraryBooks(manualRoot);
    utassert(LibraryStorePlaceBook(store, b->id, 0, category->id, false));
    manualRoot = LibraryStoreGetBooks(store, LibraryBookScope::ManualRoot, 0, LibrarySort::Title, Str());
    utassert(len(manualRoot) == 1 && manualRoot[0]->id == imported->id);
    DeleteLibraryBooks(manualRoot);
    LibraryCollection* shelf2 = LibraryStoreCreateCollection(store, 0, true, StrL("More"));
    utassert(shelf2);
    utassert(!LibraryStoreMoveCollection(store, shelf->id, other->id));
    // Folder under folder is allowed.
    utassert(LibraryStoreMoveCollection(store, other->id, category->id));
    utassert(LibraryStoreMoveCollection(store, other->id, shelf->id));
    // Folder may return to library root (stays a folder/tag).
    utassert(LibraryStoreMoveCollection(store, other->id, 0));
    utassert(LibraryStoreMoveCollection(store, other->id, shelf->id));
    utassert(LibraryStoreDeleteCollection(store, shelf2->id));
    DeleteLibraryCollection(shelf2);
    utassert(LibraryStoreSetBookOnDesk(store, a->id, true));
    Vec<LibraryBook*> all = LibraryStoreGetBooks(store, LibraryBookScope::All, 0, LibrarySort::OpenCount, Str());
    utassert(len(all) == 4 && str::EqI(all[0]->path, StrL("C:\\Books\\Legacy.pdf")));
    DeleteLibraryBooks(all);
    Vec<LibraryBook*> escapedFilter =
        LibraryStoreGetBooks(store, LibraryBookScope::All, 0, LibrarySort::Title, StrL("%_"));
    utassert(len(escapedFilter) == 1 && escapedFilter[0]->id == imported->id);
    DeleteLibraryBooks(escapedFilter);
    Vec<LibraryBook*> desk = LibraryStoreGetBooks(store, LibraryBookScope::Desk, 0, LibrarySort::Title, Str());
    utassert(len(desk) == 1 && desk[0]->id == a->id);
    DeleteLibraryBooks(desk);
    Vec<LibraryBook*> unclassified =
        LibraryStoreGetBooks(store, LibraryBookScope::Unclassified, 0, LibrarySort::Title, Str());
    utassert(len(unclassified) == 2 && unclassified[0]->id == imported->id &&
             str::EqI(unclassified[1]->path, StrL("C:\\Books\\Legacy.pdf")));
    DeleteLibraryBooks(unclassified);
    Vec<LibraryPathChange*> changes = LibraryStorePreviewPathReplace(store, StrL("C:\\Books"), StrL("D:\\Moved Books"));
    utassert(len(changes) == 4);
    utassert(LibraryStoreApplyPathReplace(store, changes));
    DeleteLibraryPathChanges(changes);
    utassert(LibraryStoreDeleteCollection(store, shelf->id));
    manualRoot = LibraryStoreGetBooks(store, LibraryBookScope::ManualRoot, 0, LibrarySort::Title, Str());
    utassert(len(manualRoot) == 3 && manualRoot[0]->id == imported->id && manualRoot[1]->id == a->id &&
             manualRoot[2]->id == b->id);
    DeleteLibraryBooks(manualRoot);
    DeleteLibraryCollection(category);
    DeleteLibraryCollection(other);
    DeleteLibraryCollection(shelf);
    DeleteLibraryBook(a);
    DeleteLibraryBook(b);
    DeleteLibraryBook(imported);
    LibraryStoreClose(store);
    sqlite3* rawDb = nullptr;
    utassert(sqlite3_open(CStrTemp(dbPath), &rawDb) == SQLITE_OK);
    utassert(sqlite3_exec(rawDb, "PRAGMA user_version=1", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(rawDb);
    store = LibraryStoreOpen(dbPath);
    utassert(LibraryStoreIsOpen(store));
    LibraryStoreClose(store);
    utassert(sqlite3_open(CStrTemp(dbPath), &rawDb) == SQLITE_OK);
    utassert(sqlite3_exec(rawDb, "PRAGMA user_version=99", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(rawDb);
    store = LibraryStoreOpen(dbPath);
    utassert(!LibraryStoreIsOpen(store));
    LibraryStoreClose(store);
    utassert(file::Delete(dbPath));
    TestLibraryStableSorts();
    TestLibraryNaturalTitleSort();
    TestLibraryManualReorder();
    TestLibraryPathReplaceAndPersistence();
}
#endif
