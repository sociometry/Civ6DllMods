@echo off
call "E:\VisualStudio\Products\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cl /nologo /EHsc /std:c++20 /utf-8 /W3 /DCIV6_PLUGIN_GLOBAL /I"E:\WorkSpace\Civ6\Civ6DllMods\recipes\Global\Civ6BasePlugin\dll" /I"E:\WorkSpace\Civ6\Civ6DllMods\recipes\Global\Civ6BasePlugin\dll\APIExtension\ModConfig" "E:\WorkSpace\Civ6\Civ6DllMods\tools\modconfig_test\main.cpp" /Fe:"E:\WorkSpace\Civ6\Civ6DllMods\tools\modconfig_test\modconfig_test.exe" /Fo:"E:\WorkSpace\Civ6\Civ6DllMods\tools\modconfig_test\\" shell32.lib ole32.lib
