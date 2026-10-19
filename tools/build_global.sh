#!/usr/bin/env bash
set -euo pipefail
MSBUILD="E:/VisualStudio/Products/MSBuild/Current/Bin/amd64/MSBuild.exe"
cd "E:/WorkSpace/Civ6/Civ6DllMods"
"$MSBUILD" "recipes/Global/Civ6BasePlugin/dll/Civ6BasePlugin_Global.vcxproj" -p:Configuration=Release -p:Platform=x64 -nologo -v:minimal
