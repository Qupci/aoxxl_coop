@echo off
REM Builds xxl_timerfix.asi (32-bit), separate from the co-op mod.
REM Requires the VS 2022 Build Tools x86 toolchain.

setlocal
set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat
if not exist "%VCVARS%" (
    echo ERROR: vcvars32.bat not found at "%VCVARS%"
    exit /b 1
)
call "%VCVARS%" >nul || exit /b 1

set ROOT=%~dp0
set OUT=%ROOT%build
if not exist "%OUT%" mkdir "%OUT%"
pushd "%OUT%" || exit /b 1

cl /nologo /O2 /MT /W3 /LD "%ROOT%timerfix.cpp" /Fe:xxl_timerfix.asi /link kernel32.lib
if errorlevel 1 goto :fail

popd
echo.
echo Built %OUT%\xxl_timerfix.asi
exit /b 0

:fail
popd
echo.
echo BUILD FAILED
exit /b 1
