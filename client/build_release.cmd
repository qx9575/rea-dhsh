@echo off
setlocal
set "CLIENT_DIR=%~dp0"
set "CLIENT_DIR=%CLIENT_DIR:~0,-1%"
cmake -S "%CLIENT_DIR%" -B "%CLIENT_DIR%\build" -G "Visual Studio 17 2022" -A Win32
if errorlevel 1 exit /b %errorlevel%
cmake --build "%CLIENT_DIR%\build" --config Release
exit /b %errorlevel%
