#include "scan.h"

#include "format.h"
#include "fs.h"

#include <bcrypt.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <unordered_map>

namespace roomy {
namespace {

constexpr int64_t kDay = 86400;

std::wstring known(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &p))) s = p;
    CoTaskMemFree(p);
    return s;
}

bool elevated() {
    HANDLE t;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return false;
    TOKEN_ELEVATION e{};
    DWORD n = 0;
    bool r = GetTokenInformation(t, TokenElevation, &e, sizeof e, &n) && e.TokenIsElevated;
    CloseHandle(t);
    return r;
}

std::vector<std::wstring> fixedDrives() {
    std::vector<std::wstring> out;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        std::wstring root = std::wstring(1, wchar_t(L'A' + i)) + L":\\";
        if (GetDriveTypeW(root.c_str()) == DRIVE_FIXED) out.push_back(root);
    }
    return out;
}

void addUnique(std::vector<std::wstring>& v, const std::wstring& p) {
    if (p.empty()) return;
    for (auto& q : v)
        if (_wcsicmp(q.c_str(), p.c_str()) == 0) return;
    v.push_back(p);
}

Category make(const char* id, const wchar_t* name, const wchar_t* blurb, wchar_t glyph, uint32_t tint, Safety s) {
    Category c;
    c.id = id;
    c.name = name;
    c.blurb = blurb;
    c.glyph = glyph;
    c.tint = tint;
    c.safety = s;
    return c;
}

void sortBySize(Category& c) {
    std::stable_sort(c.items.begin(), c.items.end(), [](const Item& a, const Item& b) { return a.bytes > b.bytes; });
}

// Measures one or more folders and adds them as a single row. Nothing is added when they're empty.
bool addFolders(Category& c, const std::vector<std::wstring>& paths, const std::wstring& label, const std::wstring& kind,
                bool selected, Progress& p, int64_t olderThan = 0) {
    Item it;
    it.label = label;
    it.kind = kind;
    it.olderThan = olderThan;
    it.selected = selected;
    it.action = Action::DeleteContents;
    for (auto& path : paths) {
        if (!fs::isDir(path)) continue;
        fs::Stats st;
        fs::measure(path, olderThan, st, &p);
        if (st.bytes == 0) continue;
        it.paths.push_back(path);
        it.bytes += st.bytes;
        it.files += st.files;
        it.newest = std::max(it.newest, st.newest);
    }
    if (it.bytes == 0) return false;
    if (it.paths.size() > 1) it.line2 = fs::commonFolder(it.paths);
    c.items.push_back(std::move(it));
    return true;
}

