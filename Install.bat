@echo off
title Installing Archive Previewer & Thumbnail Provider
echo ============================================================
echo Requesting Administrator privileges to install...
echo ============================================================
powershell -NoProfile -ExecutionPolicy Bypass -Command "& {Start-Process powershell -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File \"%~dp0register_hklm.ps1\"' -Verb RunAs}"
echo Installation trigger sent. Please check the UAC pop-up on your screen.
pause
