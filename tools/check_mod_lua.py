#!/usr/bin/env python3
"""check_mod_lua.py - 校验 Civ6 mod 的 Lua 与 XML 语法。

Civ6 用的是 HavokScript，支持 local x:number = 1 这类类型注解，
标准 Lua 5.1 的 luac 不认识，所以这里先把注解 strip 掉再交给 luac -p 做语法检查。

用法:
    python tools/check_mod_lua.py <mod目录>
"""
import os
import re
import subprocess
import sys
import tempfile
import xml.dom.minidom

LUAC = r"C:\Program Files (x86)\Lua\5.1\luac.exe"

ANNOTATION = re.compile(r"(?<=[\w\)\]])\s*:\s*(number|string|boolean|table|function|thread|userdata)\b")


def strip_annotations(text):
    return ANNOTATION.sub("", text)


def check_lua(path):
    src = open(path, encoding="utf-8").read()
    cleaned = strip_annotations(src)
    with tempfile.NamedTemporaryFile("w", suffix=".lua", delete=False, encoding="utf-8", newline="\n") as f:
        f.write(cleaned)
        tmp = f.name
    try:
        r = subprocess.run([LUAC, "-p", tmp], capture_output=True, text=True,
                           encoding="utf-8", errors="replace")
        return r.returncode == 0, (r.stderr or "").replace(tmp, path).strip()
    finally:
        os.unlink(tmp)


def check_xml(path):
    try:
        xml.dom.minidom.parse(path)
        return True, ""
    except Exception as e:
        return False, str(e)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    root = sys.argv[1]
    lua_files, xml_files = [], []
    for dirpath, _dirs, files in os.walk(root):
        if os.sep + "Binaries" in dirpath:
            continue
        for fn in files:
            p = os.path.join(dirpath, fn)
            if fn.endswith(".lua") and ".bak" not in fn:
                lua_files.append(p)
            elif (fn.endswith(".xml") or fn.endswith(".modinfo")) and ".bak" not in fn:
                xml_files.append(p)

    bad = 0
    for p in sorted(lua_files):
        ok, err = check_lua(p)
        print(("[OK ] " if ok else "[ERR] ") + os.path.relpath(p, root))
        if not ok:
            print("      " + err.replace("\n", "\n      "))
            bad += 1
    for p in sorted(xml_files):
        ok, err = check_xml(p)
        print(("[OK ] " if ok else "[ERR] ") + os.path.relpath(p, root))
        if not ok:
            print("      " + err)
            bad += 1

    print()
    if bad:
        print(f"{len(lua_files)} lua + {len(xml_files)} xml checked, {bad} problem(s)")
    else:
        print(f"all {len(lua_files)} lua + {len(xml_files)} xml OK")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
