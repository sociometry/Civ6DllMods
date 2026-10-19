import os, struct, sys

BASE = r"E:\Sumcam\Documents\My Games\Sid Meier's Civilization VI\Mods\FourRing\Binaries\Win64"
PLUGINS = {
    "Global/Civ6BasePlugin.dll": 1,
    "GameCore/Civ6BasePlugin_GameCore.dll": 2,
    "GameCore/FourRing.dll": 2,
}

def read_pe(path):
    data = open(path, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    machine = struct.unpack_from("<H", data, pe + 4)[0]
    magic = struct.unpack_from("<H", data, pe + 24)[0]
    return data, machine, magic

def find_export(data, name):
    """极简 PE 导出表查找：返回 RVA 或 None。"""
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    optsize = struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    if magic == 0x20B:      # PE32+
        ddir = opt + 112
    else:
        ddir = opt + 96
    exp_rva, exp_size = struct.unpack_from("<II", data, ddir)
    if exp_rva == 0:
        return None
    numsec = struct.unpack_from("<H", data, pe + 6)[0]
    sec = opt + optsize
    def rva2off(rva):
        for i in range(numsec):
            off = sec + i * 40
            vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, off + 8)
            if vaddr <= rva < vaddr + max(vsize, rawsize):
                return rawptr + (rva - vaddr)
        return None
    eo = rva2off(exp_rva)
    if eo is None:
        return None
    nNames = struct.unpack_from("<I", data, eo + 24)[0]
    namesRva = struct.unpack_from("<I", data, eo + 32)[0]
    funcsRva = struct.unpack_from("<I", data, eo + 28)[0]
    ordinalsRva = struct.unpack_from("<I", data, eo + 36)[0]
    no = rva2off(namesRva)
    fo = rva2off(funcsRva)
    oo = rva2off(ordinalsRva)
    for i in range(nNames):
        nr = struct.unpack_from("<I", data, no + i * 4)[0]
        sOff = rva2off(nr)
        end = data.index(b"\x00", sOff)
        s = data[sOff:end].decode("ascii", "replace")
        if s == name:
            idx = struct.unpack_from("<H", data, oo + i * 2)[0]
            return struct.unpack_from("<I", data, fo + idx * 4)[0]
    return None

bad = 0
for rel, expectKind in PLUGINS.items():
    path = os.path.join(BASE, "Plugins", rel.replace("/", os.sep))
    if not os.path.exists(path):
        print(f"[MISS] {rel}")
        bad += 1
        continue
    data, machine, magic = read_pe(path)
    onload = find_export(data, "OnLoad")
    kind = find_export(data, "Civ6PluginKind")
    ok64 = machine == 0x8664 and magic == 0x20B
    entry_ok = onload is not None
    print(f"[{'OK ' if (ok64 and entry_ok) else 'ERR'}] {rel}")
    print(f"       x64={ok64} OnLoad={'yes' if onload else 'no'} "
          f"Civ6PluginKind={'yes' if kind else 'no'} "
          f"(expect kind={expectKind})")
    if not (ok64 and entry_ok):
        bad += 1

rt = os.path.join(BASE, "Civ6DllLoader.runtime.dll")
print(f"[{'OK ' if os.path.exists(rt) else 'ERR'}] Civ6DllLoader.runtime.dll exists")
print()
print("ALL OK" if bad == 0 else f"{bad} problem(s)")
sys.exit(1 if bad else 0)
