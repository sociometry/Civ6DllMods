import re, subprocess, os

HOOKAPI = r"E:\WorkSpace\Civ6\Civ6DllMods\recipes\Global\Civ6BasePlugin\dll\HookAPI.h"
RUN_DEPLOYED = r"E:\Sumcam\Documents\My Games\Sid Meier's Civilization VI\Mods\FourRing\Binaries\Win64\Civ6DllLoader.runtime.dll"
RUN_BUILT = r"E:\WorkSpace\Civ6\Civ6DllLoader\x64\Release\Civ6DllLoader.runtime.dll"

src = open(HOOKAPI, encoding="utf-8").read()
entries = re.findall(r"^\s*X\((\w+),", src, re.M)
print("HookAPI.h entries:", len(entries))

def strings_of(path):
    data = open(path, "rb").read()
    # crude ASCII string extraction
    out = set()
    cur = bytearray()
    for b in data:
        if 32 <= b < 127:
            cur.append(b)
        else:
            if len(cur) >= 4:
                out.add(cur.decode("ascii"))
            cur = bytearray()
    if len(cur) >= 4:
        out.add(cur.decode("ascii"))
    return out

for label, path in (("DEPLOYED", RUN_DEPLOYED), ("BUILT", RUN_BUILT)):
    if not os.path.exists(path):
        print(label, "missing")
        continue
    ss = strings_of(path)
    missing = [e for e in entries if e not in ss]
    print(f"{label}: size={os.path.getsize(path)} entries_present={len(entries)-len(missing)}/{len(entries)}")
    if missing:
        print("   MISSING:", ", ".join(missing))
