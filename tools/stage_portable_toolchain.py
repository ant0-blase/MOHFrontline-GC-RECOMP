#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import stat
import tarfile
import tempfile
import urllib.request
import zipfile

CMAKE_VERSION = "4.4.3"
NINJA_VERSION = "1.13.2"
ZIG_VERSION = "0.16.0"
PYTHON_WINDOWS_VERSION = "3.13.13"
PYTHON_LINUX_VERSION = "3.13.12"
PYTHON_LINUX_RELEASE = "20260303"

URLS = {
    "windows": {
        "cmake": f"https://github.com/Kitware/CMake/releases/download/v{CMAKE_VERSION}/cmake-{CMAKE_VERSION}-windows-x86_64.zip",
        "ninja": f"https://github.com/ninja-build/ninja/releases/download/v{NINJA_VERSION}/ninja-win.zip",
        "zig": f"https://ziglang.org/download/{ZIG_VERSION}/zig-x86_64-windows-{ZIG_VERSION}.zip",
        "python": f"https://www.python.org/ftp/python/{PYTHON_WINDOWS_VERSION}/python-{PYTHON_WINDOWS_VERSION}-embed-amd64.zip",
    },
    "linux": {
        "cmake": f"https://github.com/Kitware/CMake/releases/download/v{CMAKE_VERSION}/cmake-{CMAKE_VERSION}-linux-x86_64.tar.gz",
        "ninja": f"https://github.com/ninja-build/ninja/releases/download/v{NINJA_VERSION}/ninja-linux.zip",
        "zig": f"https://ziglang.org/download/{ZIG_VERSION}/zig-x86_64-linux-{ZIG_VERSION}.tar.xz",
        "python": (
            "https://github.com/astral-sh/python-build-standalone/releases/download/"
            f"{PYTHON_LINUX_RELEASE}/cpython-{PYTHON_LINUX_VERSION}+{PYTHON_LINUX_RELEASE}-"
            "x86_64-unknown-linux-gnu-install_only_stripped.tar.gz"
        ),
    },
}

def download(url: str, dst: Path) -> None:
    print(f"[portable] download {url}")
    req = urllib.request.Request(url, headers={"User-Agent": "MOHFrontline-portable/1"})
    with urllib.request.urlopen(req) as response, dst.open("wb") as output:
        shutil.copyfileobj(response, output)

def extract(archive: Path, dst: Path) -> None:
    dst.mkdir(parents=True, exist_ok=True)
    if archive.suffix == ".zip":
        with zipfile.ZipFile(archive) as z:
            z.extractall(dst)
    else:
        with tarfile.open(archive, "r:*") as t:
            t.extractall(dst)

def single_root(path: Path) -> Path:
    entries = list(path.iterdir())
    return entries[0] if len(entries) == 1 and entries[0].is_dir() else path

def copy_contents(src: Path, dst: Path) -> None:
    dst.mkdir(parents=True, exist_ok=True)
    for child in src.iterdir():
        target = dst / child.name
        if child.is_dir():
            shutil.copytree(child, target, dirs_exist_ok=True)
        else:
            shutil.copy2(child, target)

def executable(path: Path) -> None:
    if path.exists():
        path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

