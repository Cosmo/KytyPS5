@echo off
rem Runs the lazy region test unchanged and with three planted bugs (it must pass the first and fail the other three).
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "SRC=%~dp0..\.."
set "OUT=%TEMP%\lazyMut"
if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%\xbox"
call :run NONE ""
call :run NO_APPLY_MODES "s/host.Protect(run_start, p - run_start, run_mode);//"
call :run NO_DECOMMIT "s/host.Decommit(run_start, run_end - run_start);//g"
call :run BOUNDARY_UNCOMMITTED "s/r.committed\[w - (start >> kWindowBits)\] = old.committed\[w - (old.start >> kWindowBits)\];/r.committed[w - (start >> kWindowBits)] = 0;/"
exit /b 0
:run
echo == %1
copy /y "%SRC%\xbox\lazyRegions.h" "%OUT%\xbox\lazyRegions.h" >nul
if not "%~2"=="" "C:\Program Files\Git\usr\bin\sed.exe" -i "%~2" "%OUT%\xbox\lazyRegions.h"
clang-cl /nologo /std:c++20 /EHsc /O2 /I "%OUT%" "%SRC%\xbox\tests\lazyRegionsTest.cpp" /Fo"%OUT%\\" /Fe"%OUT%\t.exe" >nul 2>&1 || (echo compile failed & exit /b 0)
"%OUT%\t.exe" > "%OUT%\out.txt"
echo exit code %ERRORLEVEL%
exit /b 0
