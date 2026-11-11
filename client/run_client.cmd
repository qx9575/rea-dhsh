@echo off
setlocal
if "%~1"=="" (
    echo Usage: %~nx0 "D:\path\to\shdata"
    exit /b 2
)
if not exist "%~dp0map-cache\client_map_cache.json" (
    echo Missing map cache: %~dp0map-cache
    exit /b 1
)
"%~dp0dhsh_client.exe" --shdata "%~1" --cache "%~dp0map-cache" --log "%~dp0client.log"