def write_toolchain(root: Path, platform: str) -> None:
    tc = root / "toolchain"
    bindir = tc / "bin"
    bindir.mkdir(parents=True, exist_ok=True)
    target = "x86_64-windows-gnu" if platform == "windows" else "x86_64-linux-gnu"
    zig = "${CMAKE_CURRENT_LIST_DIR}/zig/zig.exe" if platform == "windows" else "${CMAKE_CURRENT_LIST_DIR}/zig/zig"

    if platform == "windows":
        ar = bindir / "zig-ar.cmd"
        ranlib = bindir / "zig-ranlib.cmd"
        ar.write_text('@echo off\r\n"%~dp0..\\zig\\zig.exe" ar %*\r\n')
        ranlib.write_text('@echo off\r\n"%~dp0..\\zig\\zig.exe" ranlib %*\r\n')
        ar_ref = "${CMAKE_CURRENT_LIST_DIR}/bin/zig-ar.cmd"
        ranlib_ref = "${CMAKE_CURRENT_LIST_DIR}/bin/zig-ranlib.cmd"
    else:
        ar = bindir / "zig-ar"
        ranlib = bindir / "zig-ranlib"
        ar.write_text('#!/bin/sh\nexec "$(dirname "$0")/../zig/zig" ar "$@"\n')
        ranlib.write_text('#!/bin/sh\nexec "$(dirname "$0")/../zig/zig" ranlib "$@"\n')
        executable(ar)
        executable(ranlib)
        ar_ref = "${CMAKE_CURRENT_LIST_DIR}/bin/zig-ar"
        ranlib_ref = "${CMAKE_CURRENT_LIST_DIR}/bin/zig-ranlib"

    (tc / "moh-zig.cmake").write_text(
        "# Generated portable MOH Frontline toolchain\n"
        f'set(CMAKE_C_COMPILER "{zig}" "cc" "-target" "{target}")\n'
        f'set(CMAKE_CXX_COMPILER "{zig}" "c++" "-target" "{target}")\n'
        f'set(CMAKE_AR "{ar_ref}" CACHE FILEPATH "" FORCE)\n'
        f'set(CMAKE_RANLIB "{ranlib_ref}" CACHE FILEPATH "" FORCE)\n'
        f'set(CMAKE_C_COMPILER_AR "{ar_ref}" CACHE FILEPATH "" FORCE)\n'
        f'set(CMAKE_CXX_COMPILER_AR "{ar_ref}" CACHE FILEPATH "" FORCE)\n'
        f'set(CMAKE_C_COMPILER_RANLIB "{ranlib_ref}" CACHE FILEPATH "" FORCE)\n'
        f'set(CMAKE_CXX_COMPILER_RANLIB "{ranlib_ref}" CACHE FILEPATH "" FORCE)\n'
        "set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)\n"
    )

def stage(root: Path, platform: str) -> None:
    tc = root / "toolchain"
    if tc.exists():
        shutil.rmtree(tc)
    (tc / "bin").mkdir(parents=True)
    (tc / "licenses").mkdir(parents=True)

    with tempfile.TemporaryDirectory(prefix="moh-portable-") as temp_s:
        temp = Path(temp_s)
        for name, url in URLS[platform].items():
            if url.endswith(".zip"):
                suffix = ".zip"
            elif url.endswith(".tar.xz"):
                suffix = ".tar.xz"
            else:
                suffix = ".tar.gz"
            archive = temp / f"{name}{suffix}"
            unpack = temp / f"{name}-unpack"
            download(url, archive)
            extract(archive, unpack)
            src = single_root(unpack)

            if name == "cmake":
                copy_contents(src, tc / "cmake")
            elif name == "zig":
                copy_contents(src, tc / "zig")
            elif name == "python":
                if platform == "windows":
                    copy_contents(src, tc / "python")
                else:
                    pyroot = src / "python" if (src / "python").is_dir() else src
                    copy_contents(pyroot, tc / "python")
            else:
                expected = "ninja.exe" if platform == "windows" else "ninja"
                exe = next(p for p in src.rglob(expected) if p.is_file())
                shutil.copy2(exe, tc / "bin" / expected)

    for path in [
        tc / "bin/ninja",
        tc / "cmake/bin/cmake",
        tc / "cmake/bin/ctest",
        tc / "zig/zig",
        tc / "python/bin/python3",
    ]:
        executable(path)

    write_toolchain(root, platform)
    (tc / "bootstrap.txt").write_text(
        "moh-portable-toolchain-v1\n"
        f"platform={platform}\n"
        f"cmake={CMAKE_VERSION}\n"
        f"ninja={NINJA_VERSION}\n"
        f"zig={ZIG_VERSION}\n"
        f"python={PYTHON_WINDOWS_VERSION if platform == 'windows' else PYTHON_LINUX_VERSION}\n"
    )
    print(f"[portable] ready: {tc}")

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", type=Path, required=True)
    ap.add_argument("--platform", choices=("windows", "linux"), required=True)
    ns = ap.parse_args()
    stage(ns.root.resolve(), ns.platform)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
