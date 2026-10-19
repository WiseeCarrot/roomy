#pragma once

#include "model.h"

namespace roomy {

struct CleanResult {
    uint64_t freed = 0;     // bytes actually gone from the disk
    uint64_t recycled = 0;  // bytes moved to the Recycle Bin
    uint64_t deleted = 0;   // files and links removed
    uint64_t skipped = 0;   // files that were open somewhere else, or not ours to touch
};

CleanResult clean(const std::vector<Item>& items, const Env& env, Progress& p, bool useRecycleBin = true);

// True for folders that must never be deleted as a whole: drive roots, Windows, your profile and so on.
bool isProtected(const std::wstring& path, const Env& env);

}  // namespace roomy
