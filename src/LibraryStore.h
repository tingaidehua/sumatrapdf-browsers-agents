/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

struct LibraryStore;

enum class LibrarySort {
    OpenCount,
    ReadingTime,
    Recent,
    Title,
    Manual,
};

enum class LibraryBookScope {
    All,
    Desk,
    Unclassified,
    Collection,
    ManualRoot,
};

enum class LibraryBookKind {
    Pdf = 0,
    Web = 1,
};

struct LibraryBook {
    i64 id = 0;
    Str path;
    Str title;
    Str url; // web books only; empty for PDF
    LibraryBookKind kind = LibraryBookKind::Pdf;
    i64 openCount = 0;
    i64 readingSeconds = 0;
    i64 lastReadMs = 0;
    i64 sortPos = 0;
    u32 bgColor = 0; // 0 = none, else 0x00RRGGBB
    // JSON blob for NotebookLM placement, e.g.
    // {"notebook":"SumatraPDF1","notebookId":"...","notebookUrl":"...","sourceTitle":"...","updatedMs":0}
    Str notebooklm;
    // LibraryStoreGetPlacedBooks only: collection the row is placed in, 0 = library root
    i64 placementId = 0;
};

struct LibraryCollection {
    i64 id = 0;
    i64 parentId = 0;
    bool isShelf = false;
    Str name;
    u32 bgColor = 0; // 0 = none, else 0x00RRGGBB
    i64 sortPos = 0;
};

struct LibraryPathChange {
    i64 bookId = 0;
    Str oldPath;
    Str newPath;
    bool targetExists = false;
    bool conflict = false;
};

LibraryStore* LibraryStoreOpen(Str dbPath);
void LibraryStoreClose(LibraryStore* store);
bool LibraryStoreIsOpen(LibraryStore* store);
Str LibraryStoreError(LibraryStore* store);

LibraryBook* LibraryStoreRecordOpen(LibraryStore* store, Str path, Str title, i64 nowMs, bool* placedAtRoot = nullptr);
LibraryBook* LibraryStoreAddBook(LibraryStore* store, Str path, Str title, i64 nowMs);
// Web page as a first-class library book (stable id; url/title are attributes).
LibraryBook* LibraryStoreAddWebBook(LibraryStore* store, Str url, Str title, i64 nowMs);
LibraryBook* LibraryStoreImportBook(LibraryStore* store, Str path, Str title, i64 openCount, i64 nowMs);
bool LibraryStoreAddReadingTime(LibraryStore* store, Str path, i64 seconds, i64 nowMs);
Vec<LibraryBook*> LibraryStoreGetBooks(LibraryStore* store, LibraryBookScope scope, i64 collectionId, LibrarySort sort,
                                       Str filter);
// Every folder / root placement in one query (a book copied into N places appears N times),
// ordered by placementId then manual order — same per-placement order as LibrarySort::Manual.
Vec<LibraryBook*> LibraryStoreGetPlacedBooks(LibraryStore* store, Str filter);
Vec<LibraryCollection*> LibraryStoreGetCollections(LibraryStore* store);
// Bulk fake rows in one transaction; -for-testing perf runs only.
bool LibraryStoreSeedForTesting(LibraryStore* store, int nFolders, int booksPerFolder, int rootBooks);
LibraryCollection* LibraryStoreCreateCollection(LibraryStore* store, i64 parentId, bool isShelf, Str name);
bool LibraryStoreDeleteCollection(LibraryStore* store, i64 collectionId);
bool LibraryStoreRenameCollection(LibraryStore* store, i64 collectionId, Str name);
bool LibraryStoreMoveCollection(LibraryStore* store, i64 collectionId, i64 newParentId);
// Folders are multi-level tags: retagging folder "X" to "T" turns every "X/..." tag into "T/X/...",
// i.e. X moves under dstId (0 = library root). If T already has a same-named folder the two merge
// (books and sub-folders, recursively). srcId == dstId is a no-op; dstId inside X is rejected.
bool LibraryStoreRetagCollection(LibraryStore* store, i64 srcId, i64 dstId);
// Reorder a folder/shelf among siblings under parentId (0 = library root).
bool LibraryStoreReorderCollection(LibraryStore* store, i64 collectionId, i64 parentId, i64 targetCollectionId,
                                   bool insertAfter);
