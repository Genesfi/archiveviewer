@echo off
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"

if not exist "%VCVARS%" (
    echo Error: Visual Studio 2022 Community compiler tools not found at:
    echo "%VCVARS%"
    exit /b 1
)

echo Initializing MSVC compiler environment...
call "%VCVARS%" x64

echo.
echo ====================================================
echo 0. Compiling Resources...
echo ====================================================
rc.exe src\resource.rc

echo.
echo ====================================================
echo 1. Compiling Archive Thumbnail Provider DLL...
echo ====================================================
cl.exe /EHsc /std:c++17 /O2 /DUNICODE /D_UNICODE /LD /Fe:ArchiveThumbnailProvider.dll src/dllmain.cpp src/archive_reader.cpp src/miniz.c src/miniz_tdef.c src/miniz_tinfl.c src/miniz_zip.c src\resource.res user32.lib gdi32.lib gdiplus.lib ole32.lib shell32.lib advapi32.lib /link /def:src/exports.def

echo.
echo ====================================================
echo 2. Compiling Archive Previewer App EXE...
echo ====================================================
cl.exe /EHsc /std:c++17 /O2 /DUNICODE /D_UNICODE /Fe:ArchivePreviewer.exe src\main.cpp src\archive_reader.cpp src\miniz.c src\miniz_tdef.c src\miniz_tinfl.c src\miniz_zip.c src\resource.res user32.lib gdi32.lib gdiplus.lib ole32.lib shell32.lib comctl32.lib advapi32.lib /link /SUBSYSTEM:WINDOWS


if %ERRORLEVEL% equ 0 (
    echo.
    echo Build SUCCESS! Generated ArchiveThumbnailProvider.dll and ArchivePreviewer.exe
) else (
    echo.
    echo Build FAILED!
)
