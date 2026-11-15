@echo off
REM Builds xxl_coop.asi (32-bit, the game is a 2004 x86 binary).
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
del /q *.obj 2>nul
REM Roman pools (levelforge.cpp): XXL-Editor's level code, built once (romanforge\build_forge.bat).
if not exist "%ROOT%romanforge\build\romanforge.lib" call "%ROOT%romanforge\build_forge.bat" || goto :fail

cl /nologo /c /O2 /MT /EHsc /W3 /std:c++17 /D_CRT_SECURE_NO_WARNINGS ^
   "%ROOT%src\main.cpp" "%ROOT%src\kal.cpp" "%ROOT%src\coop.cpp" "%ROOT%src\hooks.cpp" ^
   "%ROOT%src\life.cpp" "%ROOT%src\trio.cpp" "%ROOT%src\pads.cpp" "%ROOT%src\inputmgr.cpp" ^
   "%ROOT%src\watchdog.cpp" "%ROOT%src\vtprofile.cpp" "%ROOT%src\detour.cpp" "%ROOT%src\p2input.cpp" ^
   "%ROOT%src\render.cpp" "%ROOT%src\autotest.cpp" "%ROOT%src\shot.cpp" "%ROOT%src\config.cpp" ^
   "%ROOT%src\log.cpp" "%ROOT%src\padident.cpp" "%ROOT%src\crash.cpp" "%ROOT%src\splitscreen.cpp" "%ROOT%src\p2cam.cpp" "%ROOT%src\gamecam.cpp" "%ROOT%src\proximity.cpp" "%ROOT%src\trace.cpp" "%ROOT%src\klusters.cpp" "%ROOT%src\sound.cpp" "%ROOT%src\shared.cpp" "%ROOT%src\hints.cpp" "%ROOT%src\individual.cpp" "%ROOT%src\hud2.cpp" "%ROOT%src\experimental.cpp" "%ROOT%src\toughness.cpp" "%ROOT%src\potioncombos.cpp" "%ROOT%src\pathfind.cpp" "%ROOT%src\icons.cpp" "%ROOT%src\nolock.cpp" "%ROOT%src\guard.cpp" "%ROOT%src\combohints.cpp" "%ROOT%src\tint.cpp" "%ROOT%src\cutskip.cpp" "%ROOT%src\levelforge.cpp" "%ROOT%src\ctrlopts.cpp" "%ROOT%src\coopopts.cpp" "%ROOT%src\loc.cpp"
if errorlevel 1 goto :fail

link /nologo /DLL /OUT:xxl_coop.asi *.obj "%ROOT%romanforge\build\romanforge.lib" kernel32.lib user32.lib gdi32.lib gdiplus.lib psapi.lib
if errorlevel 1 goto :fail

popd
echo.
echo Built %OUT%\xxl_coop.asi
exit /b 0

:fail
popd
echo.
echo BUILD FAILED
exit /b 1
