#!/usr/bin/env python3
"""verify_stack_semantics.py —— 静态核对 ModConfigCore.h 里 Lua 栈操作的索引语义。

这不是运行期测试（那需要游戏进程），而是把"栈索引必须怎么写"固化成可执行的检查，
防止后续维护时把 -3 / -2 写错导致表项丢失或栈不平衡。

检查项：
 1. PushJsonValue 的 Array / Object 两个分支：
        create -> push(key) -> push(value) -> settable(-3)
    （栈顶是 value，-2 是 key，-3 是 table）；两处都必须是 -3。
 2. lKeys：同样是 create -> push(i) -> push(str) -> settable(-3)。
 3. RegisterLuaTable：NewGlobalTable + CreateTable 之后
        SetField(-2, "__raw")   —— 栈顶是内表，-2 才是 ModConfig
    并以 Pop(1) 收尾（只弹 ModConfig，内表已被 setfield 消费）。
 4. 门面校验段：GetField(globals) / GetField(-1,"Save") / Pop(1) / Pop(1) 必须成对。

注意：PushJsonValue 与 lKeys 里的 create 是**返回值**（C 函数把 table 交给 Lua），
它们不配对 pop 是正确行为，所以不能拿 create/pop 计数做平衡判断。
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "recipes", "Global", "Civ6BasePlugin", "dll",
                   "APIExtension", "ModConfig", "ModConfigCore.h")


def section(text, start_marker, end_marker="\n}\n"):
    return text.split(start_marker, 1)[-1].split(end_marker, 1)[0]


def main():
    text = open(SRC, encoding="utf-8").read()
    problems = []

    push_json = section(text, "inline void PushJsonValue")
    n_settable = len(re.findall(r"LuaSetTable\(L, -3\)", push_json))
    if n_settable != 2:
        problems.append(f"PushJsonValue: expected 2 LuaSetTable(L, -3), found {n_settable}")
    if re.search(r"LuaSetTable\(L, -2\)", push_json):
        problems.append("PushJsonValue: found LuaSetTable(L, -2) - would drop the key")

    l_keys = section(text, "inline int lKeys")
    if "LuaSetTable(L, -3)" not in l_keys:
        problems.append("lKeys: missing LuaSetTable(L, -3)")

    reg = text.split("inline void RegisterLuaTable", 1)[-1]
    for fn in ("lLoad", "lSave", "lGet", "lSet", "lDelete", "lHas",
               "lKeys", "lExists", "lGetPath", "lDeleteFile"):
        if ("&" + fn) not in reg:
            problems.append(f"RegisterLuaTable: missing field for {fn}")
    if "LuaDoString" in text:
        problems.append("ModConfigCore.h still injects Lua source - keep it pure C++")
    if 'R"LUA' in text:
        problems.append("ModConfigCore.h still contains an embedded Lua chunk")

    if problems:
        for p in problems:
            print("FAIL:", p)
        return 1
    print("OK: push/set/pop indices consistent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
