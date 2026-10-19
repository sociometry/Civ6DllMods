@echo off
call "E:\VisualStudio\Products\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cl /nologo /EHsc /std:c++20 /utf-8 /W4 "E:\WorkSpace\Civ6\Civ6DllMods\tools\aob_pattern_test\main.cpp" /Fe:"E:\WorkSpace\Civ6\Civ6DllMods\tools\aob_pattern_test\aob_pattern_test.exe" /Fo:"E:\WorkSpace\Civ6\Civ6DllMods\tools\aob_pattern_test\\"
