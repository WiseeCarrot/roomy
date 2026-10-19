#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace roomy {

// How sure we are that deleting something is harmless.
enum class Safety {
    Safe,        // nothing anyone would miss
    Rebuilds,    // apps or tools recreate it on their own, maybe a little slower the first time
    CheckFirst,  // probably junk, but only you can tell
};

enum class Action {
    DeleteTree,       // remove the file or folder itself
    DeleteContents,   // empty the folder but keep it
    EmptyRecycleBin,
};

struct Item {
    std::vector<std::wstring> paths;
    std::wstring label;      // first line in the list
    std::wstring kind;       // short note shown after the label
    std::wstring line2;      // second line; the first path when empty
    uint64_t bytes = 0;
    uint64_t files = 0;
    int64_t newest = 0;      // unix time of the newest file inside, 0 if unknown
    int64_t olderThan = 0;   // only touch files written before this unix time, 0 means everything
    Action action = Action::DeleteContents;
    bool recycle = false;    // send to the Recycle Bin instead of deleting for good
    bool selected = false;
    bool showAge = false;
};

struct Category {
    std::string id;
    std::wstring name;
    std::wstring blurb;
    wchar_t glyph = 0;
    uint32_t tint = 0;       // 0xRRGGBB
    Safety safety = Safety::Safe;
    std::vector<Item> items;
    bool expanded = false;   // UI state only

    uint64_t bytes() const {
        uint64_t n = 0;
        for (auto& it : items) n += it.bytes;
        return n;
    }
    uint64_t selectedBytes() const {
        uint64_t n = 0;
        for (auto& it : items)
            if (it.selected) n += it.bytes;
        return n;
    }
    // 0 = nothing ticked, 1 = some, 2 = all
    int selectionState() const {
        size_t on = 0;
        for (auto& it : items) on += it.selected;
        return on == 0 ? 0 : on == items.size() ? 2 : 1;
    }
};

// Every location Roomy looks at. Tests point all of it at one folder with --home.
struct Env {
    std::wstring home, local, localLow, roaming, temp, windir, programData, downloads;
    std::vector<std::wstring> projectRoots;   // where build folders are looked for
    std::vector<std::wstring> personalRoots;  // where duplicates are looked for
    bool admin = false;
    bool sandbox = false;
    bool duplicates = true;
};

// Shared between a worker thread and the window.
class Progress {
public:
    std::atomic<bool> cancel{false};
    std::atomic<uint64_t> files{0};

    bool cancelled() const { return cancel.load(std::memory_order_relaxed); }

    void stage(const std::wstring& s) {
        std::lock_guard<std::mutex> lock(m_);
        stage_ = s;
        path_.clear();
    }
    void at(const std::wstring& path) {
        if ((tick_++ & 31) != 0) return;  // the window only repaints so often
        std::lock_guard<std::mutex> lock(m_);
        path_ = path;
    }
    std::wstring stageName() {
        std::lock_guard<std::mutex> lock(m_);
        return stage_;
    }
    std::wstring path() {
        std::lock_guard<std::mutex> lock(m_);
        return path_;
    }
    void reset() {
        cancel = false;
        files = 0;
        tick_ = 0;
        std::lock_guard<std::mutex> lock(m_);
        stage_.clear();
        path_.clear();
    }

private:
    std::mutex m_;
    std::wstring stage_, path_;
    std::atomic<uint32_t> tick_{0};
};

}  // namespace roomy
