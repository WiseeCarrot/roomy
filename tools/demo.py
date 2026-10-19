"""Builds a believable fake PC for screenshots and renders the README images.

Big files are sparse, so a "6 GB" Rust target folder takes a few KB on disk.

    python tools/demo.py            # builds %TEMP%\\roomy-demo and renders docs/*.png
"""
import ctypes
import os
import shutil
import subprocess
import tempfile
import time
from ctypes import wintypes
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "build" / "roomy.exe"
HOME = Path(tempfile.gettempdir()) / "roomy-demo"
DAY = 86400
GB, MB = 1 << 30, 1 << 20

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.CreateFileW.restype = wintypes.HANDLE
FSCTL_SET_SPARSE = 0x000900C4


kernel32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
                                 wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
kernel32.DeviceIoControl.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD,
                                     wintypes.LPVOID, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
kernel32.SetFilePointerEx.argtypes = [wintypes.HANDLE, ctypes.c_longlong, wintypes.LPVOID, wintypes.DWORD]
kernel32.SetEndOfFile.argtypes = [wintypes.HANDLE]
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
kernel32.GetCompressedFileSizeW.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(wintypes.DWORD)]


def sparse(path, size, age_days=0):
    """A file that reports `size` bytes but takes almost nothing on disk.

    Python's truncate() can't be used here: on Windows it grows a file by writing zeros.
    """
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    h = kernel32.CreateFileW(str(path), 0xC0000000, 0, None, 2, 0x80, None)  # CREATE_ALWAYS
    if h in (None, wintypes.HANDLE(-1).value):
        raise OSError(ctypes.get_last_error(), f"can't create {path}")
    try:
        ret = wintypes.DWORD()
        if not kernel32.DeviceIoControl(h, FSCTL_SET_SPARSE, None, 0, None, 0, ctypes.byref(ret), None):
            raise OSError(ctypes.get_last_error(), f"can't make {path} sparse")
        kernel32.SetFilePointerEx(h, int(size), None, 0)
        kernel32.SetEndOfFile(h)
    finally:
        kernel32.CloseHandle(h)
    high = wintypes.DWORD()
    on_disk = kernel32.GetCompressedFileSizeW(str(path), ctypes.byref(high)) + (high.value << 32)
    if on_disk > 1 << 20:
        os.remove(path)
        raise SystemExit(f"{path} took {on_disk:,} real bytes, stopping so the disk doesn't fill up")
    if age_days:
        t = time.time() - age_days * DAY
        os.utime(path, (t, t))


def text(path, s="", age_days=0):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(s)
    if age_days:
        t = time.time() - age_days * DAY
        os.utime(path, (t, t))


def build():
    if HOME.exists():
        shutil.rmtree(HOME)
    local, roaming = HOME / "AppData/Local", HOME / "AppData/Roaming"
    projects = HOME / "Projects"

    sparse(local / "Temp/setup-log-8812.tmp", 2.7 * GB, 9)
    sparse(local / "Temp/chrome_installer.log", 310 * MB, 40)

    def rust(name, size, age):
        text(projects / name / "Cargo.toml", "[package]")
        text(projects / name / "target/CACHEDIR.TAG", "Signature: 8a477f597d28d172789f06886806bc55\n# created by cargo\n", age)
        sparse(projects / name / "target/debug/deps/lib.rlib", size, age)

    def node(name, size, age):
        text(projects / name / "package.json", "{}")
        text(projects / name / "package-lock.json", "{}")
        sparse(projects / name / "node_modules/.package-lock.json", size, age)

    rust("raytracer", 6.4 * GB, 70)
    rust("tiny-db", 2.1 * GB, 3)
    node("portfolio-site", 1.3 * GB, 120)
    node("weather-dashboard", 870 * MB, 45)
    node("chat-app", 640 * MB, 2)
    text(projects / "mobile-game/CMakeLists.txt", "project(game)")
    text(projects / "mobile-game/build/CMakeCache.txt", "", 50)
    sparse(projects / "mobile-game/build/game.pdb", 1.1 * GB, 50)

    def electron(folder, size):
        text(roaming / folder / "Local State", "{}")
        sparse(roaming / folder / "Cache/Cache_Data/data_3", size)

    electron("Code", 1.4 * GB)
    electron("discord", 690 * MB)
    electron("Slack", 420 * MB)
    electron("obsidian", 96 * MB)
    sparse(local / "Spotify/Data/cache.bin", 2.2 * GB)
    chrome = local / "Google/Chrome/User Data"
    text(chrome / "Default/Preferences", "{}")
    text(chrome / "Profile 1/Preferences", "{}")
    sparse(chrome / "Default/Cache/Cache_Data/data_1", 1.6 * GB)
    sparse(chrome / "Profile 1/Cache/Cache_Data/data_1", 380 * MB)
    edge = local / "Microsoft/Edge/User Data"
    text(edge / "Default/Preferences", "{}")
    sparse(edge / "Default/Cache/Cache_Data/data_1", 520 * MB)

    sparse(local / "npm-cache/_cacache/content-v2/sha512/blob", 3.1 * GB)
    sparse(local / "pip/cache/http/blob", 1.2 * GB)
    sparse(HOME / ".cargo/registry/cache/index.crates.io/blob", 2.4 * GB)
    sparse(local / "NVIDIA/DXCache/cache.bin", 1.8 * GB)
    sparse(local / "D3DSCache/cache.bin", 260 * MB)
    sparse(local / "CrashDumps/Game.exe.18220.dmp", 1.9 * GB)

    sparse(HOME / "Downloads/NVIDIA-Driver-572.16.exe", 712 * MB, 95)
    sparse(HOME / "Downloads/python-3.13.1-amd64.exe", 27 * MB, 160)
    sparse(HOME / "Downloads/SteamSetup.exe", 2.3 * MB, 300)
    sparse(HOME / "Downloads/blender-4.3-windows-x64.zip", 341 * MB, 61)
    sparse(HOME / "Downloads/ubuntu-24.04-desktop-amd64.iso", 5.8 * GB, 210)
    sparse(HOME / "Downloads/vacation-2025 (1).mp4", 1.37 * GB)
    sparse(HOME / "Videos/vacation-2025.mp4", 1.37 * GB, 200)
    sparse(HOME / "Pictures/wallpaper-pack (1).zip", 214 * MB)
    sparse(HOME / "Documents/wallpaper-pack.zip", 214 * MB, 100)


def shot(name, theme, expand, size="980x760"):
    out = ROOT / "docs" / name
    subprocess.run([str(EXE), "--home", str(HOME), "--screenshot", str(out), "--theme", theme,
                    "--expand", expand, "--size", size], check=True)
    print("wrote", out.relative_to(ROOT))


if __name__ == "__main__":
    build()
    shot("screenshot-dark.png", "dark", "dev")
    shot("screenshot-light.png", "light", "downloads")
