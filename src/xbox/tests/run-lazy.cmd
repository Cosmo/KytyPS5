@echo off
rem Compiles and runs the standalone test of the lazy region bookkeeping (needs Visual Studio's clang-cl).
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "HERE=%~dp0"
set "OUT=%TEMP%\lazyRegionsTest"
if not exist "%OUT%" mkdir "%OUT%"
clang-cl /nologo /std:c++20 /EHsc /O2 /W3 /I "%HERE%..\.." "%HERE%lazyRegionsTest.cpp" /Fo"%OUT%\\" /Fe"%OUT%\lazyRegionsTest.exe" || exit /b 1
"%OUT%\lazyRegionsTest.exe"
