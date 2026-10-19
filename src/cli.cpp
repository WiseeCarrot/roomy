#include "cli.h"

#include "clean.h"
#include "format.h"
#include "scan.h"

#include <cstdio>

namespace roomy {
namespace {

HANDLE g_out = nullptr;

// Roomy is a GUI app, so it has no console of its own. Borrow the one it was started from.
void openOutput() {
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (g_out && g_out != INVALID_HANDLE_VALUE && GetFileType(g_out) != FILE_TYPE_UNKNOWN) return;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        g_out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        SetConsoleOutputCP(CP_UTF8);
    }
}

void out(const std::string& s) {
    if (!g_out || g_out == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(g_out, s.data(), DWORD(s.size()), &w, nullptr);
}

void out(const std::wstring& s) { out(utf8(s)); }

const char* safetyId(Safety s) { return s == Safety::Safe ? "safe" : s == Safety::Rebuilds ? "rebuilds" : "check-first"; }
const char* actionId(Action a) {
    return a == Action::DeleteTree ? "delete" : a == Action::DeleteContents ? "empty" : "empty-recycle-bin";
}

void printJson(const std::vector<Category>& cats, const std::vector<std::wstring>& empty) {
    std::string s = "{\n  \"categories\": [";
    for (size_t i = 0; i < cats.size(); ++i) {
        auto& c = cats[i];
        s += i ? ",\n    {" : "\n    {";
        s += "\"id\": \"" + c.id + "\", \"name\": " + jsonString(c.name) + ", \"safety\": \"" + safetyId(c.safety) +
             "\", \"bytes\": " + std::to_string(c.bytes()) + ", \"items\": [";
        for (size_t j = 0; j < c.items.size(); ++j) {
            auto& it = c.items[j];
            s += j ? ",\n      {" : "\n      {";
            s += "\"label\": " + jsonString(it.label) + ", \"kind\": " + jsonString(it.kind) +
                 ", \"bytes\": " + std::to_string(it.bytes) + ", \"files\": " + std::to_string(it.files) +
                 ", \"action\": \"" + actionId(it.action) + "\", \"recycle\": " + (it.recycle ? "true" : "false") +
                 ", \"selected\": " + (it.selected ? "true" : "false") + ", \"paths\": [";
            for (size_t k = 0; k < it.paths.size(); ++k) s += (k ? ", " : "") + jsonString(it.paths[k]);
            s += "]}";
        }
        s += c.items.empty() ? "]}" : "\n    ]}";
    }
    s += "\n  ],\n  \"empty\": [";
    for (size_t i = 0; i < empty.size(); ++i) s += (i ? ", " : "") + jsonString(empty[i]);
    s += "]\n}\n";
    out(s);
}

void printText(const std::vector<Category>& cats) {
    uint64_t total = 0;
    for (auto& c : cats) {
        total += c.bytes();
        const wchar_t* safety = c.safety == Safety::Safe ? L"safe" : c.safety == Safety::Rebuilds ? L"rebuilds itself" : L"check first";
        out(L"\n" + c.name + L"  " + sizeText(c.bytes()) + L"  (" + safety + L")\n");
        for (auto& it : c.items) {
            std::wstring where = !it.line2.empty() ? it.line2 : it.paths.empty() ? L"" : it.paths[0];
            out(std::wstring(L"  ") + (it.selected ? L"[x] " : L"[ ] ") + sizeText(it.bytes) + L"  " + it.label +
                (it.kind.empty() ? L"" : L" (" + it.kind + L")") + L"  " + where + L"\n");
        }
    }
    out(L"\nTotal: " + sizeText(total) + L". Ticked items are what --clean removes.\n");
}

const char* kHelp =
    "Roomy, a disk cleaner that tells you what is safe to delete.\n"
    "\n"
    "  roomy                     open the app\n"
    "  roomy --scan              list what can be cleaned\n"
    "  roomy --scan --json       the same, as JSON\n"
    "  roomy --clean --yes       remove everything that is ticked by default\n"
    "\n"
    "Options:\n"
    "  --all             with --clean: remove every item found, not just the default picks\n"
    "  --no-duplicates   skip the duplicate file search (the slowest check)\n"
    "  --no-recycle      delete for good instead of using the Recycle Bin\n"
    "  --home <folder>   pretend <folder> is your whole PC (for testing)\n";

}  // namespace

int runCli(const std::vector<std::wstring>& args) {
    openOutput();
    bool scan = false, doClean = false, json = false, yes = false, all = false, recycle = true, dupes = true, timing = false;
    std::wstring home;
    for (size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        if (a == L"--scan") scan = true;
        else if (a == L"--clean") doClean = true;
        else if (a == L"--json") json = true;
        else if (a == L"--yes") yes = true;
        else if (a == L"--all") all = true;
        else if (a == L"--no-recycle") recycle = false;
        else if (a == L"--no-duplicates") dupes = false;
        else if (a == L"--timing") timing = true;
        else if (a == L"--home" && i + 1 < args.size()) home = args[++i];
        else if (a == L"--version") {
            out("Roomy 1.0.0\n");
            return 0;
        } else {
            out(kHelp);
            return a == L"--help" || a == L"-h" ? 0 : 2;
        }
    }
    if (doClean && !yes) {
        out("--clean deletes files. Add --yes to confirm.\n");
        return 2;
    }

    Env env;
    if (!home.empty()) {
        wchar_t full[MAX_PATH];
        GetFullPathNameW(home.c_str(), MAX_PATH, full, nullptr);
        env = sandboxEnv(full);
    } else {
        env = detectEnv();
    }
    env.duplicates = dupes;

    Progress progress;
    std::vector<Category> cats;
    std::vector<std::wstring> empty;
    ULONGLONG last = GetTickCount64();
    scanAll(env, progress, [&](Category&& c) {
        if (timing) {
            ULONGLONG t = GetTickCount64();
            fprintf(stderr, "%6.1f s  %s\n", double(t - last) / 1000.0, c.id.c_str());
            last = t;
        }
        if (c.bytes() > 0) cats.push_back(std::move(c));
        else empty.push_back(c.name);
    });

    if (!doClean) {
        if (json) printJson(cats, empty);
        else printText(cats);
        return 0;
    }

    std::vector<Item> chosen;
    for (auto& c : cats)
        for (auto& it : c.items)
            if (all || it.selected) chosen.push_back(it);
    CleanResult r = clean(chosen, env, progress, recycle);
    if (json) {
        out("{\"freed\": " + std::to_string(r.freed) + ", \"recycled\": " + std::to_string(r.recycled) +
            ", \"deleted\": " + std::to_string(r.deleted) + ", \"skipped\": " + std::to_string(r.skipped) + "}\n");
    } else {
        out(L"Freed " + sizeText(r.freed) + L", moved " + sizeText(r.recycled) + L" to the Recycle Bin, skipped " +
            countText(r.skipped) + L" files that were in use.\n");
    }
    return 0;
}

}  // namespace roomy
