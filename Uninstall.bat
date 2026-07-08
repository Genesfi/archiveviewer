@echo off
title Uninstalling Archive Previewer & Thumbnail Provider
echo ============================================================
echo Requesting Administrator privileges to uninstall...
echo ============================================================
powershell -NoProfile -ExecutionPolicy Bypass -Command "& {Start-Process powershell -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File \"%~dp0unregister_hklm.ps1\"' -Verb RunAs}"
echo Uninstall trigger sent. Please check the UAC pop-up on your screen.
pause
