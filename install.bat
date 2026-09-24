@echo off
rem Runs the Salivo SDK installer (install.ps1). "Salivo Setup.exe" does the same.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
pause
