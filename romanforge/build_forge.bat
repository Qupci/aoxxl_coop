@echo off
REM Builds romanforge.lib (linked into xxl_coop.asi by ..\build.bat) and the
REM romanforge.exe test tool from the vendored XXL-Editor core (xec\, MIT).
setlocal
set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat
call "%VCVARS%" >nul || exit /b 1
set ROOT=%~dp0
set OUT=%ROOT%build
if not exist "%OUT%" mkdir "%OUT%"
pushd "%OUT%" || exit /b 1
del /q *.obj 2>nul
cl /nologo /c /O2 /Zi /Fdromanforge.pdb /MT /EHsc /W0 /std:c++20 /bigobj /Zc:char8_t- /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /DVector3=XecVector3 /DCNode=XecCNode /DCKService=XecCKService ^
   /I"%ROOT%shim" /I"%ROOT%xec" /FIforge_force.h ^
   "%ROOT%forge_lib.cpp" "%ROOT%xec\stubs.cpp" ^
   "%ROOT%xec\CKLocalObjectSubs.cpp" "%ROOT%xec\CKUtils.cpp" "%ROOT%xec\ClassRegister.cpp" "%ROOT%xec\Duplicator.cpp" ^
   "%ROOT%xec\Events.cpp" "%ROOT%xec\File.cpp" "%ROOT%xec\KEnvironment.cpp" "%ROOT%xec\KLocalObject.cpp" ^
   "%ROOT%xec\KObject.cpp" "%ROOT%xec\rw.cpp" "%ROOT%xec\rwext.cpp" "%ROOT%xec\rwsound.cpp" "%ROOT%xec\Shape.cpp" ^
   "%ROOT%xec\vecmat.cpp" "%ROOT%xec\FatalError.cpp" "%ROOT%xec\GamePatcher.cpp" "%ROOT%xec\GamePatcherKClasses.cpp" ^
   "%ROOT%xec\CoreClasses\*.cpp" "%ROOT%xec\GameClasses\*.cpp"
if errorlevel 1 goto :fail
lib /nologo /OUT:romanforge.lib *.obj
if errorlevel 1 goto :fail
cl /nologo /c /O2 /Zi /Fdromanforge_exe.pdb /MT /EHsc /W0 /std:c++20 /Zc:char8_t- /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /DVector3=XecVector3 /DCNode=XecCNode /DCKService=XecCKService ^
   /I"%ROOT%shim" /I"%ROOT%xec" /FIforge_force.h /Foforge_main.o "%ROOT%forge_main.cpp"
if errorlevel 1 goto :fail
link /nologo /DEBUG /OUT:romanforge.exe forge_main.o romanforge.lib kernel32.lib user32.lib
if errorlevel 1 goto :fail
popd
echo Built %OUT%\romanforge.lib and romanforge.exe
exit /b 0
:fail
popd
echo BUILD FAILED
exit /b 1
