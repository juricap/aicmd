@echo off
setlocal
set "VSROOT=C:\Program Files\Microsoft Visual Studio\2022\Community"
if not exist "%VSROOT%\Common7\Tools\VsDevCmd.bat" (
  echo Visual Studio 2022 C++ tools not found. Use CMake from a developer prompt.
  exit /b 1
)
call "%VSROOT%\Common7\Tools\VsDevCmd.bat" -arch=x64 >nul
set "CMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
"%CMAKE%" -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1
"%CMAKE%" --build build
if errorlevel 1 exit /b 1
"%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir build --output-on-failure
exit /b %errorlevel%
