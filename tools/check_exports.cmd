@echo off
call "E:\VisualStudio\Products\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
dumpbin /exports "E:\WorkSpace\Civ6\Civ6DllMods\x64\Release\Civ6BasePlugin_GameCore.dll" | findstr /i "OnLoad OnUnload Civ6PluginKind"
echo ---GLOBAL---
dumpbin /exports "E:\WorkSpace\Civ6\Civ6DllMods\x64\Release\Civ6BasePlugin_Global.dll" | findstr /i "OnLoad OnUnload Civ6PluginKind"
