#pragma once

#include "model.h"

#include <functional>
#include <string>
#include <vector>

namespace roomy::fs {

std::wstring longPath(const std::wstring& p);  // adds the \\?\ prefix so deep node_modules work
std::wstring join(const std::wstring& a, const std::wstring& b);
std::wstring parent(const std::wstring& p);
std::wstring name(const std::wstring& p);
std::wstring lower(std::wstring s);
std::wstring extension(const std::wstring& p);  // lowercase, with the dot
bool startsWith(const std::wstring& path, const std::wstring& prefix);  // case-insensitive, whole folders only
std::wstring commonFolder(const std::vector<std::wstring>& paths);

DWORD attributes(const std::wstring& p);
bool isDir(const std::wstring& p);
bool isFile(const std::wstring& p);

int64_t toUnix(const FILETIME& ft);
int64_t now();
uint64_t fileSize(const WIN32_FIND_DATAW& fd);

// True for OneDrive and other cloud files that aren't on this disk. They take no space,
// and reading one would download it.
bool cloudOnly(DWORD attrs);
bool isLink(DWORD attrs);

std::vector<WIN32_FIND_DATAW> list(const std::wstring& dir);
std::vector<std::wstring> subdirs(const std::wstring& dir);
bool findOne(const std::wstring& path, WIN32_FIND_DATAW& out);
std::string readSmall(const std::wstring& path, size_t max = 8192);

// Folder walking spread over several threads. SSDs answer many small requests at once
// far faster than one after another.
struct WalkTask {
    std::wstring dir;
    int depth = 0;
    unsigned flags = 0;
};
using Visit = std::function<void(const WalkTask& task, int worker, std::vector<WalkTask>& subfolders)>;
int workerCount();
void parallelWalk(std::vector<WalkTask> start, Progress* p, const Visit& visit);
void parallelFor(size_t n, const std::function<void(size_t i, int worker)>& fn);

struct Stats {
    uint64_t bytes = 0;
    uint64_t files = 0;
    int64_t newest = 0;
};

// Adds up everything under dir. Junctions and symlinks are never followed.
void measure(const std::wstring& dir, int64_t olderThan, Stats& out, Progress* p);

struct DeleteStats {
    uint64_t freed = 0;
    uint64_t deleted = 0;
    uint64_t skipped = 0;
};

// Empties a folder. A junction or symlink inside is removed as a link; what it points to is left alone.
void deleteContents(const std::wstring& dir, int64_t olderThan, DeleteStats& st, Progress* p);
// Removes a file or a whole folder, with the same rule for links.
void deleteTree(const std::wstring& path, DeleteStats& st, Progress* p);

}  // namespace roomy::fs