void addFile(Category& c, const std::wstring& path, const std::wstring& label, bool selected) {
    WIN32_FIND_DATAW fd;
    if (!fs::findOne(path, fd) || (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return;
    Item it;
    it.paths = {path};
    it.label = label;
    it.bytes = fs::fileSize(fd);
    it.files = 1;
    it.newest = fs::toUnix(fd.ftLastWriteTime);
    it.action = Action::DeleteTree;
    it.selected = selected;
    it.showAge = true;
    if (it.bytes) c.items.push_back(std::move(it));
}

// ---------------------------------------------------------------- temp and system

Category scanTemp(const Env& e, Progress& p) {
    auto c = make("temp", L"Temporary files",
                  L"Leftovers apps were supposed to clean up. Only files older than a day are removed.", 0xE81C,
                  0x6E8BFF, Safety::Safe);
    int64_t cutoff = fs::now() - kDay;
    addFolders(c, {e.temp}, L"Your temp folder", L"", true, p, cutoff);
    if (e.admin) addFolders(c, {fs::join(e.windir, L"Temp")}, L"Windows temp folder", L"", true, p, cutoff);
    return c;
}

Category scanRecycleBin(const Env& e, Progress&) {
    auto c = make("recycle", L"Recycle Bin", L"Files you already deleted. Emptying the bin can't be undone.", 0xE74D,
                  0x8E8E93, Safety::CheckFirst);
    if (e.sandbox) return c;
    SHQUERYRBINFO info{};
    info.cbSize = sizeof info;
    if (SUCCEEDED(SHQueryRecycleBinW(nullptr, &info)) && info.i64Size > 0) {
        Item it;
        it.label = L"Recycle Bin";
        it.line2 = countText(uint64_t(info.i64NumItems)) + (info.i64NumItems == 1 ? L" item" : L" items") +
                   L" on all drives";
        it.bytes = uint64_t(info.i64Size);
        it.files = uint64_t(info.i64NumItems);
        it.action = Action::EmptyRecycleBin;
        c.items.push_back(std::move(it));
    }
    return c;
}

Category scanCrashes(const Env& e, Progress& p) {
    auto c = make("crash", L"Crash reports",
                  L"Memory dumps and error reports saved when an app or Windows crashed. Nobody reads them.", 0xE9D9,
                  0xEF5A4C, Safety::Safe);
    addFolders(c, {fs::join(e.local, L"CrashDumps")}, L"App crash dumps", L"", true, p);
    std::wstring wer = fs::join(e.local, L"Microsoft\\Windows\\WER");
    addFolders(c, {fs::join(wer, L"ReportArchive"), fs::join(wer, L"ReportQueue"), fs::join(wer, L"Temp")},
               L"Error reports", L"your account", true, p);
    if (e.admin) {
        std::wstring swer = fs::join(e.programData, L"Microsoft\\Windows\\WER");
        addFolders(c, {fs::join(swer, L"ReportArchive"), fs::join(swer, L"ReportQueue"), fs::join(swer, L"Temp")},
                   L"Error reports", L"system", true, p);
        addFolders(c, {fs::join(e.windir, L"Minidump"), fs::join(e.windir, L"LiveKernelReports")},
                   L"System crash dumps", L"", true, p);
        addFile(c, fs::join(e.windir, L"MEMORY.DMP"), L"Full memory dump", true);
    }
    sortBySize(c);
    return c;
}

Category scanWindowsUpdate(const Env& e, Progress& p) {
    auto c = make("update", L"Windows Update leftovers",
                  L"Update files Windows has already installed. Only files older than 10 days are removed.", 0xE895,
                  0x33A7D8, Safety::Safe);
    if (!e.admin) return c;
    addFolders(c, {fs::join(e.windir, L"SoftwareDistribution\\Download")}, L"Downloaded updates", L"", true, p,
               fs::now() - 10 * kDay);
    addFolders(c,
               {fs::join(e.windir,
                         L"ServiceProfiles\\NetworkService\\AppData\\Local\\Microsoft\\Windows\\DeliveryOptimization\\Cache")},
               L"Delivery Optimization", L"update pieces shared between PCs", true, p);
    return c;
}

// ---------------------------------------------------------------- browsers and apps

const wchar_t* kChromiumCaches[] = {L"Cache",           L"Code Cache",     L"GPUCache",
                                    L"DawnCache",       L"DawnGraphiteCache", L"DawnWebGPUCache",
                                    L"GrShaderCache",   L"GraphiteDawnCache", L"ShaderCache"};

void chromiumProfileCaches(const std::wstring& profile, std::vector<std::wstring>& out) {
    for (auto n : kChromiumCaches) {
        std::wstring p = fs::join(profile, n);
        if (fs::isDir(p)) out.push_back(p);
    }
}

// A Chromium "User Data" folder: shared shader caches at the top, then one folder per profile.
std::vector<std::wstring> chromiumCaches(const std::wstring& userData, int& profiles) {
    std::vector<std::wstring> out;
    chromiumProfileCaches(userData, out);
    for (auto& sub : fs::subdirs(userData)) {
        if (!fs::isFile(fs::join(sub, L"Preferences"))) continue;
        ++profiles;
        chromiumProfileCaches(sub, out);
    }
    return out;
}

std::vector<std::wstring> geckoCaches(const std::wstring& profilesDir, int& profiles) {
    std::vector<std::wstring> out;
    for (auto& sub : fs::subdirs(profilesDir)) {
        bool any = false;
        for (auto n : {L"cache2", L"startupCache", L"jumpListCache"}) {
            std::wstring p = fs::join(sub, n);
            if (fs::isDir(p)) {
                out.push_back(p);
                any = true;
            }
        }
        profiles += any;
    }
    return out;
}

std::wstring profilesNote(int n) { return n > 1 ? std::to_wstring(n) + L" profiles" : L""; }

Category scanBrowsers(const Env& e, Progress& p) {
    auto c = make("browsers", L"Browser caches",
                  L"Saved copies of websites so they load faster. Sites are a little slower the first time after.",
                  0xE774, 0x3D9BFF, Safety::Rebuilds);
    struct Chromium {
        const wchar_t* name;
        std::wstring dir;
        bool single;  // the folder is the profile itself
    };
    const Chromium chromium[] = {
        {L"Google Chrome", fs::join(e.local, L"Google\\Chrome\\User Data"), false},
        {L"Microsoft Edge", fs::join(e.local, L"Microsoft\\Edge\\User Data"), false},
        {L"Brave", fs::join(e.local, L"BraveSoftware\\Brave-Browser\\User Data"), false},
        {L"Vivaldi", fs::join(e.local, L"Vivaldi\\User Data"), false},
        {L"Chromium", fs::join(e.local, L"Chromium\\User Data"), false},
        {L"Opera", fs::join(e.local, L"Opera Software\\Opera Stable"), true},
        {L"Opera GX", fs::join(e.local, L"Opera Software\\Opera GX Stable"), true},
    };
    for (auto& b : chromium) {
        if (!fs::isDir(b.dir)) continue;
        int profiles = 0;
        std::vector<std::wstring> paths;
        if (b.single) {
            chromiumProfileCaches(b.dir, paths);
            profiles = 1;
        } else {
            paths = chromiumCaches(b.dir, profiles);
        }
        addFolders(c, paths, b.name, profilesNote(profiles), true, p);
    }
    const std::pair<const wchar_t*, std::wstring> gecko[] = {
        {L"Firefox", fs::join(e.local, L"Mozilla\\Firefox\\Profiles")},
        {L"Zen", fs::join(e.local, L"zen\\Profiles")},
        {L"LibreWolf", fs::join(e.local, L"librewolf\\Profiles")},
        {L"Waterfox", fs::join(e.local, L"Waterfox\\Profiles")},
        {L"Floorp", fs::join(e.local, L"Floorp\\Profiles")},
    };
    for (auto& [name, dir] : gecko) {
        if (!fs::isDir(dir)) continue;
        int profiles = 0;
        auto paths = geckoCaches(dir, profiles);
        addFolders(c, paths, name, profilesNote(profiles), true, p);
    }
    sortBySize(c);
    return c;
}

std::wstring prettyAppName(const std::wstring& folder) {
    static const std::map<std::wstring, std::wstring> names = {
        {L"discord", L"Discord"},       {L"discordptb", L"Discord PTB"},   {L"discordcanary", L"Discord Canary"},
        {L"code", L"Visual Studio Code"}, {L"code - insiders", L"VS Code Insiders"}, {L"cursor", L"Cursor"},
        {L"slack", L"Slack"},           {L"obsidian", L"Obsidian"},        {L"notion", L"Notion"},
        {L"claude", L"Claude"},         {L"postman", L"Postman"},          {L"figma", L"Figma"},
        {L"spotify", L"Spotify"},       {L"whatsapp", L"WhatsApp"},        {L"signal", L"Signal"},
        {L"github desktop", L"GitHub Desktop"}, {L"zoom", L"Zoom"},        {L"windsurf", L"Windsurf"},
    };
    auto it = names.find(fs::lower(folder));
    if (it != names.end()) return it->second;
    std::wstring s = folder;
    if (!s.empty()) s[0] = wchar_t(towupper(s[0]));
    return s;
}

Category scanApps(const Env& e, Progress& p) {
    auto c = make("apps", L"App caches",
                  L"Cached images and code from apps like Discord, Spotify and VS Code. They rebuild as you use them.",
                  0xE71D, 0xA371F7, Safety::Rebuilds);
    // Folders that belong to browsers (handled above) or hold more than one app.
    static const std::set<std::wstring> skip = {L"google",   L"microsoft", L"bravesoftware", L"opera software",
                                                L"vivaldi",  L"mozilla",   L"zen",           L"librewolf",
                                                L"chromium", L"packages",  L"programs",      L"temp"};
    for (auto& root : {e.roaming, e.local}) {
        for (auto& dir : fs::subdirs(root)) {
            std::wstring folder = fs::name(dir);
            if (skip.count(fs::lower(folder))) continue;
            // Electron and CEF apps keep a Chromium profile at the top of their folder.
            if (!fs::isFile(fs::join(dir, L"Local State")) && !fs::isFile(fs::join(dir, L"Preferences"))) continue;
            std::vector<std::wstring> paths;
            chromiumProfileCaches(dir, paths);
            for (auto n : {L"CachedData", L"CachedExtensionVSIXs"}) {  // VS Code and its forks
                std::wstring q = fs::join(dir, n);
                if (fs::isDir(q)) paths.push_back(q);
            }
            if (!paths.empty()) addFolders(c, paths, prettyAppName(folder), L"", true, p);
        }
    }
    addFolders(c, {fs::join(e.local, L"Spotify\\Data")}, L"Spotify", L"streamed songs", true, p);
    addFolders(c, {fs::join(e.local, L"Steam\\htmlcache")}, L"Steam", L"store and overlay pages", true, p);
    {
        std::vector<std::wstring> epic;
        for (auto& sub : fs::subdirs(fs::join(e.local, L"EpicGamesLauncher\\Saved")))
            if (fs::lower(fs::name(sub)).rfind(L"webcache", 0) == 0) epic.push_back(sub);
        addFolders(c, epic, L"Epic Games Launcher", L"", true, p);
    }
    {
        int profiles = 0;
        auto teams = chromiumCaches(
            fs::join(e.local, L"Packages\\MSTeams_8wekyb3d8bbwe\\LocalCache\\Microsoft\\MSTeams\\EBWebView"), profiles);
        addFolders(c, teams, L"Microsoft Teams", L"", true, p);
    }
    sortBySize(c);
    return c;
}

// ---------------------------------------------------------------- developer stuff

Category scanPackageCaches(const Env& e, Progress& p) {
    auto c = make("packages", L"Package manager caches",
                  L"Packages downloaded by npm, pip, Cargo and friends. They download again when a project needs them.",
                  0xE7B8, 0x2BB3A6, Safety::Rebuilds);
    std::wstring cargo = fs::join(e.home, L".cargo");
    struct Cache {
        const wchar_t* label;
        std::vector<std::wstring> paths;
    };
    const Cache caches[] = {
        {L"npm", {fs::join(e.local, L"npm-cache")}},
        {L"Yarn", {fs::join(e.local, L"Yarn\\Cache")}},
        {L"pip", {fs::join(e.local, L"pip\\cache")}},
        {L"uv", {fs::join(e.local, L"uv\\cache")}},
        {L"Cargo", {fs::join(cargo, L"registry\\cache"), fs::join(cargo, L"registry\\src"), fs::join(cargo, L"git\\checkouts")}},
        {L"Go build cache", {fs::join(e.local, L"go-build")}},
        {L"NuGet", {fs::join(e.local, L"NuGet\\v3-cache"), fs::join(e.local, L"NuGet\\plugins-cache")}},
        {L"Gradle", {fs::join(e.home, L".gradle\\caches")}},
        {L"Maven", {fs::join(e.home, L".m2\\repository")}},
        {L"Bun", {fs::join(e.home, L".bun\\install\\cache")}},
        {L"Electron downloads", {fs::join(e.local, L"electron\\Cache"), fs::join(e.local, L"electron-builder\\Cache")}},
        {L"node-gyp", {fs::join(e.local, L"node-gyp\\Cache")}},
        {L"Scoop downloads", {fs::join(e.home, L"scoop\\cache")}},
    };
    for (auto& k : caches) addFolders(c, k.paths, k.label, L"", true, p);
    sortBySize(c);
    return c;
}

std::vector<std::wstring> steamLibraries() {
    wchar_t buf[MAX_PATH];
    DWORD sz = sizeof buf;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, buf, &sz) !=
        ERROR_SUCCESS)
        return {};
    std::wstring steam = buf;
    std::replace(steam.begin(), steam.end(), L'/', L'\\');
    std::vector<std::wstring> libs = {steam};
    // libraryfolders.vdf has lines like:  "path"  "D:\\SteamLibrary"
    std::string vdf = fs::readSmall(fs::join(steam, L"steamapps\\libraryfolders.vdf"), 1 << 16);
    size_t pos = 0;
    while ((pos = vdf.find("\"path\"", pos)) != std::string::npos) {
        pos += 6;
        size_t a = vdf.find('"', pos), b = a == std::string::npos ? a : vdf.find('"', a + 1);
        if (b == std::string::npos) break;
        std::string v = vdf.substr(a + 1, b - a - 1);
        std::string clean;
        for (size_t i = 0; i < v.size(); ++i) {
            if (v[i] == '\\' && i + 1 < v.size() && v[i + 1] == '\\') ++i;
            clean += v[i];
        }
        addUnique(libs, wide(clean));
        pos = b + 1;
    }
    return libs;
}

