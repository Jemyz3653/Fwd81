@echo off
rem SPDX-License-Identifier: MIT
rem
rem Fwd81 build shim. All logic and all messages live in build.ps1;
rem this file exists so the project can be built by a double click.
chcp 65001 >nul
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
exit /b %ERRORLEVEL%
