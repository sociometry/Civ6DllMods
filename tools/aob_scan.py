#!/usr/bin/env python3
"""aob_scan.py —— 校验 GameCore 插件里所有 AOB 特征码的唯一性。

用法：
    python tools/aob_scan.py [--dll <路径>]

默认扫描
    DLC\Expansion2\Binaries\Win64\GameCore_XP2_FinalRelease.dll
的 .text 段，从 recipes/GameCore 下所有 .cpp/.h 里抓出形如

    HookName("符号名", "48 8B C4 ...")

的第二参数（AOB），逐条统计命中数：
    * 1 处  -> OK（唯一，可以安全作为兜底）
    * 0 处  -> 说明特征码已经过时（游戏更新了），必须更新
    * >1 处 -> 危险：兜底会随机命中别的函数，应加长特征码或干脆去掉

为什么需要它：Loader 的 ResolveByHookInfo 在符号名解析失败时会拿 AOB 去扫，
命中多处时只取第一个——那往往就是错的函数，装上钩子等于随机破坏内存。
"""
import argparse
import os
import re
import struct
import sys

DEFAULT_DLL = r"E:\SteamLibrary\steamapps\common\Sid Meier's Civilization VI\DLC\Expansion2\Binaries\Win64\GameCore_XP2_FinalRelease.dll"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE_DIRS = [
    os.path.join(ROOT, "recipes", "GameCore", "Civ6BasePlugin_GameCore", "dll"),
]


def read_text_section(path):
    data = open(path, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    numsec = struct.unpack_from("<H", data, pe + 6)[0]
    optsize = struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    sec = opt + optsize
    for i in range(numsec):
        off = sec + i * 40
        name = data[off:off + 8].rstrip(b"\x00").decode("latin1")
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, off + 8)
        if name == ".text":
            return vaddr, data[rawptr:rawptr + rawsize]
    raise SystemExit("no .text section")


def parse_pattern(pat):
    toks = []
    for t in pat.split():
        if t == "??":
            toks.append((0x00, 0x00))
        else:
            toks.append((int(t, 16), 0xFF))
    return toks


def find_all(hay, toks, limit=50):
    n = len(toks)
    if n == 0:
        return []
    b0, m0 = toks[0]
    hits = []
    i = 0
    last = len(hay) - n
    while i <= last:
        if (hay[i] & m0) == b0:
            ok = True
            for k in range(1, n):
                b, m = toks[k]
                if (hay[i + k] & m) != b:
                    ok = False
                    break
            if ok:
                hits.append(i)
                if len(hits) >= limit:
                    break
        i += 1
    return hits


def collect_targets():
    """从源码里抓 HookName("...", aob) 与 HookRva(rva, aob) 的特征码。

    两种写法共用同一套 AOB 语义（见 docs/PLUGIN_GUIDE.md §5.2），只抓 HookName
    会漏掉以 RVA 为主路径的目标——那正是最容易写错的一类（RVA 手抄错一位也
    照样编译通过），必须同样纳入唯一性校验。
    """
    by_name = re.compile(
        r'HookName\(\s*"((?:[^"\\]|\\.)*)"\s*,\s*\n?\s*"((?:[^"\\]|\\.)*)"', re.S)
    by_rva = re.compile(
        r'HookRva\(\s*(0[xX][0-9A-Fa-f]+)\s*,\s*\n?\s*"((?:[^"\\]|\\.)*)"', re.S)
    targets = []
    for base in SOURCE_DIRS:
        for dirpath, _dirs, files in os.walk(base):
            for fn in files:
                if not fn.endswith((".cpp", ".h")):
                    continue
                path = os.path.join(dirpath, fn)
                text = open(path, encoding="utf-8", errors="replace").read()
                for regex, label in ((by_name, None), (by_rva, "rva")):
                    for m in regex.finditer(text):
                        first = m.group(1)
                        aob = m.group(2)
                        if not re.search(r"\b[0-9A-Fa-f?]{2}\b", aob):
                            continue
                        sym = first if label is None else f"rva:{first}"
                        line = text[:m.start()].count("\n") + 1
                        targets.append((os.path.relpath(path, ROOT), line, sym, aob))
    return targets


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dll", default=DEFAULT_DLL)
    args = ap.parse_args()
    if not os.path.exists(args.dll):
        raise SystemExit(f"dll not found: {args.dll}")

    text_rva, hay = read_text_section(args.dll)
    print(f"image: {args.dll}")
    print(f".text rva=0x{text_rva:X} size=0x{len(hay):X}\n")

    targets = collect_targets()
    if not targets:
        raise SystemExit("no HookName(..., aob) found")

    bad = 0
    for rel, line, sym, aob in targets:
        hits = find_all(hay, parse_pattern(aob))
        if len(hits) == 1:
            status = "OK  "
        elif not hits:
            status = "MISS"
            bad += 1
        else:
            status = "MULTI"
            bad += 1
        locs = ", ".join(f"0x{text_rva + h:X}" for h in hits[:4])
        print(f"[{status}] {sym}")
        print(f"        {rel}:{line}  hits={len(hits)}  {locs}")

    print()
    if bad:
        print(f"{bad} pattern(s) need attention")
        return 1
    print(f"all {len(targets)} patterns unique")
    return 0


if __name__ == "__main__":
    sys.exit(main())