bool LibraryStoreAddBookToCollection(LibraryStore* store, i64 bookId, i64 collectionId);
// Folders act as tags. A book with no tag yet moves from the library root into the folder;
// a book that already has tags keeps them and additionally shows up in this folder.
bool LibraryStoreAddBookTag(LibraryStore* store, i64 bookId, i64 collectionId);
Vec<i64> LibraryStoreGetBookCollectionIds(LibraryStore* store, i64 bookId);
// Collection id 0 denotes the visible Library root. A move removes the source
// membership; a copy preserves it and adds the destination membership.
bool LibraryStorePlaceBook(LibraryStore* store, i64 bookId, i64 sourceCollectionId, i64 targetCollectionId, bool copy);
// Reorder a book among siblings in collectionId (0 = library root). Places
// bookId immediately before or after targetBookId and renumbers sort_pos.
bool LibraryStoreReorderBook(LibraryStore* store, i64 bookId, i64 collectionId, i64 targetBookId, bool insertAfter);
bool LibraryStoreSetBookOnDesk(LibraryStore* store, i64 bookId, bool onDesk);
bool LibraryStoreRemoveBook(LibraryStore* store, i64 bookId);
bool LibraryStoreSetBookBgColor(LibraryStore* store, i64 bookId, u32 bgColor);
bool LibraryStoreSetCollectionBgColor(LibraryStore* store, i64 collectionId, u32 bgColor);
bool LibraryStoreSetBookNotebookLm(LibraryStore* store, i64 bookId, Str notebooklmJson);
Str LibraryStoreGetBookNotebookLm(LibraryStore* store, i64 bookId); // owned; caller frees
LibraryBook* LibraryStoreFindBookByPath(LibraryStore* store, Str path); // owned; caller DeleteLibraryBook
LibraryBook* LibraryStoreFindBookById(LibraryStore* store, i64 bookId); // owned; caller DeleteLibraryBook
// Rename file on disk path + update books.path/title/path_key. newBaseName includes .pdf.
// Rejects web books (use LibraryStoreSetBookTitle instead).
bool LibraryStoreRenameBookFile(LibraryStore* store, i64 bookId, Str newBaseName, Str* outNewPath);
// Title-only rename (PDF display name or web book label).
bool LibraryStoreSetBookTitle(LibraryStore* store, i64 bookId, Str title);
bool LibraryStoreSetBookUrl(LibraryStore* store, i64 bookId, Str url);
// Update PDF path/path_key only (file moved elsewhere). Keeps title and other stats.
bool LibraryStoreSetBookPath(LibraryStore* store, i64 bookId, Str newPath);
bool LibraryStoreTouchBookOpen(LibraryStore* store, i64 bookId, i64 nowMs);

Vec<LibraryPathChange*> LibraryStorePreviewPathReplace(LibraryStore* store, Str oldPrefix, Str newPrefix);
bool LibraryStoreApplyPathReplace(LibraryStore* store, Vec<LibraryPathChange*>& changes);

void DeleteLibraryBook(LibraryBook* book);
void DeleteLibraryBooks(Vec<LibraryBook*>& books);
void DeleteLibraryCollection(LibraryCollection* collection);
void DeleteLibraryCollections(Vec<LibraryCollection*>& collections);
void DeleteLibraryPathChange(LibraryPathChange* change);
void DeleteLibraryPathChanges(Vec<LibraryPathChange*>& changes);

#if defined(DEBUG)
void LibraryStore_UnitTests();
#endif
