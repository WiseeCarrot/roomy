#include "fs.h"

#include <algorithm>
#include <condition_variable>
#include <ctime>
#include <cwctype>
#include <deque>
#include <thread>

#ifndef FILE_ATTRIBUTE_RECALL_ON_OPEN
#define FILE_ATTRIBUTE_RECALL_ON_OPEN 0x00040000
#endif
#ifndef FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
#define FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS 0x00400000
#endif

namespace roomy::fs {

std::wstring longPath(const std::wstring& p) {
    if (p.rfind(L"\\\\?\\", 0) == 0) return p;
    if (p.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + p.substr(2);
    if (p.size() >= 2 && p[1] == L':') return L"\\\\?\\" + p;
    return p;
}

std::wstring join(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (a.back() == L'\\') return a + b;
    return a + L"\\" + b;
}

std::wstring parent(const std::wstring& p) {
    size_t i = p.find_last_of(L'\\');
    if (i == std::wstring::npos) return L"";
    if (i == 2 && p.size() > 1 && p[1] == L':') return p.substr(0, 3);
    return p.substr(0, i);
}

std::wstring name(const std::wstring& p) {
    size_t i = p.find_last_of(L'\\');
    return i == std::wstring::npos ? p : p.substr(i + 1);
}

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = wchar_t(std::towlower(c));
    return s;
}

std::wstring extension(const std::wstring& p) {
    std::wstring n = name(p);
    size_t i = n.find_last_of(L'.');
    return i == std::wstring::npos || i == 0 ? L"" : lower(n.substr(i));
}

bool startsWith(const std::wstring& path, const std::wstring& prefix) {
    if (prefix.empty() || path.size() < prefix.size()) return false;
    if (_wcsnicmp(path.c_str(), prefix.c_str(), prefix.size()) != 0) return false;
    return path.size() == prefix.size() || prefix.back() == L'\\' || path[prefix.size()] == L'\\';
}

std::wstring commonFolder(const std::vector<std::wstring>& paths) {
    if (paths.empty()) return L"";
    std::wstring c = paths[0];
    for (auto& p : paths)
        while (!c.empty() && !startsWith(p, c)) c = parent(c);
    return c;
}

DWORD attributes(const std::wstring& p) { return GetFileAttributesW(longPath(p).c_str()); }

bool isDir(const std::wstring& p) {
    DWORD a = attributes(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool isFile(const std::wstring& p) {
    DWORD a = attributes(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

int64_t toUnix(const FILETIME& ft) {
    uint64_t t = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    if (t == 0) return 0;
    return int64_t(t / 10000000ULL) - 11644473600LL;
}

int64_t now() { return int64_t(std::time(nullptr)); }

uint64_t fileSize(const WIN32_FIND_DATAW& fd) { return (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow; }

bool cloudOnly(DWORD a) {
    return a & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS);
}

bool isLink(DWORD a) { return a & FILE_ATTRIBUTE_REPARSE_POINT; }

static bool dots(const wchar_t* n) { return n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0)); }

std::vector<WIN32_FIND_DATAW> list(const std::wstring& dir) {
    std::vector<WIN32_FIND_DATAW> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(longPath(join(dir, L"*")).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!dots(fd.cFileName)) out.push_back(fd);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

std::vector<std::wstring> subdirs(const std::wstring& dir) {
    std::vector<std::wstring> out;
    for (auto& fd : list(dir))
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !isLink(fd.dwFileAttributes))
            out.push_back(join(dir, fd.cFileName));
    return out;
}

bool findOne(const std::wstring& path, WIN32_FIND_DATAW& out) {
    HANDLE h = FindFirstFileExW(longPath(path).c_str(), FindExInfoBasic, &out, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return false;
    FindClose(h);
    return true;
}

std::string readSmall(const std::wstring& path, size_t max) {
    HANDLE h = CreateFileW(longPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::string buf(max, '\0');
    DWORD got = 0;
    ReadFile(h, buf.data(), DWORD(max), &got, nullptr);
    CloseHandle(h);
    buf.resize(got);
    return buf;
}

int workerCount() {
    static const int n = std::clamp(int(std::thread::hardware_concurrency()), 2, 8);
    return n;
}

void parallelWalk(std::vector<WalkTask> start, Progress* p, const Visit& visit) {
    std::mutex m;
    std::condition_variable cv;
    std::deque<WalkTask> queue(start.begin(), start.end());
    int active = 0;
    auto work = [&](int worker) {
        std::vector<WalkTask> found;
        for (;;) {
            WalkTask task;
            {
                std::unique_lock<std::mutex> lock(m);
                // Wait for work, or until nobody is left who could produce more.
                cv.wait(lock, [&] { return !queue.empty() || active == 0 || (p && p->cancelled()); });
                if (queue.empty() || (p && p->cancelled())) {
                    cv.notify_all();
                    return;
                }
                task = std::move(queue.back());  // newest first keeps the queue small
                queue.pop_back();
                ++active;
            }
            found.clear();
            visit(task, worker, found);
            {
                std::lock_guard<std::mutex> lock(m);
                for (auto& f : found) queue.push_back(std::move(f));
                --active;
            }
            cv.notify_all();
        }
    };
    std::vector<std::thread> threads;
    for (int i = 1; i < workerCount(); ++i) threads.emplace_back(work, i);
    work(0);
    for (auto& t : threads) t.join();
}

void parallelFor(size_t n, const std::function<void(size_t, int)>& fn) {
    std::atomic<size_t> next{0};
    auto work = [&](int worker) {
        for (size_t i; (i = next++) < n;) fn(i, worker);
    };
    std::vector<std::thread> threads;
    int count = int(std::min<size_t>(size_t(workerCount()), n));
    for (int i = 1; i < count; ++i) threads.emplace_back(work, i);
    work(0);
    for (auto& t : threads) t.join();
}

void measure(const std::wstring& dir, int64_t olderThan, Stats& st, Progress* p) {
    std::vector<Stats> part(static_cast<size_t>(workerCount()));
    parallelWalk({{dir}}, p, [&](const WalkTask& t, int w, std::vector<WalkTask>& sub) {
        if (p) p->at(t.dir);
        Stats& s = part[size_t(w)];
        for (auto& fd : list(t.dir)) {
            DWORD a = fd.dwFileAttributes;
            if (a & FILE_ATTRIBUTE_DIRECTORY) {
                if (!isLink(a)) sub.push_back({join(t.dir, fd.cFileName)});
                continue;
            }
            int64_t time = toUnix(fd.ftLastWriteTime);
            if (olderThan && time >= olderThan) continue;
            s.files++;
            if (p) p->files++;
            s.newest = std::max(s.newest, time);
            if (!cloudOnly(a) && !isLink(a)) s.bytes += fileSize(fd);
        }
    });
    for (auto& s : part) {
        st.bytes += s.bytes;
        st.files += s.files;
        st.newest = std::max(st.newest, s.newest);
    }
}

static void clearReadOnly(const std::wstring& lp, DWORD a) {
    if (a & FILE_ATTRIBUTE_READONLY) {
        DWORD b = a & ~DWORD(FILE_ATTRIBUTE_READONLY);
        SetFileAttributesW(lp.c_str(), b ? b : FILE_ATTRIBUTE_NORMAL);
    }
}

static void removeFile(const std::wstring& path, const WIN32_FIND_DATAW& fd, DeleteStats& st, Progress* p) {
    std::wstring lp = longPath(path);
    clearReadOnly(lp, fd.dwFileAttributes);
    if (DeleteFileW(lp.c_str())) {
        st.deleted++;
        if (p) p->files++;
        if (!cloudOnly(fd.dwFileAttributes) && !isLink(fd.dwFileAttributes)) st.freed += fileSize(fd);
    } else {
        st.skipped++;  // usually open in another app
    }
}

static void removeDir(const std::wstring& path, DWORD a) {
    std::wstring lp = longPath(path);
    clearReadOnly(lp, a);
    RemoveDirectoryW(lp.c_str());  // fails quietly while something is still inside
}

void deleteContents(const std::wstring& dir, int64_t olderThan, DeleteStats& st, Progress* p) {
    if (p) {
        if (p->cancelled()) return;
        p->at(dir);
    }
    // Read the whole listing first; deleting while enumerating is asking for trouble.
    for (auto& fd : list(dir)) {
        std::wstring path = join(dir, fd.cFileName);
        DWORD a = fd.dwFileAttributes;
        bool old = !olderThan || toUnix(fd.ftLastWriteTime) < olderThan;
        if (a & FILE_ATTRIBUTE_DIRECTORY) {
            if (isLink(a)) {
                // A junction or folder symlink. RemoveDirectory takes away the link only.
                if (old) {
                    if (RemoveDirectoryW(longPath(path).c_str())) st.deleted++;
                    else st.skipped++;
                }
                continue;
            }
            deleteContents(path, olderThan, st, p);
            if (old) removeDir(path, a);
        } else if (old) {
            removeFile(path, fd, st, p);
        }
        if (p && p->cancelled()) return;
    }
}

void deleteTree(const std::wstring& path, DeleteStats& st, Progress* p) {
    WIN32_FIND_DATAW fd;
    if (!findOne(path, fd)) return;
    DWORD a = fd.dwFileAttributes;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        removeFile(path, fd, st, p);
    } else if (isLink(a)) {
        if (RemoveDirectoryW(longPath(path).c_str())) st.deleted++;
        else st.skipped++;
    } else {
        deleteContents(path, 0, st, p);
        removeDir(path, a);
    }
}

}  // namespace roomy::fs
