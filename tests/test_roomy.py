"""End-to-end tests for Roomy.

Builds a fake user profile in a temp folder (projects, caches, downloads, duplicates and a few
traps), runs `roomy --scan --json --home <fake>` and `roomy --clean`, and checks that exactly the
right things were found and removed. Nothing outside the temp folder is touched.

    python tests/test_roomy.py [path\\to\\roomy.exe]
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build" / "roomy.exe"
DAY = 86400
CACHEDIR_TAG = "Signature: 8a477f597d28d172789f06886806bc55\n# This file is a cache directory tag created by cargo.\n"

failures = []


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        failures.append(what)


def lp(p):
    """Extended-length path so the deep node_modules test works."""
    p = os.path.abspath(p)
    return p if p.startswith("\\\\?\\") else "\\\\?\\" + p


def write(path, size=1024, age_days=0, data=None):
    path = Path(path)
    os.makedirs(lp(path.parent), exist_ok=True)
    with open(lp(path), "wb") as f:
        f.write(data if data is not None else os.urandom(size))
    if age_days:
        t = time.time() - age_days * DAY
        os.utime(lp(path), (t, t))
    return path


def exists(p):
    return os.path.exists(lp(p))


def build_fixture(home: Path):
    local, roaming = home / "AppData/Local", home / "AppData/Roaming"

    # Temp: only the old file goes.
    write(local / "Temp/old.tmp", 4000, age_days=3)
    write(local / "Temp/fresh.tmp", 4000)

    # Rust project, last built two months ago, with a read-only file in target.
    rust = home / "Projects/rusty"
    write(rust / "Cargo.toml", data=b"[package]\nname='rusty'\n")
    write(rust / "src/main.rs", data=b"fn main(){}\n")
    write(rust / "target/CACHEDIR.TAG", data=CACHEDIR_TAG.encode(), age_days=60)
    ro = write(rust / "target/debug/rusty.exe", 300_000, age_days=60)
    os.chmod(lp(ro), 0o444)

    # Node project with a lockfile, touched today, so it should start unticked.
    web = home / "Projects/webapp"
    write(web / "package.json", data=b"{}")
    write(web / "package-lock.json", data=b"{}")
    write(web / "node_modules/left-pad/index.js", 50_000)
    # A path well past 260 characters.
    deep = web / "node_modules" / "/".join(["deeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeep"] * 9) / "file.js"
    write(deep, 10_000)
    # Trap: a junction inside node_modules pointing at something precious.
    precious = home / "Precious"
    write(precious / "keep.txt", data=b"do not delete")
    subprocess.run(["cmd", "/c", "mklink", "/J", str(web / "node_modules/linked"), str(precious)],
                   check=True, capture_output=True)

    # node_modules without a lockfile: left alone.
    write(home / "Projects/nolock/package.json", data=b"{}")
    write(home / "Projects/nolock/node_modules/x/index.js", 20_000)

    # node_modules inside a dot folder (like VS Code extensions): left alone.
    ext = home / ".vscode/extensions/some.ext"
    write(ext / "package.json", data=b"{}")
    write(ext / "package-lock.json", data=b"{}")
    write(ext / "node_modules/dep/index.js", 20_000)

    # CMake: out-of-source build folder found, in-source build ignored.
    write(home / "Projects/cm/CMakeLists.txt", data=b"project(x)")
    write(home / "Projects/cm/build/CMakeCache.txt", 5000, age_days=40)
    write(home / "Projects/cm/build/app.obj", 80_000, age_days=40)
    write(home / "Projects/insrc/CMakeLists.txt", data=b"project(y)")
    write(home / "Projects/insrc/CMakeCache.txt", 5000)

    # A Python virtualenv: left alone even though it contains a "build" folder.
    write(home / "Projects/venv/pyvenv.cfg", data=b"home = C:\\Python")
    write(home / "Projects/venv/build/CMakeCache.txt", 5000)

    # Electron app cache found; a "Cache" folder without the Electron markers ignored.
    write(roaming / "discord/Local State", data=b"{}")
    write(roaming / "discord/Cache/Cache_Data/data_1", 70_000)
    write(roaming / "notanapp/Cache/stuff.bin", 70_000)

    # Browser cache.
    chrome = local / "Google/Chrome/User Data"
    write(chrome / "Default/Preferences", data=b"{}")
    write(chrome / "Default/Cache/Cache_Data/f_000001", 90_000)
    write(chrome / "Default/Bookmarks", data=b"{}")

    # Package caches.
    write(local / "npm-cache/_cacache/content/aa", 33_000)

    # Old installer goes, new one and non-installer stay.
    write(home / "Downloads/old-setup.exe", 200_000, age_days=45)
    write(home / "Downloads/new-setup.exe", 200_000)
    write(home / "Downloads/notes.txt", 2000, age_days=90)

    # Duplicates: a copy in Downloads goes, the Documents original stays.
    blob = os.urandom(2 * 1024 * 1024)
    write(home / "Documents/video.mp4", data=blob, age_days=10)
    write(home / "Downloads/video (1).mp4", data=blob)
    # Same size, different content: not duplicates.
    write(home / "Pictures/a.raw", data=os.urandom(2 * 1024 * 1024))
    write(home / "Pictures/b.raw", data=os.urandom(2 * 1024 * 1024))
    # Hard links to one file: not duplicates.
    h1 = write(home / "Pictures/h1.bin", 1_500_000)
    os.link(lp(h1), lp(home / "Pictures/h2.bin"))
    # Copies inside a git repo are on purpose.
    write(home / "Documents/repo/.git/HEAD", data=b"ref: refs/heads/main")
    write(home / "Documents/repo/asset.bin", data=blob)

    # Crash dumps and the fake Windows folder.
    write(local / "CrashDumps/app.exe.1234.dmp", 120_000)
    write(home / "Windows/Temp/old.log", 3000, age_days=5)
    write(home / "Windows/SoftwareDistribution/Download/kb123.cab", 400_000, age_days=30)
    write(home / "Windows/SoftwareDistribution/Download/kb999.cab", 400_000)


def run(*args):
    r = subprocess.run([str(EXE), *args], capture_output=True, timeout=120)
    if r.returncode != 0:
        print(r.stdout.decode("utf-8", "replace"), r.stderr.decode("utf-8", "replace"))
    return r.returncode, r.stdout.decode("utf-8")


def items_by_cat(report):
    return {c["id"]: c["items"] for c in report["categories"]}


def all_paths(report):
    return [p for c in report["categories"] for it in c["items"] for p in it["paths"]]


def main():
    if not EXE.exists():
        sys.exit(f"{EXE} not found. Build first with build.bat")
    home = Path(tempfile.mkdtemp(prefix="roomy-test-"))
    try:
        build_fixture(home)
        print(f"fixture: {home}\n\nscan")
        code, out = run("--scan", "--json", "--home", str(home))
        check(code == 0, "scan exits cleanly")
        report = json.loads(out)
        cats = items_by_cat(report)
        paths = [p.lower() for p in all_paths(report)]

        def found(sub):
            return any(sub.lower() in p for p in paths)

        dev = {Path(it["paths"][0]).name + ":" + it["label"]: it for it in cats.get("dev", [])}
        check("target:rusty" in dev, "finds the Rust target folder")
        check(dev.get("target:rusty", {}).get("selected") is True, "old Rust build starts ticked")
        check("node_modules:webapp" in dev, "finds node_modules with a lockfile")
        check(dev.get("node_modules:webapp", {}).get("selected") is False, "recently used node_modules starts unticked")
        check(dev.get("node_modules:webapp", {}).get("bytes", 0) >= 60_000, "counts files past 260 characters")
        check(dev.get("node_modules:webapp", {}).get("bytes", 0) < 100_000, "does not follow the junction when measuring")
        check("build:cm" in dev, "finds the out-of-source CMake build")
        check(not found("nolock"), "ignores node_modules without a lockfile")
        check(not found(".vscode"), "ignores dot folders like VS Code extensions")
        check(not found("insrc"), "ignores in-source CMake builds")
        check(not found("projects\\venv"), "ignores Python virtualenvs")

        check(found("discord\\cache"), "finds Electron app caches")
        check(not found("notanapp"), "ignores Cache folders that aren't Electron's")
        check(found("chrome\\user data\\default\\cache"), "finds the Chrome cache")
        check(not found("bookmarks"), "never lists browser data like bookmarks")
        check(found("npm-cache"), "finds the npm cache")
        check(found("crashdumps"), "finds crash dumps")

        temp = cats.get("temp", [])
        check(len(temp) == 2, "temp has your folder and the Windows one")
        check(any(it["bytes"] == 4000 for it in temp), "temp counts only files older than a day")
        upd = cats.get("update", [])
        check(len(upd) == 1 and upd[0]["bytes"] == 400_000, "Windows Update counts only older downloads")

        dl = [it["label"] for it in cats.get("downloads", [])]
        check(dl == ["old-setup.exe"], "only the old installer is listed under Downloads")
        check(all(it["recycle"] and not it["selected"] for it in cats.get("downloads", [])),
              "old downloads go to the Recycle Bin and start unticked")

        dupes = cats.get("dupes", [])
        check(len(dupes) == 1, "exactly one duplicate found")
        check(dupes and dupes[0]["paths"][0].endswith("video (1).mp4"), "the Downloads copy is the one to remove")

        print("\nclean")
        code, out = run("--clean", "--yes", "--all", "--no-recycle", "--json", "--home", str(home))
        check(code == 0, "clean exits cleanly")
        res = json.loads(out)
        check(res["freed"] > 2_500_000, f"reports freed bytes ({res['freed']:,})")

        check(not exists(home / "Projects/rusty/target"), "Rust target removed (read-only file included)")
        check(exists(home / "Projects/rusty/Cargo.toml") and exists(home / "Projects/rusty/src/main.rs"),
              "Rust source untouched")
        check(not exists(home / "Projects/webapp/node_modules"), "node_modules removed (long paths included)")
        check(exists(home / "Precious/keep.txt"), "junction target survived")
        check(exists(home / "Projects/webapp/package.json"), "package.json untouched")
        check(exists(home / "Projects/nolock/node_modules/x/index.js"), "unlocked node_modules untouched")
        check(exists(home / ".vscode/extensions/some.ext/node_modules/dep/index.js"), "extension untouched")
        check(exists(home / "Projects/insrc/CMakeCache.txt"), "in-source CMake untouched")
        check(exists(home / "Projects/venv/build/CMakeCache.txt"), "virtualenv untouched")
        check(not exists(home / "AppData/Local/Temp/old.tmp"), "old temp file removed")
        check(exists(home / "AppData/Local/Temp/fresh.tmp"), "fresh temp file kept")
        check(exists(home / "AppData/Local/Temp"), "temp folder itself kept")
        check(exists(home / "AppData/Local/Google/Chrome/User Data/Default/Bookmarks"), "browser bookmarks kept")
        check(not exists(home / "AppData/Roaming/discord/Cache/Cache_Data/data_1"), "Discord cache emptied")
        check(exists(home / "AppData/Roaming/discord/Local State"), "Discord settings kept")
        check(exists(home / "AppData/Roaming/notanapp/Cache/stuff.bin"), "non-Electron Cache kept")
        check(not exists(home / "Downloads/old-setup.exe"), "old installer removed")
        check(exists(home / "Downloads/new-setup.exe") and exists(home / "Downloads/notes.txt"), "other downloads kept")
        check(not exists(home / "Downloads/video (1).mp4"), "duplicate removed")
        check(exists(home / "Documents/video.mp4"), "original of the duplicate kept")
        check(exists(home / "Pictures/a.raw") and exists(home / "Pictures/b.raw"), "same-size different files kept")
        check(exists(home / "Pictures/h1.bin") and exists(home / "Pictures/h2.bin"), "hard links kept")
        check(exists(home / "Documents/repo/asset.bin"), "files inside git repos kept")
        check(exists(home / "Windows/SoftwareDistribution/Download/kb999.cab"), "recent update download kept")

        print("\nrescan after cleaning")
        code, out = run("--scan", "--json", "--home", str(home))
        left = json.loads(out)
        leftover = [c["id"] for c in left["categories"]]
        check(leftover == [], f"nothing left to clean (got {leftover})")
    finally:
        # Remove the junction first so rmtree can't wander through it.
        link = home / "Projects/webapp/node_modules/linked"
        if os.path.lexists(lp(link)):
            os.rmdir(lp(link))
        shutil.rmtree(lp(home), ignore_errors=True)

    print(f"\n{'all passed' if not failures else f'{len(failures)} failed'}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
