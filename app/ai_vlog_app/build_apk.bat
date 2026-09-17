@echo off
rem One-click launcher: builds release APK via build_apk.ps1 (bypasses PowerShell execution policy)
rem Output: AI-Vlog.apk next to this file. See build_apk.ps1 header for requirements.
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "build_apk.ps1"
echo.
pause
