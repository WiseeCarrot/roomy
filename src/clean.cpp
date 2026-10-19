#include "clean.h"

#include "fs.h"

#include <shellapi.h>
#include <shlobj.h>

namespace roomy {

bool isProtected(const std::wstring& path, const Env& e) {
    if (path.size() <= 3 || path[1] != L':') return true;  // a drive root, or not a full path at all
    std::vector<std::wstring> roots = {e.home,      e.local,       e.localLow, e.roaming, e.temp,
                                       e.windir,    e.programData, e.downloads};
    for (auto& r : e.personalRoots) roots.push_back(r);
    for (auto& r : e.projectRoots) roots.push_back(r);
    for (auto id : {FOLDERID_ProgramFiles, FOLDERID_ProgramFilesX86, FOLDERID_Windows, FOLDERID_System}) {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &p))) roots.push_back(p);
        CoTaskMemFree(p);
    }
    std::wstring trimmed = path;
    while (trimmed.size() > 3 && trimmed.back() == L'\\') trimmed.pop_back();
    for (auto& r : roots) {
        if (r.empty()) continue;
        // The root itself, or anything above it (deleting C:\Users would take the profile with it).
        if (fs::startsWith(r, trimmed)) return true;
    }
    return false;
}

static bool sendToRecycleBin(const std::wstring& path) {
    std::wstring from = path;
    from.push_back(L'\0');  // SHFileOperation wants a double-null-terminated list
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted && !fs::isFile(path) && !fs::isDir(path);
}

CleanResult clean(const std::vector<Item>& items, const Env& env, Progress& p, bool useRecycleBin) {
    CleanResult r;
    fs::DeleteStats st;
    for (auto& it : items) {
        if (p.cancelled()) break;
        switch (it.action) {
            case Action::EmptyRecycleBin: {
                if (env.sandbox) break;
                SHQUERYRBINFO before{};
                before.cbSize = sizeof before;
                SHQueryRecycleBinW(nullptr, &before);
                if (SUCCEEDED(SHEmptyRecycleBinW(nullptr, nullptr,
                                                 SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND))) {
                    r.freed += uint64_t(before.i64Size);
                    r.deleted += uint64_t(before.i64NumItems);
                }
                break;
            }
            case Action::DeleteContents:
                // Emptying is fine even for a protected folder like %TEMP%: the folder itself stays.
                for (auto& path : it.paths) {
                    if (path.size() <= 3) continue;
                    fs::deleteContents(path, it.olderThan, st, &p);
                }
                break;
            case Action::DeleteTree:
                for (auto& path : it.paths) {
                    if (isProtected(path, env)) {
                        r.skipped++;
                        continue;
                    }
                    if (it.recycle && useRecycleBin) {
                        p.at(path);
                        uint64_t size = 0;
                        if (fs::isDir(path)) {
                            fs::Stats s;
                            fs::measure(path, 0, s, nullptr);
                            size = s.bytes;
                        } else {
                            WIN32_FIND_DATAW fd;
                            if (fs::findOne(path, fd)) size = fs::fileSize(fd);
                        }
                        if (sendToRecycleBin(path)) {
                            r.recycled += size;
                            r.deleted++;
                            p.files++;
                        } else {
                            r.skipped++;
                        }
                    } else {
                        fs::deleteTree(path, st, &p);
                    }
                }
                break;
        }
    }
    r.freed += st.freed;
    r.deleted += st.deleted;
    r.skipped += st.skipped;
    return r;
}

}  // namespace roomy