Category scanShaders(const Env& e, Progress& p) {
    auto c = make("shaders", L"Shader caches",
                  L"Compiled graphics shaders. Games can stutter for a few minutes while they rebuild, so these start unticked.",
                  0xE7FC, 0xE85D9C, Safety::Rebuilds);
    addFolders(c, {fs::join(e.local, L"D3DSCache")}, L"DirectX shader cache", L"", false, p);
    addFolders(c,
               {fs::join(e.local, L"NVIDIA\\DXCache"), fs::join(e.local, L"NVIDIA\\GLCache"),
                fs::join(e.localLow, L"NVIDIA\\PerDriverVersion\\DXCache"),
                fs::join(e.programData, L"NVIDIA Corporation\\NV_Cache")},
               L"NVIDIA", L"", false, p);
    addFolders(c,
               {fs::join(e.local, L"AMD\\DxCache"), fs::join(e.local, L"AMD\\DxcCache"),
                fs::join(e.local, L"AMD\\GLCache"), fs::join(e.local, L"AMD\\VkCache")},
               L"AMD", L"", false, p);
    addFolders(c, {fs::join(e.local, L"Intel\\ShaderCache"), fs::join(e.localLow, L"Intel\\ShaderCache")}, L"Intel",
               L"", false, p);
    if (!e.sandbox) {
        std::vector<std::wstring> steam;
        for (auto& lib : steamLibraries()) steam.push_back(fs::join(lib, L"steamapps\\shadercache"));
        addFolders(c, steam, L"Steam shader pre-caching", L"", false, p);
    }
    sortBySize(c);
    return c;
}

