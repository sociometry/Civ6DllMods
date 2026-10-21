#!/usr/bin/env python3
"""package_release.py - 打包 Civ6DllMods 的三个插件产物。

用法（在仓库根目录执行）：
    python package_release.py                  # 打当前版本
    python package_release.py --version 1.0.1  # 覆盖版本号
    python package_release.py --no-build       # 跳过构建，直接用现有产物

产物（与 docs/PLUGIN_GUIDE.md 的交付说明一致）：
    Plugins/Global/Civ6BasePlugin_Global.dll       全局插件
    Plugins/GameCore/Civ6BasePlugin_GameCore.dll   对局插件
    Plugins/GameCore/FourRing.dll                  FourRing

产物落在 dist/ 下：Civ6DllMods-<ver>.zip
Loader 侧有自己独立的打包脚本（Civ6DllLoader/package_release.py）。

为什么只能放这三个目录：Loader 严格只扫 Plugins/Global 与 Plugins/GameCore，
散在 Plugins/ 根目录的 DLL 不认。脚本按这个结构装包，用户解压后直接对位。
"""

from __future__ import annotations

import argparse
import hashlib
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

# Windows 控制台默认可能是 GBK；输出里有中文，统一成 UTF-8 避免乱码。
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

SLN = "Recipes.sln"

MSBUILD_CANDIDATES = [
    "msbuild",
    r"E:\VisualStudio\Products\MSBuild\Current\Bin\MSBuild.exe",
    r"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe",
    r"C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\MSBuild.exe",
]


def find_msbuild(explicit):
    if explicit:
        return explicit if Path(explicit).is_file() or shutil.which(explicit) else None
    for cand in MSBUILD_CANDIDATES:
        if Path(cand).is_file():
            return cand
        found = shutil.which(cand)
        if found:
            return found
    return None


def run_build(repo, msbuild):
    """跑一次 Release|x64 构建。True = 成功。"""
    if msbuild is None:
        print("[错误] 找不到 msbuild，无法构建。")
        print("       用 --msbuild <路径> 指定，或加 --no-build 直接打包现有产物。")
        return False
    sln = repo / SLN
    if not sln.is_file():
        print("[错误] 找不到 " + str(sln))
        return False
    cmd = [msbuild, str(sln), "/t:Build",
           "/p:Configuration=Release", "/p:Platform=x64",
           "/v:minimal", "/nologo"]
    print("[构建] " + str(sln))
    rc = subprocess.run(cmd, cwd=str(repo)).returncode
    if rc != 0:
        print("[错误] 构建失败, exit=" + str(rc))
        return False
    return True


def collect(repo):
    out = repo / "x64" / "Release"
    return [
        (out / "Civ6BasePlugin_Global.dll",
         "Plugins/Global/Civ6BasePlugin_Global.dll"),
        (out / "Civ6BasePlugin_GameCore.dll",
         "Plugins/GameCore/Civ6BasePlugin_GameCore.dll"),
        (out / "FourRing.dll", "Plugins/GameCore/FourRing.dll"),
    ]


def check(items):
    """逐个确认源文件存在。缺一个就整体失败 —— 发布包少文件很难事后发现。"""
    missing = [(s, d) for s, d in items if not s.is_file()]
    if not missing:
        return True
    print("[错误] 缺少以下产物：")
    for s, d in missing:
        print("       " + str(s) + "   -> " + d)
    return False


def sha256(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def write_zip(zip_path, items, extra_text=None):
    """打 zip 并写 SHA256SUMS.txt。返回包内文件数。"""
    zip_path.parent.mkdir(parents=True, exist_ok=True)
    lines = []
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for src, arc in items:
            zf.write(src, arc)
            lines.append(sha256(src) + "  " + arc)
        for arc, text in (extra_text or {}).items():
            data = text.encode("utf-8")
            zf.writestr(arc, data)
            lines.append(hashlib.sha256(data).hexdigest() + "  " + arc)
        zf.writestr("SHA256SUMS.txt", "\n".join(lines) + "\n")
    return len(lines)


README = """# Civ6DllMods {version}

三个插件产物。

## 产物

    Plugins/Global/Civ6BasePlugin_Global.dll       全局插件
    Plugins/GameCore/Civ6BasePlugin_GameCore.dll   对局插件
    Plugins/GameCore/FourRing.dll                  FourRing

## 装法

放进 Mod 的 Binaries/Win64/Plugins/ 下，目录名必须是 Global / GameCore。
Loader 严格只扫这两个目录，散在 Plugins/ 根目录的 DLL 不认。

同时需要 Loader 本体（见 Civ6DllLoader 的发布包）。

## 前提

插件的 VERSIONINFO 文件版本是**强制要求**：没有版本的 DLL 会被 Loader
直接拒绝，并在日志里留一行。
"""


def main():
    ap = argparse.ArgumentParser(description="打包 Civ6DllMods 插件产物")
    ap.add_argument("--version", default="1.0", help="版本号，默认 1.0")
    ap.add_argument("--no-build", action="store_true", help="跳过构建")
    ap.add_argument("--msbuild", default=None, help="msbuild 路径（默认自动探测）")
    ap.add_argument("--outdir", default="dist", help="输出目录，默认 dist/")
    args = ap.parse_args()

    repo = Path(__file__).resolve().parent
    outdir = (repo / args.outdir).resolve()
    version = args.version

    print("版本  : " + version)
    print("仓库  : " + str(repo))
    print("输出  : " + str(outdir))
    print()

    if not args.no_build:
        if not run_build(repo, find_msbuild(args.msbuild)):
            return 1
        print()

    items = collect(repo)
    if not check(items):
        return 1

    zip_path = outdir / ("Civ6DllMods-" + version + ".zip")
    n = write_zip(zip_path, items, {"README.md": README.format(version=version)})
    print("[完成] " + zip_path.name + "  " + str(n) + " 项, " +
          format(zip_path.stat().st_size, ",") + " 字节")
    print()
    print("下一步：打 tag 并上传这个 zip 到 Release。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
