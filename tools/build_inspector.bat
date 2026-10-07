@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "F:\XXL COOP MOD\xxl-inspector"
del /q *.obj 2>nul
set XXLVER=1
set PATCHVER=0,2,0,0
set PATCHVERSTR="0.2.0"
rc /nologo /dPATCHVER=%PATCHVER% /dXXLVER=%XXLVER% resource.rc || exit /b 1
cl /nologo /O2 /MT /c /DPATCHVERSTR=%PATCHVERSTR% /DXXLVER=%XXLVER% /LD /EHsc *.cpp imgui/imgui.cpp imgui/imgui_draw.cpp imgui/imgui_widgets.cpp imgui/imgui_demo.cpp imgui/imgui_impl_dx9.cpp imgui/imgui_impl_win32.cpp >build.log 2>&1 || (type build.log & exit /b 1)
link /nologo /DLL /DEF:d3d9.def /OUT:bin\apd3d9_xxl1_behind_qol.dll *.obj resource.res user32.lib version.lib || exit /b 1
del /q *.obj resource.res 2>nul
copy /y bin\apd3d9_xxl1_behind_qol.dll "F:\XXL COOP MOD\Game Files\apd3d9.dll" >nul
echo inspector built