struct Markers {
    bool pkg = false, lock = false, cargo = false, cmakeLists = false, gradle = false, pom = false, zig = false,
         pubspec = false, mix = false, stack = false, cabal = false, swift = false, godot = false, dotnet = false,
         venv = false;
};

Markers readMarkers(const std::vector<WIN32_FIND_DATAW>& entries) {
    Markers m;
    for (auto& fd : entries) {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring n = fs::lower(fd.cFileName), ext = fs::extension(n);
        if (n == L"package.json") m.pkg = true;
        else if (n == L"package-lock.json" || n == L"yarn.lock" || n == L"pnpm-lock.yaml" || n == L"bun.lockb" ||
                 n == L"bun.lock" || n == L"npm-shrinkwrap.json")
            m.lock = true;
        else if (n == L"cargo.toml") m.cargo = true;
        else if (n == L"cmakelists.txt") m.cmakeLists = true;
        else if (n == L"build.gradle" || n == L"build.gradle.kts" || n == L"settings.gradle" || n == L"settings.gradle.kts")
            m.gradle = true;
        else if (n == L"pom.xml") m.pom = true;
        else if (n == L"build.zig") m.zig = true;
        else if (n == L"pubspec.yaml") m.pubspec = true;
        else if (n == L"mix.exs") m.mix = true;
        else if (n == L"stack.yaml") m.stack = true;
        else if (ext == L".cabal") m.cabal = true;
        else if (n == L"package.swift") m.swift = true;
        else if (n == L"project.godot") m.godot = true;
        else if (ext == L".csproj" || ext == L".fsproj" || ext == L".vbproj") m.dotnet = true;
        else if (n == L"pyvenv.cfg") m.venv = true;
    }
    return m;
}

