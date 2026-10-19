#!/usr/bin/env bash
# deploy_fourring.sh -- 构建 BasePlugin 并部署到 FourRing，然后校验。
#
# 用法：
#   bash tools/deploy_fourring.sh            # 构建 + 部署 + 校验
#   bash tools/deploy_fourring.sh --check    # 只校验现有部署
#   bash tools/deploy_fourring.sh --no-build # 用现有产物部署（不重新编译）
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOADER="/e/WorkSpace/Civ6/Civ6DllLoader"

# Documents 可能被重定向（本机就在 E:\Sumcam\Documents），
# 必须问注册表而不是拼 $USERPROFILE/Documents。
DOCS_WIN="$(python -c "import winreg;k=winreg.OpenKey(winreg.HKEY_CURRENT_USER,r'Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders');print(winreg.QueryValueEx(k,'Personal')[0])")"
MOD="$(cygpath -u "$DOCS_WIN")/My Games/Sid Meier's Civilization VI/Mods/FourRing"

MODE="${1:-}"

if [[ "$MODE" != "--check" ]]; then
    if [[ "$MODE" != "--no-build" ]]; then
        echo "=== 构建 Global 插件 ==="
        bash "$ROOT/tools/build_global.sh" | tail -2
        echo "=== 构建 GameCore 插件 ==="
        bash "$ROOT/tools/build_gamecore.sh" | tail -2
    fi

    if [[ ! -d "$MOD" ]]; then
        echo "错误：找不到 Mod 目录 $MOD" >&2
        exit 1
    fi

    TS="$(date +%Y%m%d_%H%M%S)"
    BIN="$MOD/Binaries/Win64"

    echo "=== 备份被覆盖的文件 (.bak_$TS) ==="
    for f in "$BIN/Civ6DllLoader.runtime.dll" \
             "$BIN/Plugins/Global/Civ6BasePlugin.dll" \
             "$BIN/Plugins/GameCore/Civ6BasePlugin_GameCore.dll"; do
        if [[ -f "$f" ]]; then
            cp "$f" "$f.bak_$TS"
            echo "  $(basename "$f")"
        fi
    done

    echo "=== 复制产物 ==="
    cp "$LOADER/x64/Release/Civ6DllLoader.runtime.dll"  "$BIN/Civ6DllLoader.runtime.dll"
    cp "$ROOT/x64/Release/Civ6BasePlugin_Global.dll"    "$BIN/Plugins/Global/Civ6BasePlugin.dll"
    cp "$ROOT/x64/Release/Civ6BasePlugin_GameCore.dll"  "$BIN/Plugins/GameCore/Civ6BasePlugin_GameCore.dll"
    echo "  runtime + Global + GameCore done"
fi

echo
echo "=== 校验：插件 PE / 导出 / 架构 ==="
python "$ROOT/tools/verify_deployment.py"

echo
echo "=== 校验：runtime 接口覆盖 ==="
python "$ROOT/tools/check_runtime_api.py"

echo
echo "=== 校验：Lua / XML 语法 ==="
python "$ROOT/tools/check_mod_lua.py" "$DOCS_WIN/My Games/Sid Meier's Civilization VI/Mods/FourRing"
