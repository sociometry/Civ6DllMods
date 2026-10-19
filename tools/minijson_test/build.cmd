@echo off
call "E:\VisualStudio\Products\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cl /nologo /EHsc /std:c++20 /utf-8 /W4 /I"E:\WorkSpace\Civ6\Civ6DllMods\recipes\Global\Civ6BasePlugin\dll\APIExtension\ModConfig" "E:\WorkSpace\Civ6\Civ6DllMods\tools\minijson_test\main.cpp" /Fe:"E:\WorkSpace\Civ6\Civ6DllMods\tools\minijson_test\minijson_test.exe" /Fo:"E:\WorkSpace\Civ6\Civ6DllMods\tools\minijson_test\\"