// See https://bford.info/cachedir/ : a folder holding this tag says "everything in here can be regenerated".
const char* kCacheDirSignature = "Signature: 8a477f597d28d172789f06886806bc55";

constexpr unsigned kDriveRoot = 1, kHomeRoot = 2;

// "MyApp\src-tauri" and "MyApp\android\app" should both read as "MyApp".
std::wstring projectName(const std::wstring& dir) {
    static const std::set<std::wstring> generic = {
        L"src-tauri", L"app", L"apps", L"src", L"gen", L"android", L"ios", L"packages", L"frontend", L"backend",
        L"client", L"server", L"web", L"desktop", L"native", L"core", L"lib", L"crates", L"ui", L"www"};
    std::wstring d = dir;
    for (int i = 0; i < 6; ++i) {
        std::wstring n = fs::name(d), up = fs::parent(d);
        if (!generic.count(fs::lower(n)) || up.size() <= 3) return n;
        d = up;
    }
    return fs::name(dir);
}

struct ProjectScan {
    const Env& e;
    Progress& p;
    Category& c;
    int64_t staleBefore = fs::now() - 30 * kDay;

    struct Found {
        std::wstring path, project;
        const wchar_t* kind;
    };
    std::mutex mx;
    std::vector<Found> found;

    void add(const std::wstring& path, const std::wstring& project, const wchar_t* kind) {
        fs::Stats st;
        fs::measure(path, 0, st, &p);
        if (st.bytes == 0) return;
        Item it;
        it.paths = {path};
        it.label = project;
        it.kind = kind;
        it.bytes = st.bytes;
        it.files = st.files;
        it.newest = st.newest;
        it.action = Action::DeleteTree;
        it.showAge = true;
        // Projects you're actively working on start unticked so the next build isn't a slow one.
        it.selected = st.newest != 0 && st.newest < staleBefore;
        c.items.push_back(std::move(it));
    }

    const wchar_t* classify(const std::wstring& n, const Markers& m, const std::wstring& sub) {
        if (n == L"node_modules") return m.pkg && m.lock ? L"node_modules" : nullptr;
        if (n == L"target") {
            if (m.cargo && (fs::isFile(fs::join(sub, L"CACHEDIR.TAG")) || fs::isDir(fs::join(sub, L"debug")) ||
                            fs::isDir(fs::join(sub, L"release"))))
                return L"Rust build output";
            if (m.pom && (fs::isDir(fs::join(sub, L"classes")) || fs::isDir(fs::join(sub, L"maven-status"))))
                return L"Maven build output";
        }
        if (m.pkg) {
            if (n == L".next") return L"Next.js build";
            if (n == L".nuxt") return L"Nuxt build";
            if (n == L".svelte-kit") return L"SvelteKit build";
            if (n == L".turbo") return L"Turborepo cache";
            if (n == L".parcel-cache") return L"Parcel cache";
            if (n == L".angular") return L"Angular cache";
        }
        if (m.gradle && (n == L"build" || n == L".gradle")) return L"Gradle build output";
        if (m.zig && (n == L".zig-cache" || n == L"zig-cache" || n == L"zig-out")) return L"Zig build output";
        if (m.pubspec && (n == L".dart_tool" || n == L"build")) return L"Flutter build output";
        if (m.mix && n == L"_build") return L"Elixir build output";
        if ((m.stack && n == L".stack-work") || (m.cabal && n == L"dist-newstyle")) return L"Haskell build output";
        if (m.swift && n == L".build") return L"Swift build output";
        if (m.godot && n == L".godot") return L"Godot import cache";
        if (m.dotnet) {
            if (n == L"obj" && fs::isFile(fs::join(sub, L"project.assets.json"))) return L".NET build output";
            if (n == L"bin" && (fs::isDir(fs::join(sub, L"Debug")) || fs::isDir(fs::join(sub, L"Release"))))
                return L".NET build output";
        }
        // An out-of-source CMake build folder. In-source builds are skipped: that folder is the project.
        if (fs::isFile(fs::join(sub, L"CMakeCache.txt")) && !fs::isFile(fs::join(sub, L"CMakeLists.txt")))
            return L"CMake build folder";
        std::wstring tag = fs::join(sub, L"CACHEDIR.TAG");
        if (fs::isFile(tag)) {
            std::string t = fs::readSmall(tag, 512);
            if (t.rfind(kCacheDirSignature, 0) == 0)
                return t.find("cargo") != std::string::npos ? L"Rust build output" : L"Cache folder";
        }
        return nullptr;
    }

    bool skip(const WIN32_FIND_DATAW& fd, const std::wstring& n, bool driveRoot, bool homeRoot) {
        DWORD a = fd.dwFileAttributes;
        if ((a & FILE_ATTRIBUTE_HIDDEN) && (a & FILE_ATTRIBUTE_SYSTEM)) return true;
        if (n[0] == L'.' || n[0] == L'$') return true;  // .git, .vscode extensions, .cargo and friends
        if (n == L"node_modules" || n == L"__pycache__" || n == L"site-packages") return true;
        if (homeRoot) {
            static const std::set<std::wstring> home = {L"appdata", L"scoop", L"miniconda3", L"anaconda3"};
            if (home.count(n)) return true;
        }
        if (driveRoot) {
            static const std::set<std::wstring> root = {
                L"windows",  L"program files", L"program files (x86)", L"programdata", L"users",
                L"system volume information", L"recovery", L"perflogs", L"msys64", L"cygwin64",
                L"windowsapps", L"xboxgames", L"intel", L"amd", L"nvidia", L"onedrivetemp"};
            if (root.count(n)) return true;
        }
        return false;
    }

    void visit(const fs::WalkTask& t, std::vector<fs::WalkTask>& sub) {
        if (t.depth > 16) return;
        p.at(t.dir);
        auto entries = fs::list(t.dir);
        Markers mk = readMarkers(entries);
        if (mk.venv) return;  // a Python virtualenv; recreating one isn't always easy, so leave it be
        std::wstring project = projectName(t.dir);
        for (auto& fd : entries) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fs::isLink(fd.dwFileAttributes)) continue;
            std::wstring n = fs::lower(fd.cFileName), path = fs::join(t.dir, fd.cFileName);
            if (const wchar_t* kind = classify(n, mk, path)) {
                std::lock_guard<std::mutex> lock(mx);
                found.push_back({path, project, kind});
                continue;
            }
            if (skip(fd, n, t.flags & kDriveRoot, t.flags & kHomeRoot)) continue;
            sub.push_back({path, t.depth + 1, 0});
        }
    }
};

Category scanProjects(const Env& e, Progress& p) {
    auto c = make("dev", L"Developer build folders",
                  L"node_modules, Rust target folders and other build output. Your code isn't touched. Projects "
                  L"changed in the last 30 days start unticked.",
                  0xE943, 0xF08A3C, Safety::Rebuilds);
    ProjectScan scan{e, p, c};
    std::vector<fs::WalkTask> roots;
    for (auto& root : e.projectRoots) {
        unsigned flags = (root.size() == 3 && root[1] == L':' ? kDriveRoot : 0) |
                         (_wcsicmp(root.c_str(), e.home.c_str()) == 0 ? kHomeRoot : 0);
        roots.push_back({root, 0, flags});
    }
    // Find everything first, then measure, so the two never fight over threads.
    fs::parallelWalk(roots, &p, [&](const fs::WalkTask& t, int, std::vector<fs::WalkTask>& sub) { scan.visit(t, sub); });
    for (auto& f : scan.found) {
        if (p.cancelled()) break;
        scan.add(f.path, f.project, f.kind);
    }
    sortBySize(c);
    return c;
}

// ---------------------------------------------------------------- downloads and duplicates

Category scanOldDownloads(const Env& e, Progress& p) {
    auto c = make("downloads", L"Old installers and archives",
                  L"Setup files and zips in Downloads older than a month. Usually installed or extracted long ago. "
                  L"They go to the Recycle Bin.",
                  0xE896, 0x4CB86B, Safety::CheckFirst);
    static const std::map<std::wstring, const wchar_t*> kinds = {
        {L".exe", L"Installer"},    {L".msi", L"Installer"},   {L".msix", L"Installer"},
        {L".msixbundle", L"Installer"}, {L".appx", L"Installer"}, {L".appxbundle", L"Installer"},
        {L".zip", L"Archive"},      {L".7z", L"Archive"},      {L".rar", L"Archive"},
        {L".tar", L"Archive"},      {L".gz", L"Archive"},      {L".tgz", L"Archive"},
        {L".xz", L"Archive"},       {L".iso", L"Disk image"},  {L".img", L"Disk image"},
    };
    int64_t cutoff = fs::now() - 30 * kDay;
    p.at(e.downloads);
    for (auto& fd : fs::list(e.downloads)) {
        DWORD a = fd.dwFileAttributes;
        if ((a & FILE_ATTRIBUTE_DIRECTORY) || fs::isLink(a) || fs::cloudOnly(a) || (a & FILE_ATTRIBUTE_SYSTEM)) continue;
        auto k = kinds.find(fs::extension(fd.cFileName));
        if (k == kinds.end()) continue;
        int64_t t = fs::toUnix(fd.ftLastWriteTime);
        if (t >= cutoff) continue;
        Item it;
        it.paths = {fs::join(e.downloads, fd.cFileName)};
        it.label = fd.cFileName;
        it.kind = k->second;
        it.bytes = fs::fileSize(fd);
        it.files = 1;
        it.newest = t;
        it.action = Action::DeleteTree;
        it.recycle = true;
        it.showAge = true;
        if (it.bytes) c.items.push_back(std::move(it));
    }
    sortBySize(c);
    return c;
}

struct FileRec {
    std::wstring path;
    uint64_t size = 0;
    int64_t created = 0;
};

void collectFiles(const fs::WalkTask& t, std::vector<fs::WalkTask>& sub, std::vector<FileRec>& out, Progress& p) {
    if (t.depth > 14) return;
    auto entries = fs::list(t.dir);
    for (auto& fd : entries) {
        std::wstring n = fs::lower(fd.cFileName);
        // Copies inside a repo or a virtualenv are there on purpose.
        if (n == L".git" || n == L"pyvenv.cfg") return;
    }
    p.at(t.dir);
    static const std::set<std::wstring> buildJunk = {L".pdb", L".obj", L".lib", L".pch", L".ilk", L".idb",
                                                     L".rlib", L".rmeta", L".o", L".a", L".ipch"};
    for (auto& fd : entries) {
        DWORD a = fd.dwFileAttributes;
        std::wstring n = fs::lower(fd.cFileName);
        if (fs::isLink(a) || (a & FILE_ATTRIBUTE_SYSTEM) || n[0] == L'.' || n[0] == L'$') continue;
        std::wstring path = fs::join(t.dir, fd.cFileName);
        if (a & FILE_ATTRIBUTE_DIRECTORY) {
            if (n == L"node_modules" || n == L"appdata" || n == L"__pycache__") continue;
            sub.push_back({path, t.depth + 1});
            continue;
        }
        uint64_t size = fs::fileSize(fd);
        if (fs::cloudOnly(a) || (a & FILE_ATTRIBUTE_HIDDEN) || size < (1u << 20)) continue;
        if (buildJunk.count(fs::extension(n))) continue;
        out.push_back({path, size, fs::toUnix(fd.ftCreationTime)});
        p.files++;
    }
}

class Sha256 {
public:
    Sha256() { BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0); }
    ~Sha256() {
        if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
    }
    // Hashes up to `limit` bytes of the file. Returns an empty string if it can't be read.
    std::string file(const std::wstring& path, uint64_t limit, Progress& p, uint64_t* fileId) {
        HANDLE h = CreateFileW(fs::longPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h == INVALID_HANDLE_VALUE) return {};
        if (fileId) {
            BY_HANDLE_FILE_INFORMATION info{};
            GetFileInformationByHandle(h, &info);
            *fileId = (uint64_t(info.dwVolumeSerialNumber) << 48) ^ (uint64_t(info.nFileIndexHigh) << 32) ^ info.nFileIndexLow;
        }
        BCRYPT_HASH_HANDLE hash = nullptr;
        std::string digest;
        if (alg_ && BCryptCreateHash(alg_, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
            buf_.resize(1 << 20);
            uint64_t done = 0;
            DWORD got = 0;
            bool ok = true;
            while (done < limit && !p.cancelled()) {
                DWORD want = DWORD(std::min<uint64_t>(buf_.size(), limit - done));
                if (!ReadFile(h, buf_.data(), want, &got, nullptr)) {
                    ok = false;
                    break;
                }
                if (got == 0) break;
                BCryptHashData(hash, buf_.data(), got, 0);
                done += got;
            }
            std::array<unsigned char, 32> out{};
            BCryptFinishHash(hash, out.data(), ULONG(out.size()), 0);
            BCryptDestroyHash(hash);
            if (ok && !p.cancelled()) digest.assign(reinterpret_cast<char*>(out.data()), out.size());
        }
        CloseHandle(h);
        return digest;
    }

private:
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    std::vector<unsigned char> buf_;
};

std::wstring relativeToHome(const std::wstring& path, const Env& e) {
    if (fs::startsWith(path, e.home) && path.size() > e.home.size() + 1) return path.substr(e.home.size() + 1);
    return path;
}

Category scanDuplicates(const Env& e, Progress& p) {
    auto c = make("dupes", L"Duplicate files",
                  L"Identical copies of files 1 MB or bigger in your personal folders. One copy of each is always "
                  L"kept. Extras go to the Recycle Bin.",
                  0xE8C8, 0xE0A526, Safety::CheckFirst);
    if (!e.duplicates) return c;
    std::vector<fs::WalkTask> roots;
    for (auto& root : e.personalRoots) {
        // Skip a root that sits inside another one (Desktop inside OneDrive, say).
        bool nested = false;
        for (auto& other : e.personalRoots)
            if (&other != &root && fs::startsWith(root, other) && root.size() > other.size()) nested = true;
        if (!nested) roots.push_back({root});
    }
    std::vector<std::vector<FileRec>> perWorker(static_cast<size_t>(fs::workerCount()));
    fs::parallelWalk(roots, &p, [&](const fs::WalkTask& t, int w, std::vector<fs::WalkTask>& sub) {
        collectFiles(t, sub, perWorker[size_t(w)], p);
    });
    std::vector<FileRec> files;
    for (auto& v : perWorker) files.insert(files.end(), std::make_move_iterator(v.begin()), std::make_move_iterator(v.end()));
    std::sort(files.begin(), files.end(), [](const FileRec& a, const FileRec& b) {
        return a.size != b.size ? a.size < b.size : a.path < b.path;
    });

    // Only files that share their size with another one can be copies.
    std::vector<std::pair<size_t, size_t>> groups;
    for (size_t i = 0, j; i < files.size(); i = j) {
        for (j = i; j < files.size() && files[j].size == files[i].size; ++j) {}
        if (j - i > 1) groups.push_back({i, j});
    }

    std::vector<Sha256> hashers(static_cast<size_t>(fs::workerCount()));
    std::mutex m;
    fs::parallelFor(groups.size(), [&](size_t g, int w) {
        if (p.cancelled()) return;
        Sha256& sha = hashers[size_t(w)];
        auto [i, j] = groups[g];
        // Compare the first 64 KB, then the whole file, skipping hard links to one file.
        std::map<std::string, std::vector<const FileRec*>> byHead;
        std::set<uint64_t> ids;
        for (size_t k = i; k < j; ++k) {
            p.at(files[k].path);
            uint64_t id = 0;
            std::string head = sha.file(files[k].path, 64 * 1024, p, &id);
            if (head.empty() || !ids.insert(id).second) continue;
            byHead[head].push_back(&files[k]);
        }
        for (auto& [head, group] : byHead) {
            if (group.size() < 2) continue;
            std::map<std::string, std::vector<const FileRec*>> byFull;
            for (auto* f : group) {
                std::string full = f->size <= 64 * 1024 ? head : sha.file(f->path, f->size, p, nullptr);
                if (!full.empty()) byFull[full].push_back(f);
            }
            for (auto& [digest, same] : byFull) {
                if (same.size() < 2) continue;
                // Keep the copy that isn't in Downloads, then the oldest one.
                auto keeper = *std::min_element(same.begin(), same.end(), [&](const FileRec* a, const FileRec* b) {
                    bool da = fs::startsWith(a->path, e.downloads), db = fs::startsWith(b->path, e.downloads);
                    if (da != db) return !da;
                    if (a->created != b->created) return a->created < b->created;
                    return a->path.size() < b->path.size();
                });
                std::lock_guard<std::mutex> lock(m);
                for (auto* f : same) {
                    if (f == keeper) continue;
                    Item it;
                    it.paths = {f->path};
                    it.label = relativeToHome(f->path, e);
                    it.line2 = L"Same as " + relativeToHome(keeper->path, e);
                    it.bytes = f->size;
                    it.files = 1;
                    it.action = Action::DeleteTree;
                    it.recycle = true;
                    c.items.push_back(std::move(it));
                }
            }
        }
    });
    sortBySize(c);
    return c;
}

}  // namespace

Env detectEnv() {
    Env e;
    e.home = known(FOLDERID_Profile);
    e.local = known(FOLDERID_LocalAppData);
    e.localLow = known(FOLDERID_LocalAppDataLow);
    e.roaming = known(FOLDERID_RoamingAppData);
    e.programData = known(FOLDERID_ProgramData);
    e.downloads = known(FOLDERID_Downloads);
    wchar_t buf[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, buf);
    e.temp = std::wstring(buf, n);
    if (e.temp.size() > 3 && e.temp.back() == L'\\') e.temp.pop_back();
    GetWindowsDirectoryW(buf, MAX_PATH);
    e.windir = buf;
    e.admin = elevated();

    addUnique(e.projectRoots, e.home);
    for (auto& d : fixedDrives()) addUnique(e.projectRoots, d);
    for (auto id : {FOLDERID_Downloads, FOLDERID_Desktop, FOLDERID_Documents, FOLDERID_Pictures, FOLDERID_Videos,
                    FOLDERID_Music})
        addUnique(e.personalRoots, known(id));
    return e;
}

Env sandboxEnv(const std::wstring& home) {
    Env e;
    e.home = home;
    e.local = fs::join(home, L"AppData\\Local");
    e.localLow = fs::join(home, L"AppData\\LocalLow");
    e.roaming = fs::join(home, L"AppData\\Roaming");
    e.temp = fs::join(e.local, L"Temp");
    e.windir = fs::join(home, L"Windows");
    e.programData = fs::join(home, L"ProgramData");
    e.downloads = fs::join(home, L"Downloads");
    e.projectRoots = {home};
    for (auto n : {L"Downloads", L"Desktop", L"Documents", L"Pictures", L"Videos", L"Music"})
        e.personalRoots.push_back(fs::join(home, n));
    e.admin = true;  // the fake Windows folder is ours to read
    e.sandbox = true;
    return e;
}

void scanAll(const Env& env, Progress& p, const std::function<void(Category&&)>& sink) {
    using Scan = Category (*)(const Env&, Progress&);
    // Quick checks first so the list fills in fast; the full disk walks go last.
    const std::pair<const wchar_t*, Scan> steps[] = {
        {L"temporary files", scanTemp},
        {L"the Recycle Bin", scanRecycleBin},
        {L"browser caches", scanBrowsers},
        {L"app caches", scanApps},
        {L"package manager caches", scanPackageCaches},
        {L"shader caches", scanShaders},
        {L"crash reports", scanCrashes},
        {L"Windows Update", scanWindowsUpdate},
        {L"Downloads", scanOldDownloads},
        {L"build folders", scanProjects},
        {L"duplicate files", scanDuplicates},
    };
    for (auto& [label, scan] : steps) {
        if (p.cancelled()) return;
        // Not checked at all, so don't report them as "nothing found" either.
        if (scan == scanWindowsUpdate && !env.admin) continue;
        if (scan == scanRecycleBin && env.sandbox) continue;
        p.stage(label);
        Category c = scan(env, p);
        if (p.cancelled()) return;
        sink(std::move(c));
    }
}

}  // namespace roomy
