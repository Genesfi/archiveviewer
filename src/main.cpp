#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0600
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <gdiplus.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <commctrl.h>
#include <filesystem>
#include <vector>
#include <string>
#include <algorithm>
#include "archive_reader.h"
#include "resource.h"

// Enable visual styles (Common Controls v6)
#pragma comment(linker,"\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// Link required libraries
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

using namespace Gdiplus;
namespace fs = std::filesystem;

// Control IDs
#define IDC_PATH_EDIT     101
#define IDC_BROWSE_BTN    102
#define IDC_EXTRACT_BTN   103
#define IDC_ARCHIVE_LIST  104
#define IDC_CONTENTS_LIST 105
#define IDC_SETTINGS_BTN  106

// Custom window message for background scanning notifications
#define WM_USER_SCAN_ITEM       (WM_USER + 101)
#define WM_USER_METADATA_LOADED (WM_USER + 103)
#define WM_USER_NEED_PASSWORD   (WM_USER + 104)

#define IDC_VIEW_TOGGLE_BTN 107
#define IDC_GRID_VIEW_WND   108

// Struct to store parsed archive info and cached thumbnail
struct ArchiveItem {
    std::wstring filePath;
    std::wstring fileName;
    Gdiplus::Bitmap* thumbnail; // nullptr if no image inside
    std::vector<ArchiveFileInfo> internalFiles;
    bool isEncrypted;
    std::wstring currentPassword;
    std::wstring tempExtractDir; // Path to temp folder if already extracted, else L""
    bool extractionFinished;    // True if background ExtractAll completed
    bool metadataLoaded;        // True if metadata scanning is complete (success/empty/fail)
};

// Global variables
std::vector<ArchiveItem> g_archives;
std::wstring g_currentFolder;
HWND g_hPathEdit = NULL;
HWND g_hBrowseBtn = NULL;
HWND g_hExtractBtn = NULL;
HWND g_hArchiveList = NULL;
HWND g_hContentsList = NULL;
HBRUSH g_hDarkBgBrush = NULL;
HBRUSH g_hPanelBgBrush = NULL;

// Splitter and View Mode Globals
int g_splitPos = 350;
bool g_bDraggingSplitter = false;
bool g_bGridView = false;
HWND g_hGridViewWnd = NULL;
HWND g_hViewToggleBtn = NULL;
int g_priorityIndex = -1;

CRITICAL_SECTION g_csArchives;
HWND g_hMainWnd = NULL;
HANDLE g_hScanThread = NULL;
bool g_bCancelScan = false;

// Extraction globals
CRITICAL_SECTION g_csTempExtract;
std::vector<std::wstring> g_tempDirsCreated; // Keep track of all created temp folders to delete them on exit

// Grid loader globals
CRITICAL_SECTION g_csGridLoad;
HANDLE g_hGridLoadThread = NULL;
bool g_bCancelGridLoad = false;
int g_gridLoadSessionId = 0;
volatile bool g_bPauseBackgroundScan = false;
int g_metadataSessionId = 0;

// Grid item cache - declared here for forward use in LoadMetadataThread
struct GridItemCache {
    std::wstring name;
    std::string internalPath;
    Gdiplus::Bitmap* pBitmap;
    bool loadAttempted;
};
CRITICAL_SECTION g_csGridItems;
std::vector<GridItemCache> g_gridItems;

static void LogDebug(const char* format, ...) {
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    std::wstring logPath = std::wstring(tempPath) + L"ArchivePreviewerDebug.txt";
    FILE* f = _wfopen(logPath.c_str(), L"a");
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "[APP %02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        va_list args;
        va_start(args, format);
        vfprintf(f, format, args);
        va_end(args);
        fprintf(f, "\n");
        fclose(f);
    }
}

// Function declarations
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
std::wstring ChooseFolder(HWND hwndParent);
void ScanDirectory(HWND hWnd, const std::wstring& folderPath);
void ClearArchives();
void UpdateContentsList(int archiveIndex);
void ExtractSelectedArchive(int index);
bool IsImageFile(const std::wstring& filename);
Bitmap* LoadThumbnailFromMemory(const char* data, size_t size, int thumbWidth, int thumbHeight);
Bitmap* LoadThumbnailFromFile(const wchar_t* filePath, int thumbWidth, int thumbHeight);
Bitmap* CreateThumbnail(Bitmap* pSource, int thumbWidth, int thumbHeight);
INT_PTR ShowPasswordPromptDialog(HWND hwndParent, const std::wstring& archivePath, std::wstring& outPassword, bool& outSavePassword);
void ShowSettingsDialog(HWND hwndParent);
bool OpenArchiveWithPasswordSupport(ArchiveReader& reader, const std::wstring& archivePath, HWND hwndParent, std::wstring& outPassword);
void UpdateGridItems(const std::vector<ArchiveFileInfo>& files, const std::wstring& archivePath, const std::wstring& password);



// Struct for background thread parameters
struct LoadMetadataParams {
    HWND hWnd;
    int archiveIndex;
    std::wstring filePath;
    int sessionId;
};

DWORD WINAPI LoadMetadataThread(LPVOID lpParam) {
    LoadMetadataParams* params = (LoadMetadataParams*)lpParam;
    HWND hWnd = params->hWnd;
    int archiveIndex = params->archiveIndex;
    std::wstring filePath = params->filePath;
    int sessionId = params->sessionId;
    
    std::wstring savedPwd;
    EnterCriticalSection(&g_csArchives);
    if (archiveIndex >= 0 && archiveIndex < (int)g_archives.size()) {
        savedPwd = g_archives[archiveIndex].currentPassword;
    }
    LeaveCriticalSection(&g_csArchives);
    delete params;

    LogDebug("LoadMetadataThread: Started for index %d, session %d", archiveIndex, sessionId);

    // Pause background scanner to allocate all resources to this opening process
    g_bPauseBackgroundScan = true;

    ArchiveReader reader;
    bool opened = false;
    
    // Try opening, retry up to 5 times (100ms delay) to avoid sharing conflicts
    for (int retry = 0; retry < 5; ++retry) {
        if (reader.Open(filePath, savedPwd)) {
            opened = true;
            break;
        } else if (reader.IsEncrypted()) {
            EnterCriticalSection(&g_csGridLoad);
            bool sessionValid = (g_metadataSessionId == sessionId);
            LeaveCriticalSection(&g_csGridLoad);
            
            LogDebug("LoadMetadataThread: Encrypted. sessionValid=%d", sessionValid);
            if (sessionValid) {
                PostMessageW(hWnd, WM_USER_NEED_PASSWORD, (WPARAM)archiveIndex, (LPARAM)sessionId);
            }
            g_bPauseBackgroundScan = false;
            return 0;
        }
        Sleep(100);
    }

    LogDebug("LoadMetadataThread: Open returned %d", opened);

    std::vector<ArchiveFileInfo> files;
    bool isEncrypted = false;
    Gdiplus::Bitmap* thumbnail = nullptr;

    if (opened) {
        files = reader.ListFiles();
        isEncrypted = reader.IsEncrypted();

        // Extract thumbnail of first image file synchronously in this thread
        std::string firstImageInternalPath;
        for (const auto& file : files) {
            if (!file.isDirectory && IsImageFile(file.name)) {
                firstImageInternalPath = file.internalPath;
                break;
            }
        }
        if (!firstImageInternalPath.empty()) {
            std::vector<char> buffer;
            if (reader.ExtractFileToMemory(firstImageInternalPath, buffer)) {
                thumbnail = LoadThumbnailFromMemory(buffer.data(), buffer.size(), 64, 64);
            }
        }
        reader.Close();
    }

    EnterCriticalSection(&g_csArchives);
    EnterCriticalSection(&g_csGridLoad);
    bool sessionValid = (g_metadataSessionId == sessionId);
    LeaveCriticalSection(&g_csGridLoad);

    LogDebug("LoadMetadataThread: Finished. sessionValid=%d, files=%u", sessionValid, (unsigned int)files.size());

    if (sessionValid && archiveIndex >= 0 && archiveIndex < (int)g_archives.size() && g_archives[archiveIndex].filePath == filePath) {
        g_archives[archiveIndex].metadataLoaded = true;
        if (!files.empty() || g_archives[archiveIndex].internalFiles.empty()) {
            g_archives[archiveIndex].internalFiles = files;
            g_archives[archiveIndex].isEncrypted = isEncrypted;
            if (thumbnail) {
                if (g_archives[archiveIndex].thumbnail) delete g_archives[archiveIndex].thumbnail;
                g_archives[archiveIndex].thumbnail = thumbnail;
            }
        }
        PostMessageW(hWnd, WM_USER_METADATA_LOADED, (WPARAM)archiveIndex, (LPARAM)sessionId);
    } else {
        if (thumbnail) delete thumbnail;
    }
    LeaveCriticalSection(&g_csArchives);

    g_bPauseBackgroundScan = false;
    return 0;
}

struct ScanThreadParams {
    HWND hWnd;
    std::wstring folderPath;
    std::vector<std::wstring> filePaths;
};

// Background thread function
DWORD WINAPI ScanDirectoryThread(LPVOID lpParam) {
    ScanThreadParams* params = (ScanThreadParams*)lpParam;
    HWND hWnd = params->hWnd;
    std::wstring folderPath = params->folderPath;
    std::vector<std::wstring> filePaths = params->filePaths;
    delete params;

    std::vector<bool> processed(filePaths.size(), false);
    size_t processedCount = 0;

    while (processedCount < filePaths.size()) {
        // If foreground grid loader is active, pause background scan to prioritize it
        while (g_bPauseBackgroundScan) {
            Sleep(150);
            bool shouldExitPause = false;
            EnterCriticalSection(&g_csArchives);
            if (g_bCancelScan || g_currentFolder != folderPath) {
                shouldExitPause = true;
            }
            LeaveCriticalSection(&g_csArchives);
            if (shouldExitPause) break;
        }

        // Determine which index to scan next
        int idx = -1;
        EnterCriticalSection(&g_csArchives);
        if (g_bCancelScan || g_currentFolder != folderPath) {
            // will exit below
        } else {
            // Try priority index first
            if (g_priorityIndex >= 0 && g_priorityIndex < (int)filePaths.size() && !processed[g_priorityIndex]) {
                idx = g_priorityIndex;
            } else {
                // Otherwise find the first unprocessed index
                for (size_t k = 0; k < filePaths.size(); ++k) {
                    if (!processed[k]) {
                        idx = (int)k;
                        break;
                    }
                }
            }
        }
        LeaveCriticalSection(&g_csArchives);

        if (idx == -1) {
            // Either cancelled or all processed
            break;
        }

        // Mark as processed
        processed[idx] = true;
        processedCount++;

        // Stop scanning if cancelled, folder has changed, or app is closing
        bool shouldExit = false;
        bool shouldSkip = false;
        EnterCriticalSection(&g_csArchives);
        if (g_bCancelScan || g_currentFolder != folderPath) {
            shouldExit = true;
        } else if (idx < (int)g_archives.size() && g_archives[idx].metadataLoaded) {
            shouldSkip = true;
        }
        LeaveCriticalSection(&g_csArchives);
        if (shouldExit) break;
        if (shouldSkip) continue;

        std::wstring filePath = filePaths[idx];
        
        ArchiveReader reader;
        reader.SetCancelFlag(&g_bCancelScan);
        bool isEncrypted = false;
        bool opened = false;

        // Try opening without password first
        if (reader.Open(filePath)) {
            opened = true;
            if (reader.IsEncrypted()) {
                isEncrypted = true;
            }
        } else {
            isEncrypted = true;
        }

        std::vector<ArchiveFileInfo> internalFiles;
        Gdiplus::Bitmap* thumbnail = nullptr;
        std::wstring matchedPwd;

        if (opened && !isEncrypted) {
            internalFiles = reader.ListFiles();
            
            // Extract thumbnail of first image file
            std::string firstImageInternalPath;
            for (const auto& file : internalFiles) {
                if (!file.isDirectory && IsImageFile(file.name)) {
                    firstImageInternalPath = file.internalPath;
                    break;
                }
            }
            if (!firstImageInternalPath.empty()) {
                std::vector<char> buffer;
                if (reader.ExtractFileToMemory(firstImageInternalPath, buffer)) {
                    thumbnail = LoadThumbnailFromMemory(buffer.data(), buffer.size(), 64, 64);
                }
            }
            reader.Close();
        } else {
            if (opened) reader.Close();
            
            // Try saved passwords 1-by-1 in the background
            std::vector<std::wstring> savedPwds = PasswordStore::LoadPasswords();
            for (const auto& pwd : savedPwds) {
                bool exitLoop = false;
                EnterCriticalSection(&g_csArchives);
                if (g_bCancelScan || g_currentFolder != folderPath) {
                    exitLoop = true;
                }
                LeaveCriticalSection(&g_csArchives);
                if (exitLoop) break;

                if (reader.Open(filePath, pwd)) {
                    matchedPwd = pwd;
                    internalFiles = reader.ListFiles();
                    
                    // Extract thumbnail
                    std::string firstImageInternalPath;
                    for (const auto& file : internalFiles) {
                        if (!file.isDirectory && IsImageFile(file.name)) {
                            firstImageInternalPath = file.internalPath;
                            break;
                        }
                    }
                    if (!firstImageInternalPath.empty()) {
                        std::vector<char> buffer;
                        if (reader.ExtractFileToMemory(firstImageInternalPath, buffer)) {
                            thumbnail = LoadThumbnailFromMemory(buffer.data(), buffer.size(), 64, 64);
                        }
                    }
                    reader.Close();
                    break;
                }
            }
        }

        // Save scanned details safely
        EnterCriticalSection(&g_csArchives);
        if (!g_bCancelScan && g_currentFolder == folderPath && idx < (int)g_archives.size()) {
            g_archives[idx].isEncrypted = isEncrypted;
            g_archives[idx].internalFiles = internalFiles;
            g_archives[idx].thumbnail = thumbnail;
            g_archives[idx].currentPassword = matchedPwd;
            if (!isEncrypted || !internalFiles.empty()) {
                g_archives[idx].metadataLoaded = true;
            }
            
            // Send message to main window to redraw listbox item
            PostMessageW(hWnd, WM_USER_SCAN_ITEM, (WPARAM)idx, 0);
        } else {
            if (thumbnail) delete thumbnail;
        }
        LeaveCriticalSection(&g_csArchives);
    }

    return 0;
}

// Helper to get custom temp root outside Windows Temp to allow next/prev navigation in Photos App
std::wstring GetAppTempRoot() {
    wchar_t* userProfile = _wgetenv(L"USERPROFILE");
    if (userProfile) {
        std::wstring customPath = std::wstring(userProfile) + L"\\Pictures\\ArchivePreviewer";
        fs::create_directories(customPath);
        return customPath;
    }
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    std::wstring customPath = std::wstring(tempPath) + L"ArchivePreviewer";
    fs::create_directories(customPath);
    return customPath;
}

// Helper to cleanup temporary directories
void CleanupTempDirectories() {
    // Cancel grid loader thread first
    extern CRITICAL_SECTION g_csGridLoad;
    extern HANDLE g_hGridLoadThread;
    extern bool g_bCancelGridLoad;

    EnterCriticalSection(&g_csGridLoad);
    g_bCancelGridLoad = true;
    HANDLE hLoadThread = g_hGridLoadThread;
    g_hGridLoadThread = NULL;
    LeaveCriticalSection(&g_csGridLoad);

    if (hLoadThread) {
        WaitForSingleObject(hLoadThread, INFINITE);
        CloseHandle(hLoadThread);
    }



    EnterCriticalSection(&g_csTempExtract);
    for (const auto& dir : g_tempDirsCreated) {
        try {
            if (!dir.empty() && fs::exists(dir)) {
                fs::remove_all(dir);
            }
        } catch (...) {}
    }
    g_tempDirsCreated.clear();
    LeaveCriticalSection(&g_csTempExtract);
}

// Grid View WndProc declaration
LRESULT CALLBACK GridViewWndProc(HWND, UINT, WPARAM, LPARAM);

// Entry point
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    InitializeCriticalSection(&g_csArchives);
    InitializeCriticalSection(&g_csTempExtract);
    extern CRITICAL_SECTION g_csGridItems;
    InitializeCriticalSection(&g_csGridItems);
    extern CRITICAL_SECTION g_csGridLoad;
    InitializeCriticalSection(&g_csGridLoad);

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) {
        DeleteCriticalSection(&g_csArchives);
        DeleteCriticalSection(&g_csTempExtract);
        DeleteCriticalSection(&g_csGridItems);
        DeleteCriticalSection(&g_csGridLoad);
        return 0;
    }

    ULONG_PTR gdiplusToken;
    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    g_hDarkBgBrush = CreateSolidBrush(RGB(30, 30, 36));
    g_hPanelBgBrush = CreateSolidBrush(RGB(40, 40, 48));

    const wchar_t CLASS_NAME[] = L"ArchivePreviewerClass";
    WNDCLASSW wc = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = g_hDarkBgBrush;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon         = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));

    RegisterClassW(&wc);

    WNDCLASSW gvwc = {};
    gvwc.lpfnWndProc   = GridViewWndProc;
    gvwc.hInstance     = hInstance;
    gvwc.lpszClassName = L"ArchiveGridViewClass";
    gvwc.hbrBackground = g_hPanelBgBrush;
    gvwc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    gvwc.style         = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW; // Enable double clicks and redraw on resize!
    
    RegisterClassW(&gvwc);

    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    int w = 950;
    int h = 650;
    int x = (screenWidth - w) / 2;
    int y = (screenHeight - h) / 2;

    HWND hwnd = CreateWindowExW(
        0,
        CLASS_NAME,
        L"Archive Thumbnail Previewer & Extractor",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        x, y, w, h,
        NULL, NULL, hInstance, NULL
    );

    if (hwnd == NULL) {
        DeleteObject(g_hDarkBgBrush);
        DeleteObject(g_hPanelBgBrush);
        GdiplusShutdown(gdiplusToken);
        CoUninitialize();
        DeleteCriticalSection(&g_csArchives);
        DeleteCriticalSection(&g_csTempExtract);
        DeleteCriticalSection(&g_csGridItems);
        return 0;
    }

    g_hMainWnd = hwnd;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg = {};
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Signal cancellation and wait for background thread to exit before cleaning up
    EnterCriticalSection(&g_csArchives);
    g_bCancelScan = true;
    HANDLE hLastThread = g_hScanThread;
    g_hScanThread = NULL;
    LeaveCriticalSection(&g_csArchives);

    if (hLastThread) {
        WaitForSingleObject(hLastThread, INFINITE);
        CloseHandle(hLastThread);
    }

    // Cleanup temp extraction directories and threads
    CleanupTempDirectories();

    EnterCriticalSection(&g_csArchives);
    ClearArchives();
    LeaveCriticalSection(&g_csArchives);

    DeleteObject(g_hDarkBgBrush);
    DeleteObject(g_hPanelBgBrush);

    GdiplusShutdown(gdiplusToken);
    CoUninitialize();

    DeleteCriticalSection(&g_csArchives);
    DeleteCriticalSection(&g_csTempExtract);
    extern CRITICAL_SECTION g_csGridItems;
    DeleteCriticalSection(&g_csGridItems);
    extern CRITICAL_SECTION g_csGridLoad;
    DeleteCriticalSection(&g_csGridLoad);

    return (int)msg.wParam;
}

DWORD GetRegDword(HKEY hKeyParent, const std::wstring& subKey, const std::wstring& valueName, DWORD defaultValue) {
    HKEY hKey;
    DWORD value = defaultValue;
    if (RegOpenKeyExW(hKeyParent, subKey.c_str(), 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD size = sizeof(DWORD);
        RegQueryValueExW(hKey, valueName.c_str(), NULL, NULL, (BYTE*)&value, &size);
        RegCloseKey(hKey);
    }
    return value;
}

void SetRegDword(HKEY hKeyParent, const std::wstring& subKey, const std::wstring& valueName, DWORD value) {
    HKEY hKey;
    if (RegCreateKeyExW(hKeyParent, subKey.c_str(), 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, valueName.c_str(), 0, REG_DWORD, (const BYTE*)&value, sizeof(DWORD));
        RegCloseKey(hKey);
    }
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_CREATE: {
            g_hMainWnd = hWnd;
            g_hPathEdit = CreateWindowExW(0, L"EDIT", L"", 
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
                10, 10, 400, 25, hWnd, (HMENU)IDC_PATH_EDIT, NULL, NULL);

            g_hBrowseBtn = CreateWindowExW(0, L"BUTTON", L"Browse...", 
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                420, 10, 100, 25, hWnd, (HMENU)IDC_BROWSE_BTN, NULL, NULL);

            // Load view mode from registry (0 = List View, 1 = Grid View)
            DWORD savedViewMode = GetRegDword(HKEY_CURRENT_USER, L"SOFTWARE\\ArchivePreviewer", L"GridViewMode", 0);
            g_bGridView = (savedViewMode != 0);

            g_hViewToggleBtn = CreateWindowExW(0, L"BUTTON", g_bGridView ? L"List View" : L"Grid View", 
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                530, 10, 100, 25, hWnd, (HMENU)IDC_VIEW_TOGGLE_BTN, NULL, NULL);

            HWND hSettingsBtn = CreateWindowExW(0, L"BUTTON", L"Settings", 
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                640, 10, 100, 25, hWnd, (HMENU)IDC_SETTINGS_BTN, NULL, NULL);

            g_hExtractBtn = CreateWindowExW(0, L"BUTTON", L"Extract Selected", 
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                750, 10, 150, 25, hWnd, (HMENU)IDC_EXTRACT_BTN, NULL, NULL);

            g_hArchiveList = CreateWindowExW(0, L"LISTBOX", NULL,
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_OWNERDRAWFIXED | LBS_NOTIFY | WS_BORDER | LBS_NOINTEGRALHEIGHT,
                10, 45, 340, 500, hWnd, (HMENU)IDC_ARCHIVE_LIST, NULL, NULL);
            SendMessage(g_hArchiveList, LB_SETITEMHEIGHT, 0, 80);

            g_hContentsList = CreateWindowExW(0, L"LISTBOX", NULL,
                WS_CHILD | (g_bGridView ? 0 : WS_VISIBLE) | WS_VSCROLL | WS_BORDER | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                360, 45, 510, 500, hWnd, (HMENU)IDC_CONTENTS_LIST, NULL, NULL);

            g_hGridViewWnd = CreateWindowExW(0, L"ArchiveGridViewClass", NULL,
                WS_CHILD | (g_bGridView ? WS_VISIBLE : 0) | WS_VSCROLL | WS_BORDER,
                360, 45, 510, 500, hWnd, (HMENU)IDC_GRID_VIEW_WND, NULL, NULL);

            HFONT hFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            SendMessage(g_hPathEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(g_hBrowseBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(g_hViewToggleBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(hSettingsBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(g_hExtractBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(g_hContentsList, WM_SETFONT, (WPARAM)hFont, TRUE);

            std::wstring startFolder = L"C:\\Users\\Gusti N\\.gemini\\antigravity-ide\\scratch";
            std::wstring targetFileToSelect = L"";

            int argc = 0;
            LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
            if (argv && argc > 1) {
                std::wstring filePath = argv[1];
                if (fs::exists(filePath) && fs::is_regular_file(filePath)) {
                    startFolder = fs::path(filePath).parent_path().wstring();
                    targetFileToSelect = fs::path(filePath).filename().wstring();
                } else if (fs::exists(filePath) && fs::is_directory(filePath)) {
                    startFolder = filePath;
                }
                LocalFree(argv);
            }

            ScanDirectory(hWnd, startFolder);
            SetWindowTextW(g_hPathEdit, startFolder.c_str());

            if (!targetFileToSelect.empty()) {
                EnterCriticalSection(&g_csArchives);
                for (int i = 0; i < (int)g_archives.size(); ++i) {
                    if (g_archives[i].fileName == targetFileToSelect) {
                        LeaveCriticalSection(&g_csArchives);
                        SendMessage(g_hArchiveList, LB_SETCURSEL, i, 0);
                        UpdateContentsList(i);
                        break;
                    }
                }
                LeaveCriticalSection(&g_csArchives);
            }
            break;
        }

        case WM_SIZE: {
            int width = LOWORD(lParam);
            int height = HIWORD(lParam);

            int pathEditWidth = (width - 480 > 100) ? (width - 480) : 100;
            int leftWidth = g_splitPos - 10;
            int rightX = g_splitPos + 10;
            int rightWidth = width - g_splitPos - 20;

            HDWP hdwp = BeginDeferWindowPos(8);
            if (hdwp) {
                hdwp = DeferWindowPos(hdwp, g_hPathEdit, NULL, 10, 10, pathEditWidth, 25, SWP_NOZORDER | SWP_NOACTIVATE);
                hdwp = DeferWindowPos(hdwp, g_hBrowseBtn, NULL, width - 460, 10, 90, 25, SWP_NOZORDER | SWP_NOACTIVATE);
                hdwp = DeferWindowPos(hdwp, g_hViewToggleBtn, NULL, width - 360, 10, 90, 25, SWP_NOZORDER | SWP_NOACTIVATE);
                HWND hSettings = GetDlgItem(hWnd, IDC_SETTINGS_BTN);
                if (hSettings) {
                    hdwp = DeferWindowPos(hdwp, hSettings, NULL, width - 260, 10, 90, 25, SWP_NOZORDER | SWP_NOACTIVATE);
                }
                hdwp = DeferWindowPos(hdwp, g_hExtractBtn, NULL, width - 160, 10, 150, 25, SWP_NOZORDER | SWP_NOACTIVATE);
                hdwp = DeferWindowPos(hdwp, g_hArchiveList, NULL, 10, 45, leftWidth, height - 55, SWP_NOZORDER | SWP_NOACTIVATE);
                hdwp = DeferWindowPos(hdwp, g_hContentsList, NULL, rightX, 45, rightWidth, height - 55, SWP_NOZORDER | SWP_NOACTIVATE);
                if (g_hGridViewWnd) {
                    hdwp = DeferWindowPos(hdwp, g_hGridViewWnd, NULL, rightX, 45, rightWidth, height - 55, SWP_NOZORDER | SWP_NOACTIVATE);
                }
                EndDeferWindowPos(hdwp);
            }
            break;
        }

        case WM_SETCURSOR: {
            HWND hChild = (HWND)wParam;
            if (hChild == hWnd) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hWnd, &pt);
                if (pt.x >= g_splitPos && pt.x <= g_splitPos + 10 && pt.y >= 45) {
                    SetCursor(LoadCursor(NULL, IDC_SIZEWE));
                    return TRUE;
                }
            }
            break;
        }

        case WM_LBUTTONDOWN: {
            int x = (int)(short)LOWORD(lParam);
            int y = (int)(short)HIWORD(lParam);
            if (x >= g_splitPos && x <= g_splitPos + 10 && y >= 45) {
                g_bDraggingSplitter = true;
                SetCapture(hWnd);
                SetCursor(LoadCursor(NULL, IDC_SIZEWE));
            }
            break;
        }

        case WM_LBUTTONUP: {
            if (g_bDraggingSplitter) {
                g_bDraggingSplitter = false;
                ReleaseCapture();
            }
            break;
        }

        case WM_MOUSEMOVE: {
            int x = (int)(short)LOWORD(lParam);
            int y = (int)(short)HIWORD(lParam);
            
            if (g_bDraggingSplitter) {
                RECT rect;
                GetClientRect(hWnd, &rect);
                int width = rect.right;
                if (x > 150 && x < width - 150) {
                    int newSplitPos = x - 5;
                    if (newSplitPos != g_splitPos) {
                        g_splitPos = newSplitPos;
                        SendMessage(hWnd, WM_SIZE, 0, MAKELPARAM(rect.right, rect.bottom));
                        
                        RECT splitRect;
                        splitRect.left = g_splitPos - 15;
                        splitRect.right = g_splitPos + 15;
                        splitRect.top = 45;
                        splitRect.bottom = rect.bottom;
                        InvalidateRect(hWnd, &splitRect, TRUE);
                    }
                }
            } else {
                if (x >= g_splitPos && x <= g_splitPos + 10 && y >= 45) {
                    SetCursor(LoadCursor(NULL, IDC_SIZEWE));
                }
            }
            break;
        }

        case WM_CTLCOLORLISTBOX: {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, RGB(240, 240, 240));
            SetBkColor(hdc, RGB(40, 40, 48));
            return (INT_PTR)g_hPanelBgBrush;
        }

        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, RGB(220, 220, 220));
            SetBkColor(hdc, RGB(40, 40, 48));
            return (INT_PTR)g_hPanelBgBrush;
        }

        case WM_DRAWITEM: {
            LPDRAWITEMSTRUCT pDraw = (LPDRAWITEMSTRUCT)lParam;
            if (pDraw->CtlType == ODT_BUTTON) {
                HDC hdc = pDraw->hDC;
                RECT rc = pDraw->rcItem;
                UINT state = pDraw->itemState;

                COLORREF bgCol = RGB(55, 55, 65);
                COLORREF textCol = RGB(240, 240, 240);
                
                if (pDraw->CtlID == IDC_EXTRACT_BTN) {
                    bgCol = RGB(79, 70, 229);
                }

                if (state & ODS_SELECTED) {
                    if (pDraw->CtlID == IDC_EXTRACT_BTN) {
                        bgCol = RGB(67, 56, 202);
                    } else {
                        bgCol = RGB(40, 40, 50);
                    }
                }

                // Draw button background
                HBRUSH hBrush = CreateSolidBrush(bgCol);
                FillRect(hdc, &rc, hBrush);
                DeleteObject(hBrush);

                // Draw flat border
                HPEN hPen = CreatePen(PS_SOLID, 1, RGB(80, 80, 90));
                HGDIOBJ oldPen = SelectObject(hdc, hPen);
                HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
                Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
                SelectObject(hdc, oldPen);
                SelectObject(hdc, oldBrush);
                DeleteObject(hPen);

                // Draw text
                wchar_t text[64];
                GetWindowTextW(pDraw->hwndItem, text, 64);
                
                SetTextColor(hdc, textCol);
                SetBkMode(hdc, TRANSPARENT);
                
                HFONT hFont = CreateFontW(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
                HGDIOBJ oldFont = SelectObject(hdc, hFont);
                DrawTextW(hdc, text, -1, &rc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
                SelectObject(hdc, oldFont);
                DeleteObject(hFont);

                return TRUE;
            }
            if (pDraw->CtlID == IDC_ARCHIVE_LIST) {
                int index = pDraw->itemID;
                EnterCriticalSection(&g_csArchives);
                if (index < 0 || index >= (int)g_archives.size()) {
                    LeaveCriticalSection(&g_csArchives);
                    return TRUE;
                }

                const ArchiveItem& item = g_archives[index];
                HDC hdc = pDraw->hDC;
                RECT rc = pDraw->rcItem;

                COLORREF bgCol = RGB(40, 40, 48);
                COLORREF textCol = RGB(240, 240, 240);
                COLORREF accentCol = RGB(79, 70, 229);
                
                if (pDraw->itemState & ODS_SELECTED) {
                    HBRUSH hBrush = CreateSolidBrush(accentCol);
                    FillRect(hdc, &rc, hBrush);
                    DeleteObject(hBrush);
                } else {
                    HBRUSH hBrush = CreateSolidBrush(bgCol);
                    FillRect(hdc, &rc, hBrush);
                    DeleteObject(hBrush);
                }

                HPEN hPen = CreatePen(PS_SOLID, 1, RGB(60, 60, 70));
                HGDIOBJ oldPen = SelectObject(hdc, hPen);
                MoveToEx(hdc, rc.left, rc.bottom - 1, NULL);
                LineTo(hdc, rc.right, rc.bottom - 1);
                SelectObject(hdc, oldPen);
                DeleteObject(hPen);

                Graphics graphics(hdc);
                int thumbSize = 64;
                int padding = 8;
                int x = rc.left + padding;
                int y = rc.top + ((rc.bottom - rc.top) - thumbSize) / 2;

                if (item.thumbnail) {
                    graphics.DrawImage(item.thumbnail, x, y, thumbSize, thumbSize);
                } else {
                    SolidBrush placeholderBrush(Color(255, 60, 60, 70));
                    graphics.FillRectangle(&placeholderBrush, x, y, thumbSize, thumbSize);
                    
                    FontFamily fontFamily(L"Segoe UI");
                    Font font(&fontFamily, 14, FontStyleBold, UnitPixel);
                    SolidBrush textBrush(Color(255, 180, 180, 190));
                    StringFormat stringFormat;
                    stringFormat.SetAlignment(StringAlignmentCenter);
                    stringFormat.SetLineAlignment(StringAlignmentCenter);
                    
                    RectF layoutRect((REAL)x, (REAL)y, (REAL)thumbSize, (REAL)thumbSize);
                    
                    std::wstring typeText = L"ZIP";
                    std::wstring ext = fs::path(item.filePath).extension().wstring();
                    for (auto& ch : ext) ch = towupper(ch);
                    if (ext == L".RAR") typeText = L"RAR";
                    else if (ext == L".7Z") typeText = L"7Z";
                    
                    if (item.isEncrypted && item.currentPassword.empty()) {
                        typeText = L"LOCKED";
                    }
                    graphics.DrawString(typeText.c_str(), -1, &font, layoutRect, &stringFormat, &textBrush);
                }

                SetBkMode(hdc, TRANSPARENT);
                if (pDraw->itemState & ODS_SELECTED) {
                    SetTextColor(hdc, RGB(255, 255, 255));
                } else {
                    SetTextColor(hdc, textCol);
                }

                HFONT hFont = CreateFontW(15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
                HGDIOBJ oldFont = SelectObject(hdc, hFont);

                RECT rcText = rc;
                rcText.left += thumbSize + padding * 2;
                rcText.top += padding * 2;
                rcText.right -= padding;
                rcText.bottom = rcText.top + 20;
                
                DrawTextW(hdc, item.fileName.c_str(), -1, &rcText, DT_LEFT | DT_TOP | DT_END_ELLIPSIS | DT_NOPREFIX);
                SelectObject(hdc, oldFont);
                DeleteObject(hFont);

                HFONT hSubFont = CreateFontW(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
                oldFont = SelectObject(hdc, hSubFont);
                
                if (pDraw->itemState & ODS_SELECTED) {
                    SetTextColor(hdc, RGB(200, 200, 255));
                } else {
                    SetTextColor(hdc, RGB(140, 140, 150));
                }

                RECT rcSubText = rc;
                rcSubText.left += thumbSize + padding * 2;
                rcSubText.top += padding * 2 + 20;
                rcSubText.right -= padding;
                rcSubText.bottom = rcSubText.top + 30;

                wchar_t infoStr[128];
                if (item.isEncrypted && item.currentPassword.empty()) {
                    swprintf_s(infoStr, L"Encrypted archive (Click to unlock)");
                } else {
                    swprintf_s(infoStr, L"%u files inside", (unsigned int)item.internalFiles.size());
                }
                DrawTextW(hdc, infoStr, -1, &rcSubText, DT_LEFT | DT_TOP | DT_END_ELLIPSIS);

                SelectObject(hdc, oldFont);
                DeleteObject(hSubFont);

                LeaveCriticalSection(&g_csArchives);
                return TRUE;
            }
            break;
        }

        case WM_USER_SCAN_ITEM: {
            int index = (int)wParam;
            EnterCriticalSection(&g_csArchives);
            if (index >= 0 && index < (int)g_archives.size()) {
                RECT rect;
                SendMessageW(g_hArchiveList, LB_GETITEMRECT, index, (LPARAM)&rect);
                InvalidateRect(g_hArchiveList, &rect, TRUE);
                
                int curSel = (int)SendMessageW(g_hArchiveList, LB_GETCURSEL, 0, 0);
                if (curSel == index) {
                    UpdateContentsList(index);
                }
            }
            LeaveCriticalSection(&g_csArchives);
            break;
        }

        case WM_COMMAND: {
            int wmId = LOWORD(wParam);
            int wmEvent = HIWORD(wParam);

            if (wmId == IDC_ARCHIVE_LIST && wmEvent == LBN_SELCHANGE) {
                int index = (int)SendMessageW(g_hArchiveList, LB_GETCURSEL, 0, 0);
                UpdateContentsList(index);
            }
            else if (wmId == IDC_ARCHIVE_LIST && wmEvent == LBN_DBLCLK) {
                int index = (int)SendMessageW(g_hArchiveList, LB_GETCURSEL, 0, 0);
                ExtractSelectedArchive(index);
            }
            else if (wmId == IDC_CONTENTS_LIST && wmEvent == LBN_DBLCLK) {
                int archiveIndex = (int)SendMessageW(g_hArchiveList, LB_GETCURSEL, 0, 0);
                int fileIndex = (int)SendMessageW(g_hContentsList, LB_GETCURSEL, 0, 0);
                if (archiveIndex >= 0 && fileIndex >= 0) {
                    void OpenArchiveFile(int archiveIndex, int fileIndex);
                    OpenArchiveFile(archiveIndex, fileIndex);
                }
            }
            else if (wmId == IDC_BROWSE_BTN && wmEvent == BN_CLICKED) {
                std::wstring folder = ChooseFolder(hWnd);
                if (!folder.empty()) {
                    ScanDirectory(hWnd, folder);
                }
            }
            else if (wmId == IDC_SETTINGS_BTN && wmEvent == BN_CLICKED) {
                ShowSettingsDialog(hWnd);
            }
            else if (wmId == IDC_EXTRACT_BTN && wmEvent == BN_CLICKED) {
                int index = (int)SendMessage(g_hArchiveList, LB_GETCURSEL, 0, 0);
                if (index >= 0) {
                    ExtractSelectedArchive(index);
                } else {
                    MessageBoxW(hWnd, L"Please select an archive first.", L"Warning", MB_ICONWARNING | MB_OK);
                }
            }
            else if (wmId == IDC_VIEW_TOGGLE_BTN && wmEvent == BN_CLICKED) {
                g_bGridView = !g_bGridView;
                SetRegDword(HKEY_CURRENT_USER, L"SOFTWARE\\ArchivePreviewer", L"GridViewMode", g_bGridView ? 1 : 0);
                if (g_bGridView) {
                    SetWindowTextW(g_hViewToggleBtn, L"List View");
                    ShowWindow(g_hContentsList, SW_HIDE);
                    if (g_hGridViewWnd) {
                        ShowWindow(g_hGridViewWnd, SW_SHOW);
                        InvalidateRect(g_hGridViewWnd, NULL, TRUE);
                    }
                } else {
                    SetWindowTextW(g_hViewToggleBtn, L"Grid View");
                    if (g_hGridViewWnd) {
                        ShowWindow(g_hGridViewWnd, SW_HIDE);
                    }
                    ShowWindow(g_hContentsList, SW_SHOW);
                }
            }
            break;
        }

        case WM_DESTROY: {
            PostQuitMessage(0);
            break;
        }

        case WM_USER_METADATA_LOADED: {
            int archiveIndex = (int)wParam;
            int sessionId = (int)lParam;
            
            EnterCriticalSection(&g_csGridLoad);
            bool sessionValid = (g_metadataSessionId == sessionId);
            LeaveCriticalSection(&g_csGridLoad);
            
            LogDebug("WndProc: WM_USER_METADATA_LOADED for index %d, session %d, valid=%d", archiveIndex, sessionId, sessionValid);
            
            if (sessionValid) {
                if (g_hArchiveList) {
                    RECT rcItem;
                    SendMessageW(g_hArchiveList, LB_GETITEMRECT, archiveIndex, (LPARAM)&rcItem);
                    InvalidateRect(g_hArchiveList, &rcItem, TRUE);
                }
                
                int curSel = (int)SendMessageW(g_hArchiveList, LB_GETCURSEL, 0, 0);
                LogDebug("WndProc: curSel = %d", curSel);
                if (curSel == archiveIndex) {
                    UpdateContentsList(archiveIndex);
                }
            }
            return 0;
        }

        case WM_USER_NEED_PASSWORD: {
            int archiveIndex = (int)wParam;
            int sessionId = (int)lParam;
            
            EnterCriticalSection(&g_csGridLoad);
            bool sessionValid = (g_metadataSessionId == sessionId);
            LeaveCriticalSection(&g_csGridLoad);
            
            if (!sessionValid) return 0;
            
            std::wstring filePath;
            EnterCriticalSection(&g_csArchives);
            if (archiveIndex >= 0 && archiveIndex < (int)g_archives.size()) {
                filePath = g_archives[archiveIndex].filePath;
            }
            LeaveCriticalSection(&g_csArchives);
            
            if (!filePath.empty()) {
                ArchiveReader reader;
                std::wstring enteredPwd;
                if (OpenArchiveWithPasswordSupport(reader, filePath, hWnd, enteredPwd)) {
                    auto files = reader.ListFiles();
                    
                    // Extract thumbnail of first image file synchronously
                    Gdiplus::Bitmap* thumbnail = nullptr;
                    std::string firstImageInternalPath;
                    for (const auto& file : files) {
                        if (!file.isDirectory && IsImageFile(file.name)) {
                            firstImageInternalPath = file.internalPath;
                            break;
                        }
                    }
                    if (!firstImageInternalPath.empty()) {
                        std::vector<char> buffer;
                        if (reader.ExtractFileToMemory(firstImageInternalPath, buffer)) {
                            thumbnail = LoadThumbnailFromMemory(buffer.data(), buffer.size(), 64, 64);
                        }
                    }
                    reader.Close();
                    
                    EnterCriticalSection(&g_csArchives);
                    if (archiveIndex < (int)g_archives.size() && g_archives[archiveIndex].filePath == filePath) {
                        g_archives[archiveIndex].internalFiles = files;
                        g_archives[archiveIndex].currentPassword = enteredPwd;
                        g_archives[archiveIndex].isEncrypted = true;
                        g_archives[archiveIndex].metadataLoaded = true;
                        if (thumbnail) {
                            if (g_archives[archiveIndex].thumbnail) delete g_archives[archiveIndex].thumbnail;
                            g_archives[archiveIndex].thumbnail = thumbnail;
                        }
                    } else {
                        if (thumbnail) delete thumbnail;
                    }
                    LeaveCriticalSection(&g_csArchives);
                    
                    PostMessageW(hWnd, WM_USER_METADATA_LOADED, (WPARAM)archiveIndex, (LPARAM)sessionId);
                } else {
                    int curSel = (int)SendMessageW(g_hArchiveList, LB_GETCURSEL, 0, 0);
                    if (curSel == archiveIndex) {
                        SendMessageW(g_hContentsList, LB_RESETCONTENT, 0, 0);
                        SendMessageW(g_hContentsList, LB_ADDSTRING, 0, (LPARAM)L"[Dibatalkan atau memerlukan sandi]");
                    }
                }
            }
            return 0;
        }

        default:
            return DefWindowProcW(hWnd, uMsg, wParam, lParam);
    }
    return 0;
}

std::wstring ChooseFolder(HWND hwndParent) {
    std::wstring folderPath;
    IFileOpenDialog* pFileOpen = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL, IID_IFileOpenDialog, reinterpret_cast<void**>(&pFileOpen));
    if (SUCCEEDED(hr)) {
        DWORD dwOptions;
        if (SUCCEEDED(pFileOpen->GetOptions(&dwOptions))) {
            pFileOpen->SetOptions(dwOptions | FOS_PICKFOLDERS);
        }
        if (SUCCEEDED(pFileOpen->Show(hwndParent))) {
            IShellItem* pItem = nullptr;
            if (SUCCEEDED(pFileOpen->GetResult(&pItem))) {
                PWSTR pszFilePath = nullptr;
                if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath))) {
                    folderPath = pszFilePath;
                    CoTaskMemFree(pszFilePath);
                }
                pItem->Release();
            }
        }
        pFileOpen->Release();
    }
    return folderPath;
}

bool IsImageFile(const std::wstring& filename) {
    std::wstring ext;
    size_t dotPos = filename.find_last_of(L'.');
    if (dotPos != std::wstring::npos) {
        ext = filename.substr(dotPos);
    }
    for (auto& ch : ext) {
        ch = towlower(ch);
    }
    return ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".bmp" || ext == L".gif";
}

Bitmap* LoadThumbnailFromMemory(const char* data, size_t size, int thumbWidth, int thumbHeight) {
    if (size == 0) return nullptr;
    HGLOBAL hBuffer = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!hBuffer) return nullptr;

    void* pBuffer = GlobalLock(hBuffer);
    if (!pBuffer) {
        GlobalFree(hBuffer);
        return nullptr;
    }
    memcpy(pBuffer, data, size);
    GlobalUnlock(hBuffer);

    IStream* pStream = nullptr;
    if (CreateStreamOnHGlobal(hBuffer, TRUE, &pStream) != S_OK) {
        GlobalFree(hBuffer);
        return nullptr;
    }

    Bitmap* pOrig = Bitmap::FromStream(pStream);
    Bitmap* pThumb = nullptr;
    if (pOrig) {
        if (pOrig->GetLastStatus() == Ok) {
            pThumb = CreateThumbnail(pOrig, thumbWidth, thumbHeight);
        }
        delete pOrig;
    }
    pStream->Release(); // Releases stream and frees hBuffer
    return pThumb;
}

Bitmap* LoadThumbnailFromFile(const wchar_t* filePath, int thumbWidth, int thumbHeight) {
    Bitmap* pOrig = Bitmap::FromFile(filePath);
    Bitmap* pThumb = nullptr;
    if (pOrig) {
        if (pOrig->GetLastStatus() == Ok) {
            pThumb = CreateThumbnail(pOrig, thumbWidth, thumbHeight);
        }
        delete pOrig;
    }
    return pThumb;
}

Bitmap* CreateThumbnail(Bitmap* pSource, int thumbWidth, int thumbHeight) {
    Bitmap* pThumb = new Bitmap(thumbWidth, thumbHeight, PixelFormat32bppARGB);
    Graphics graphics(pThumb);
    
    graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    graphics.SetSmoothingMode(SmoothingModeHighQuality);
    graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    
    int srcWidth = pSource->GetWidth();
    int srcHeight = pSource->GetHeight();
    
    float ratioX = (float)thumbWidth / srcWidth;
    float ratioY = (float)thumbHeight / srcHeight;
    float ratio = (std::min)(ratioX, ratioY);
    
    int newWidth = (int)(srcWidth * ratio);
    int newHeight = (int)(srcHeight * ratio);
    
    int posX = (thumbWidth - newWidth) / 2;
    int posY = (thumbHeight - newHeight) / 2;

    graphics.Clear(Color(0, 0, 0, 0));
    graphics.DrawImage(pSource, posX, posY, newWidth, newHeight);
    return pThumb;
}

void ClearArchives() {
    for (auto& item : g_archives) {
        if (item.thumbnail) {
            delete item.thumbnail;
        }
    }
    g_archives.clear();
}

void ScanDirectory(HWND hWnd, const std::wstring& folderPath) {
    // Signal cancellation and wait for previous thread to exit
    EnterCriticalSection(&g_csArchives);
    g_bCancelScan = true;
    HANDLE hPrevThread = g_hScanThread;
    g_hScanThread = NULL;
    LeaveCriticalSection(&g_csArchives);

    if (hPrevThread) {
        WaitForSingleObject(hPrevThread, INFINITE);
        CloseHandle(hPrevThread);
    }

    // Stop and cleanup background extraction thread safely outside g_csArchives to prevent deadlock
    void CleanupTempDirectories();
    CleanupTempDirectories();

    EnterCriticalSection(&g_csArchives);
    g_bCancelScan = false;
    ClearArchives();
    g_currentFolder = folderPath;
    
    if (g_hArchiveList) SendMessage(g_hArchiveList, LB_RESETCONTENT, 0, 0);
    if (g_hContentsList) SendMessage(g_hContentsList, LB_RESETCONTENT, 0, 0);
    if (g_hPathEdit) SetWindowTextW(g_hPathEdit, folderPath.c_str());

    if (folderPath.empty() || !fs::exists(folderPath)) {
        LeaveCriticalSection(&g_csArchives);
        return;
    }

    std::vector<std::wstring> filePathsToScan;

    try {
        for (const auto& entry : fs::directory_iterator(folderPath)) {
            if (entry.is_regular_file()) {
                std::wstring ext = entry.path().extension().wstring();
                for (auto& ch : ext) ch = towlower(ch);
                
                if (ext == L".zip" || ext == L".rar" || ext == L".7z") {
                    if (IsNonFirstMultiPartVolume(entry.path().wstring())) {
                        continue;
                    }
                    ArchiveItem item;
                    item.filePath = entry.path().wstring();
                    item.fileName = entry.path().filename().wstring();
                    item.thumbnail = nullptr;
                    item.isEncrypted = false;
                    item.metadataLoaded = false;
                    item.extractionFinished = false;

                    g_archives.push_back(item);
                    filePathsToScan.push_back(item.filePath);
                    
                    if (g_hArchiveList) {
                        SendMessageW(g_hArchiveList, LB_ADDSTRING, 0, (LPARAM)item.fileName.c_str());
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        MessageBoxA(NULL, e.what(), "Error Scanning Folder", MB_ICONERROR | MB_OK);
    }
    
    // Spawn background thread to scan metadata and extract thumbnails
    if (!filePathsToScan.empty()) {
        ScanThreadParams* params = new ScanThreadParams();
        params->hWnd = hWnd; // Pass the valid hWnd directly!
        params->folderPath = folderPath;
        params->filePaths = filePathsToScan;
        
        g_hScanThread = CreateThread(NULL, 0, ScanDirectoryThread, params, 0, NULL);
        if (g_hScanThread) {
            SetThreadPriority(g_hScanThread, THREAD_PRIORITY_BELOW_NORMAL);
        } else {
            delete params;
        }
    }
    
    LeaveCriticalSection(&g_csArchives);
}



void UpdateContentsList(int archiveIndex) {
    if (!g_hContentsList) return;

    EnterCriticalSection(&g_csArchives);
    g_priorityIndex = archiveIndex;
    if (archiveIndex < 0 || archiveIndex >= (int)g_archives.size()) {
        SendMessage(g_hContentsList, LB_RESETCONTENT, 0, 0);
        LeaveCriticalSection(&g_csArchives);
        return;
    }

    auto& archive = g_archives[archiveIndex];
    std::wstring filePath = archive.filePath;
    std::wstring password = archive.currentPassword;
    
    // If archive is not yet loaded/scanned (metadataLoaded is false)
    if (!archive.metadataLoaded) {
        // Clear UI and show loading status immediately to keep UI active and responsive
        SendMessageW(g_hContentsList, LB_RESETCONTENT, 0, 0);
        SendMessageW(g_hContentsList, LB_ADDSTRING, 0, (LPARAM)L"Membuka arsip...");
        
        // Clear active grid items
        EnterCriticalSection(&g_csGridItems);
        for (auto& item : g_gridItems) {
            if (item.pBitmap) delete item.pBitmap;
        }
        g_gridItems.clear();
        LeaveCriticalSection(&g_csGridItems);
        if (g_hGridViewWnd) InvalidateRect(g_hGridViewWnd, NULL, TRUE);

        // Cancel any pending metadata load threads by incrementing session ID
        EnterCriticalSection(&g_csGridLoad);
        g_metadataSessionId++;
        int currentSession = g_metadataSessionId;
        LeaveCriticalSection(&g_csGridLoad);

        LoadMetadataParams* metadataParams = new LoadMetadataParams();
        metadataParams->hWnd = g_hMainWnd;
        metadataParams->archiveIndex = archiveIndex;
        metadataParams->filePath = filePath;
        metadataParams->sessionId = currentSession;
        
        HANDLE hThread = CreateThread(NULL, 0, LoadMetadataThread, metadataParams, 0, NULL);
        if (hThread) {
            SetThreadPriority(hThread, THREAD_PRIORITY_ABOVE_NORMAL);
            CloseHandle(hThread);
        } else {
            delete metadataParams;
        }

        LeaveCriticalSection(&g_csArchives);
        return;
    }

    // Populate contents if already loaded
    SendMessageW(g_hContentsList, LB_RESETCONTENT, 0, 0);
    UpdateGridItems(archive.internalFiles, filePath, password);

    if (archive.internalFiles.empty()) {
        SendMessageW(g_hContentsList, LB_ADDSTRING, 0, (LPARAM)L"[Tidak ada file atau gagal memuat arsip]");
    } else {
        for (const auto& file : archive.internalFiles) {
            std::wstring displayName = fs::path(file.name).filename().wstring();
            std::wstring itemText = displayName;
            if (file.isDirectory) {
                itemText += L"  [Directory]";
            } else {
                wchar_t sizeStr[64];
                swprintf_s(sizeStr, L"  (%u KB)", (unsigned int)(file.fileSize / 1024));
                itemText += sizeStr;
            }
            SendMessageW(g_hContentsList, LB_ADDSTRING, 0, (LPARAM)itemText.c_str());
        }
    }
    LeaveCriticalSection(&g_csArchives);
}

// Opens a file using its default associated app (e.g. Photos for images).
// Since all images are flat-extracted to the same folder, Photos can see
// sibling files and enable Next/Prev navigation.
bool OpenFileWithDefaultApp(const std::wstring& filePath) {
    HINSTANCE result = ShellExecuteW(NULL, NULL, filePath.c_str(), NULL, NULL, SW_SHOWNORMAL);
    return (INT_PTR)result > 32;
}

void OpenArchiveFile(int archiveIndex, int fileIndex) {
    EnterCriticalSection(&g_csArchives);
    if (archiveIndex < 0 || archiveIndex >= (int)g_archives.size()) {
        LeaveCriticalSection(&g_csArchives);
        return;
    }
    const auto& archive = g_archives[archiveIndex];
    if (fileIndex < 0 || fileIndex >= (int)archive.internalFiles.size()) {
        LeaveCriticalSection(&g_csArchives);
        return;
    }
    const auto& file = archive.internalFiles[fileIndex];
    if (file.isDirectory) {
        LeaveCriticalSection(&g_csArchives);
        return;
    }

    std::wstring filePath = archive.filePath;
    std::wstring currentPassword = archive.currentPassword;
    std::string internalPath = file.internalPath;
    std::wstring fileName = file.name;
    std::wstring tempExtractDir = archive.tempExtractDir;
    LeaveCriticalSection(&g_csArchives);

    // Extract file singly to temp folder
    std::wstring appTempDir = GetAppTempRoot() + L"\\SingleExtract";
    fs::create_directories(appTempDir);
    
    EnterCriticalSection(&g_csTempExtract);
    if (std::find(g_tempDirsCreated.begin(), g_tempDirsCreated.end(), appTempDir) == g_tempDirsCreated.end()) {
        g_tempDirsCreated.push_back(appTempDir);
    }
    LeaveCriticalSection(&g_csTempExtract);

    std::wstring destPath = appTempDir + L"\\" + fs::path(fileName).filename().wstring();

    ArchiveReader reader;
    if (reader.Open(filePath, currentPassword)) {
        bool success = reader.ExtractFileToDisk(internalPath, destPath);
        reader.Close();
        if (success) {
            OpenFileWithDefaultApp(destPath);
        } else {
            MessageBoxW(NULL, L"Failed to extract file for viewing.", L"Error", MB_ICONERROR | MB_OK);
        }
    }
}

void ExtractSelectedArchive(int index) {
    EnterCriticalSection(&g_csArchives);
    if (index < 0 || index >= (int)g_archives.size()) {
        LeaveCriticalSection(&g_csArchives);
        return;
    }
    const auto& archive = g_archives[index];
    std::wstring filePath = archive.filePath;
    std::wstring pwd = archive.currentPassword;
    bool isEncrypted = archive.isEncrypted;
    LeaveCriticalSection(&g_csArchives);

    std::wstring targetFolder = filePath;
    size_t dotPos = targetFolder.find_last_of(L'.');
    if (dotPos != std::wstring::npos) {
        targetFolder = targetFolder.substr(0, dotPos);
    }
    
    ArchiveReader reader;
    if (isEncrypted && pwd.empty()) {
        if (!OpenArchiveWithPasswordSupport(reader, filePath, g_hMainWnd, pwd)) {
            return;
        }
        EnterCriticalSection(&g_csArchives);
        if (index < (int)g_archives.size() && g_archives[index].filePath == filePath) {
            g_archives[index].currentPassword = pwd;
        }
        LeaveCriticalSection(&g_csArchives);
    } else {
        if (!reader.Open(filePath, pwd)) {
            MessageBoxW(NULL, L"Failed to open archive.", L"Error", MB_ICONERROR | MB_OK);
            return;
        }
    }
    
    bool success = reader.ExtractAll(targetFolder);
    reader.Close();
    if (success) {
        std::wstring msg = L"Successfully extracted to:\n" + targetFolder;
        MessageBoxW(NULL, msg.c_str(), L"Extraction Complete", MB_ICONINFORMATION | MB_OK);
        ShellExecuteW(NULL, L"open", targetFolder.c_str(), NULL, NULL, SW_SHOWNORMAL);
    } else {
        MessageBoxW(NULL, L"Failed to extract files.", L"Error", MB_ICONERROR | MB_OK);
    }
}

// Dialog procedures and helpers
struct PasswordDlgData {
    std::wstring archivePath;
    std::wstring password;
    bool savePassword;
    HFONT hFont;
    HWND hEdit;
    HWND hCheckbox;
    INT_PTR result;
};

LRESULT CALLBACK PasswordDlgProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    PasswordDlgData* data = (PasswordDlgData*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
    switch (uMsg) {
        case WM_CREATE: {
            CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
            data = (PasswordDlgData*)cs->lpCreateParams;
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)data);
            
            std::wstring filename = fs::path(data->archivePath).filename().wstring();
            std::wstring labelText = L"Arsip \"" + filename + L"\" terproteksi sandi.\nMasukkan sandi:";
            
            HWND hLabel = CreateWindowExW(0, L"STATIC", labelText.c_str(),
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                15, 15, 370, 40, hWnd, NULL, cs->hInstance, NULL);
                
            data->hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_PASSWORD | ES_AUTOHSCROLL,
                15, 60, 370, 25, hWnd, NULL, cs->hInstance, NULL);
                
            data->hCheckbox = CreateWindowExW(0, L"BUTTON", L"Simpan kata sandi ke pengaturan",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                15, 95, 370, 20, hWnd, NULL, cs->hInstance, NULL);
                
            HWND hOk = CreateWindowExW(0, L"BUTTON", L"OK",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                180, 125, 100, 25, hWnd, (HMENU)IDOK, cs->hInstance, NULL);
                
            HWND hCancel = CreateWindowExW(0, L"BUTTON", L"Batal",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                290, 125, 100, 25, hWnd, (HMENU)IDCANCEL, cs->hInstance, NULL);
                
            SendMessageW(hLabel, WM_SETFONT, (WPARAM)data->hFont, TRUE);
            SendMessageW(data->hEdit, WM_SETFONT, (WPARAM)data->hFont, TRUE);
            SendMessageW(data->hCheckbox, WM_SETFONT, (WPARAM)data->hFont, TRUE);
            SendMessageW(hOk, WM_SETFONT, (WPARAM)data->hFont, TRUE);
            SendMessageW(hCancel, WM_SETFONT, (WPARAM)data->hFont, TRUE);
            
            SetFocus(data->hEdit);
            return 0;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT: {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, RGB(220, 220, 220));
            SetBkColor(hdc, RGB(40, 40, 48));
            return (INT_PTR)g_hPanelBgBrush;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDOK) {
                wchar_t buf[256];
                GetWindowTextW(data->hEdit, buf, 256);
                data->password = buf;
                data->savePassword = (SendMessageW(data->hCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED);
                data->result = IDOK;
                DestroyWindow(hWnd);
            } else if (id == IDCANCEL) {
                data->result = IDCANCEL;
                DestroyWindow(hWnd);
            }
            break;
        }
        case WM_CLOSE: {
            data->result = IDCANCEL;
            DestroyWindow(hWnd);
            break;
        }
        default:
            return DefWindowProcW(hWnd, uMsg, wParam, lParam);
    }
    return 0;
}

INT_PTR ShowPasswordPromptDialog(HWND hwndParent, const std::wstring& archivePath, std::wstring& outPassword, bool& outSavePassword) {
    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(hwndParent, GWLP_HINSTANCE);
    
    WNDCLASSW wc = {};
    wc.lpfnWndProc = PasswordDlgProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"PasswordDlgClass";
    wc.hbrBackground = g_hDarkBgBrush;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    
    RegisterClassW(&wc);
    
    HFONT hFont = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    
    PasswordDlgData data;
    data.archivePath = archivePath;
    data.savePassword = false;
    data.hFont = hFont;
    data.result = IDCANCEL;
    
    RECT rcParent;
    GetWindowRect(hwndParent, &rcParent);
    int w = 415, h = 200;
    int x = rcParent.left + (rcParent.right - rcParent.left - w) / 2;
    int y = rcParent.top + (rcParent.bottom - rcParent.top - h) / 2;
    
    HWND hwndDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        L"PasswordDlgClass",
        L"Meminta Sandi",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        x, y, w, h,
        hwndParent, NULL, hInst, &data
    );
    
    if (hwndDlg) {
        EnableWindow(hwndParent, FALSE);
        ShowWindow(hwndDlg, SW_SHOW);
        UpdateWindow(hwndDlg);
        
        MSG msg;
        while (IsWindow(hwndDlg) && GetMessageW(&msg, NULL, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        EnableWindow(hwndParent, TRUE);
        SetForegroundWindow(hwndParent);
    }
    
    DeleteObject(hFont);
    UnregisterClassW(L"PasswordDlgClass", hInst);
    
    outPassword = data.password;
    outSavePassword = data.savePassword;
    return data.result;
}

struct InputBoxData {
    std::wstring title;
    std::wstring prompt;
    std::wstring value;
    HFONT hFont;
    HWND hEdit;
    INT_PTR result;
};

LRESULT CALLBACK InputBoxDlgProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    InputBoxData* data = (InputBoxData*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
    switch (uMsg) {
        case WM_CREATE: {
            CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
            data = (InputBoxData*)cs->lpCreateParams;
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)data);
            
            HWND hLabel = CreateWindowExW(0, L"STATIC", data->prompt.c_str(),
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                15, 15, 320, 20, hWnd, NULL, cs->hInstance, NULL);
                
            data->hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", data->value.c_str(),
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                15, 45, 320, 25, hWnd, NULL, cs->hInstance, NULL);
                
            HWND hOk = CreateWindowExW(0, L"BUTTON", L"OK",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                130, 85, 100, 25, hWnd, (HMENU)IDOK, cs->hInstance, NULL);
                
            HWND hCancel = CreateWindowExW(0, L"BUTTON", L"Batal",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                235, 85, 100, 25, hWnd, (HMENU)IDCANCEL, cs->hInstance, NULL);
                
            SendMessageW(hLabel, WM_SETFONT, (WPARAM)data->hFont, TRUE);
            SendMessageW(data->hEdit, WM_SETFONT, (WPARAM)data->hFont, TRUE);
            SendMessageW(hOk, WM_SETFONT, (WPARAM)data->hFont, TRUE);
            SendMessageW(hCancel, WM_SETFONT, (WPARAM)data->hFont, TRUE);
            
            SetFocus(data->hEdit);
            SendMessageW(data->hEdit, EM_SETSEL, 0, -1);
            return 0;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT: {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, RGB(220, 220, 220));
            SetBkColor(hdc, RGB(40, 40, 48));
            return (INT_PTR)g_hPanelBgBrush;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDOK) {
                wchar_t buf[256];
                GetWindowTextW(data->hEdit, buf, 256);
                data->value = buf;
                data->result = IDOK;
                DestroyWindow(hWnd);
            } else if (id == IDCANCEL) {
                data->result = IDCANCEL;
                DestroyWindow(hWnd);
            }
            break;
        }
        case WM_CLOSE: {
            data->result = IDCANCEL;
            DestroyWindow(hWnd);
            break;
        }
        default:
            return DefWindowProcW(hWnd, uMsg, wParam, lParam);
    }
    return 0;
}

bool ShowInputBox(HWND hwndParent, const std::wstring& title, const std::wstring& prompt, std::wstring& ioValue) {
    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(hwndParent, GWLP_HINSTANCE);
    
    WNDCLASSW wc = {};
    wc.lpfnWndProc = InputBoxDlgProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"InputBoxDlgClass";
    wc.hbrBackground = g_hDarkBgBrush;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    
    RegisterClassW(&wc);
    
    HFONT hFont = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    
    InputBoxData data;
    data.title = title;
    data.prompt = prompt;
    data.value = ioValue;
    data.hFont = hFont;
    data.result = IDCANCEL;
    
    RECT rcParent;
    GetWindowRect(hwndParent, &rcParent);
    int w = 365, h = 160;
    int x = rcParent.left + (rcParent.right - rcParent.left - w) / 2;
    int y = rcParent.top + (rcParent.bottom - rcParent.top - h) / 2;
    
    HWND hwndDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        L"InputBoxDlgClass",
        title.c_str(),
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        x, y, w, h,
        hwndParent, NULL, hInst, &data
    );
    
    if (hwndDlg) {
        EnableWindow(hwndParent, FALSE);
        ShowWindow(hwndDlg, SW_SHOW);
        UpdateWindow(hwndDlg);
        
        MSG msg;
        while (IsWindow(hwndDlg) && GetMessageW(&msg, NULL, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        EnableWindow(hwndParent, TRUE);
        SetForegroundWindow(hwndParent);
    }
    
    DeleteObject(hFont);
    UnregisterClassW(L"InputBoxDlgClass", hInst);
    
    if (data.result == IDOK) {
        ioValue = data.value;
        return true;
    }
    return false;
}

struct SettingsDlgData {
    std::vector<std::wstring> passwords;
    HFONT hFont;
    HWND hListBox;
    HWND hAddBtn;
    HWND hEditBtn;
    HWND hDelBtn;
    HWND hCloseBtn;
};

void PopulatePasswordsList(HWND hListBox, const std::vector<std::wstring>& passwords) {
    SendMessageW(hListBox, LB_RESETCONTENT, 0, 0);
    for (const auto& pwd : passwords) {
        SendMessageW(hListBox, LB_ADDSTRING, 0, (LPARAM)pwd.c_str());
    }
}

LRESULT CALLBACK SettingsDlgProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    SettingsDlgData* data = (SettingsDlgData*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
    switch (uMsg) {
        case WM_CREATE: {
            CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
            data = (SettingsDlgData*)cs->lpCreateParams;
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)data);
            
            data->hListBox = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER | LBS_NOTIFY,
                15, 15, 300, 230, hWnd, NULL, cs->hInstance, NULL);
                
            data->hAddBtn = CreateWindowExW(0, L"BUTTON", L"Tambah",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                330, 15, 100, 25, hWnd, (HMENU)1001, cs->hInstance, NULL);
                
            data->hEditBtn = CreateWindowExW(0, L"BUTTON", L"Ubah",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                330, 50, 100, 25, hWnd, (HMENU)1002, cs->hInstance, NULL);
                
            data->hDelBtn = CreateWindowExW(0, L"BUTTON", L"Hapus",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                330, 85, 100, 25, hWnd, (HMENU)1003, cs->hInstance, NULL);
                
            data->hCloseBtn = CreateWindowExW(0, L"BUTTON", L"Tutup",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                330, 220, 100, 25, hWnd, (HMENU)IDCANCEL, cs->hInstance, NULL);
                
            SendMessageW(data->hListBox, WM_SETFONT, (WPARAM)data->hFont, TRUE);
            SendMessageW(data->hAddBtn, WM_SETFONT, (WPARAM)data->hFont, TRUE);
            SendMessageW(data->hEditBtn, WM_SETFONT, (WPARAM)data->hFont, TRUE);
            SendMessageW(data->hDelBtn, WM_SETFONT, (WPARAM)data->hFont, TRUE);
            SendMessageW(data->hCloseBtn, WM_SETFONT, (WPARAM)data->hFont, TRUE);
            
            PopulatePasswordsList(data->hListBox, data->passwords);
            return 0;
        }
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT: {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, RGB(220, 220, 220));
            SetBkColor(hdc, RGB(40, 40, 48));
            return (INT_PTR)g_hPanelBgBrush;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            
            if (id == 1001) { // Add
                std::wstring newPwd;
                if (ShowInputBox(hWnd, L"Tambah Sandi", L"Masukkan kata sandi baru:", newPwd) && !newPwd.empty()) {
                    data->passwords.push_back(newPwd);
                    PasswordStore::SavePasswords(data->passwords);
                    PopulatePasswordsList(data->hListBox, data->passwords);
                }
            }
            else if (id == 1002) { // Edit
                int sel = (int)SendMessageW(data->hListBox, LB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < (int)data->passwords.size()) {
                    std::wstring pwd = data->passwords[sel];
                    if (ShowInputBox(hWnd, L"Ubah Sandi", L"Ubah kata sandi:", pwd)) {
                        if (pwd.empty()) {
                            data->passwords.erase(data->passwords.begin() + sel);
                        } else {
                            data->passwords[sel] = pwd;
                        }
                        PasswordStore::SavePasswords(data->passwords);
                        PopulatePasswordsList(data->hListBox, data->passwords);
                    }
                } else {
                    MessageBoxW(hWnd, L"Silakan pilih sandi yang ingin diubah.", L"Info", MB_ICONINFORMATION | MB_OK);
                }
            }
            else if (id == 1003) { // Delete
                int sel = (int)SendMessageW(data->hListBox, LB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < (int)data->passwords.size()) {
                    if (MessageBoxW(hWnd, L"Apakah Anda yakin ingin menghapus sandi ini?", L"Konfirmasi Hapus", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                        data->passwords.erase(data->passwords.begin() + sel);
                        PasswordStore::SavePasswords(data->passwords);
                        PopulatePasswordsList(data->hListBox, data->passwords);
                    }
                } else {
                    MessageBoxW(hWnd, L"Silakan pilih sandi yang ingin dihapus.", L"Info", MB_ICONINFORMATION | MB_OK);
                }
            }
            else if (id == IDCANCEL) {
                DestroyWindow(hWnd);
            }
            break;
        }
        case WM_CLOSE: {
            DestroyWindow(hWnd);
            break;
        }
        default:
            return DefWindowProcW(hWnd, uMsg, wParam, lParam);
    }
    return 0;
}

void ShowSettingsDialog(HWND hwndParent) {
    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(hwndParent, GWLP_HINSTANCE);
    
    WNDCLASSW wc = {};
    wc.lpfnWndProc = SettingsDlgProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"SettingsDlgClass";
    wc.hbrBackground = g_hDarkBgBrush;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    
    RegisterClassW(&wc);
    
    HFONT hFont = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    
    SettingsDlgData data;
    data.passwords = PasswordStore::LoadPasswords();
    data.hFont = hFont;
    
    RECT rcParent;
    GetWindowRect(hwndParent, &rcParent);
    int w = 465, h = 300;
    int x = rcParent.left + (rcParent.right - rcParent.left - w) / 2;
    int y = rcParent.top + (rcParent.bottom - rcParent.top - h) / 2;
    
    HWND hwndDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        L"SettingsDlgClass",
        L"Pengaturan Sandi Tersimpan",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        x, y, w, h,
        hwndParent, NULL, hInst, &data
    );
    
    if (hwndDlg) {
        EnableWindow(hwndParent, FALSE);
        ShowWindow(hwndDlg, SW_SHOW);
        UpdateWindow(hwndDlg);
        
        MSG msg;
        while (IsWindow(hwndDlg) && GetMessageW(&msg, NULL, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        EnableWindow(hwndParent, TRUE);
        SetForegroundWindow(hwndParent);
    }
    
    DeleteObject(hFont);
    UnregisterClassW(L"SettingsDlgClass", hInst);
}

bool OpenArchiveWithPasswordSupport(ArchiveReader& reader, const std::wstring& archivePath, HWND hwndParent, std::wstring& outPassword) {
    if (reader.Open(archivePath)) {
        if (!reader.IsEncrypted()) {
            return true;
        }
        reader.Close();
    }

    std::vector<std::wstring> savedPwds = PasswordStore::LoadPasswords();
    for (const auto& pwd : savedPwds) {
        if (reader.Open(archivePath, pwd)) {
            outPassword = pwd;
            return true;
        }
    }

    while (true) {
        std::wstring enteredPwd;
        bool savePwd = false;
        
        INT_PTR res = ShowPasswordPromptDialog(hwndParent, archivePath, enteredPwd, savePwd);
        if (res != IDOK) {
            return false;
        }

        if (reader.Open(archivePath, enteredPwd)) {
            outPassword = enteredPwd;
            if (savePwd) {
                if (std::find(savedPwds.begin(), savedPwds.end(), enteredPwd) == savedPwds.end()) {
                    savedPwds.push_back(enteredPwd);
                    PasswordStore::SavePasswords(savedPwds);
                }
            }
            return true;
        } else {
            MessageBoxW(hwndParent, L"Kata sandi salah. Silakan coba lagi.", L"Error", MB_ICONERROR | MB_OK);
        }
    }
}

// (GridItemCache, g_csGridItems, g_gridItems defined at top of file)
int g_selectedGridIndex = -1;
int g_hoverGridIndex = -1;
int g_gridScrollPos = 0;

void GetGridLayoutParams(int wndWidth, int& outCols, int& outCardWidth, int& outCardHeight, int& outMargin) {
    outMargin = 15;
    outCardHeight = 140;
    int minCardWidth = 105;
    
    if (wndWidth <= 0) {
        outCols = 1;
        outCardWidth = minCardWidth;
        return;
    }
    
    outCols = (wndWidth - outMargin) / (minCardWidth + outMargin);
    if (outCols < 1) outCols = 1;
    
    // Distribute remaining space by scaling cardWidth up
    outCardWidth = (wndWidth - (outCols + 1) * outMargin) / outCols;
    if (outCardWidth < minCardWidth) outCardWidth = minCardWidth;
}

struct GridLoadThreadParams {
    std::wstring archivePath;
    std::wstring password;
    int sessionId;
};

// Background thumbnail loader thread function
DWORD WINAPI GridThumbnailLoaderThread(LPVOID lpParam) {
    GridLoadThreadParams* params = (GridLoadThreadParams*)lpParam;
    std::wstring archivePath = params->archivePath;
    std::wstring password = params->password;
    int sessionId = params->sessionId;
    delete params;

    // Pause background scanner to allocate all resources to active grid thumbnail loading
    g_bPauseBackgroundScan = true;

    ArchiveReader reader;
    bool readerOpened = false;

    while (true) {
        // Quick session check
        EnterCriticalSection(&g_csGridLoad);
        bool cancel = g_bCancelGridLoad || (g_gridLoadSessionId != sessionId);
        LeaveCriticalSection(&g_csGridLoad);

        if (cancel) break;

        // Find next batch of items to load
        EnterCriticalSection(&g_csGridItems);
        size_t total = g_gridItems.size();
        
        std::vector<int> batchIndexes;
        std::vector<std::string> batchPaths;
        std::vector<std::wstring> batchNames;

        int wndHeight = 0;
        int wndWidth = 0;
        if (g_hGridViewWnd) {
            RECT rect;
            GetClientRect(g_hGridViewWnd, &rect);
            wndHeight = rect.bottom;
            wndWidth = rect.right;
        }
        int cols, cardWidth, cardHeight, margin;
        GetGridLayoutParams(wndWidth, cols, cardWidth, cardHeight, margin);

        // 1. Try to find visible, un-attempted items first
        for (size_t i = 0; i < total; ++i) {
            if (!g_gridItems[i].pBitmap && !g_gridItems[i].loadAttempted) {
                int row = (int)i / cols;
                int itemY = margin + row * (cardHeight + margin) - g_gridScrollPos;
                if (itemY + cardHeight >= 0 && itemY <= wndHeight) {
                    batchIndexes.push_back((int)i);
                    batchPaths.push_back(g_gridItems[i].internalPath);
                    batchNames.push_back(g_gridItems[i].name);
                    if (batchIndexes.size() >= 8) break;
                }
            }
        }

        // 2. If we didn't fill the batch, fall back to sequential items
        if (batchIndexes.size() < 8) {
            for (size_t i = 0; i < total; ++i) {
                if (!g_gridItems[i].pBitmap && !g_gridItems[i].loadAttempted) {
                    if (std::find(batchIndexes.begin(), batchIndexes.end(), (int)i) == batchIndexes.end()) {
                        batchIndexes.push_back((int)i);
                        batchPaths.push_back(g_gridItems[i].internalPath);
                        batchNames.push_back(g_gridItems[i].name);
                        if (batchIndexes.size() >= 8) break;
                    }
                }
            }
        }
        LeaveCriticalSection(&g_csGridItems);

        if (batchIndexes.empty()) {
            break; // No more items to load
        }

        // Open reader if not already opened
        if (!readerOpened) {
            if (reader.Open(archivePath, password)) {
                readerOpened = true;
            } else {
                // If we can't open the archive, fail all remaining items (if session is still valid)
                EnterCriticalSection(&g_csGridLoad);
                bool sessionValid = (g_gridLoadSessionId == sessionId);
                LeaveCriticalSection(&g_csGridLoad);
                if (sessionValid) {
                    EnterCriticalSection(&g_csGridItems);
                    for (auto& item : g_gridItems) {
                        item.loadAttempted = true;
                    }
                    LeaveCriticalSection(&g_csGridItems);
                }
                break;
            }
        }

        // Check cancellation again before slow extraction
        EnterCriticalSection(&g_csGridLoad);
        cancel = g_bCancelGridLoad || (g_gridLoadSessionId != sessionId);
        LeaveCriticalSection(&g_csGridLoad);
        if (cancel) break;

        // Perform batch extraction if using 7-Zip
        bool batchSuccess = false;
        std::wstring tempDir = GetAppTempRoot() + L"\\GridBatch_" + std::to_wstring(sessionId) + L"_" + std::to_wstring(batchIndexes[0]);
        if (reader.Uses7Zip()) {
            batchSuccess = reader.ExtractFilesToDisk(batchPaths, tempDir);
        }

        for (size_t k = 0; k < batchIndexes.size(); ++k) {
            int itemIndex = batchIndexes[k];
            std::string internalPath = batchPaths[k];
            std::wstring name = batchNames[k];

            // Double check cancellation for each file in the batch
            EnterCriticalSection(&g_csGridLoad);
            cancel = g_bCancelGridLoad || (g_gridLoadSessionId != sessionId);
            LeaveCriticalSection(&g_csGridLoad);
            if (cancel) break;

            Gdiplus::Bitmap* pThumb = nullptr;

            if (reader.Uses7Zip()) {
                if (batchSuccess) {
                    std::wstring wpath = Utf8ToWString(internalPath);
                    for (auto& ch : wpath) {
                        if (ch == L'/') ch = L'\\';
                    }
                    std::wstring diskPath = tempDir + L"\\" + wpath;
                    if (fs::exists(diskPath)) {
                        pThumb = LoadThumbnailFromFile(diskPath.c_str(), 100, 100);
                    }
                }
            } else {
                // Zip file, extract in-memory directly
                std::vector<char> buffer;
                if (reader.ExtractFileToMemory(internalPath, buffer)) {
                    pThumb = LoadThumbnailFromMemory(buffer.data(), buffer.size(), 100, 100);
                }
            }

            // Check session validity again before writing back to globals
            EnterCriticalSection(&g_csGridLoad);
            bool sessionValid = (g_gridLoadSessionId == sessionId);
            LeaveCriticalSection(&g_csGridLoad);

            if (sessionValid) {
                EnterCriticalSection(&g_csGridItems);
                // Verify the item at itemIndex is still the same one (in case grid items were cleared/updated)
                if (itemIndex < (int)g_gridItems.size() && g_gridItems[itemIndex].name == name) {
                    if (pThumb) {
                        g_gridItems[itemIndex].pBitmap = pThumb;
                    } else {
                        g_gridItems[itemIndex].loadAttempted = true;
                    }
                } else {
                    if (pThumb) delete pThumb;
                }
                LeaveCriticalSection(&g_csGridItems);
            } else {
                if (pThumb) delete pThumb;
            }
        }

        // Clean up temp batch folder if created
        if (reader.Uses7Zip()) {
            std::error_code ec;
            fs::remove_all(tempDir, ec);
        }

        // Repaint grid
        EnterCriticalSection(&g_csGridLoad);
        bool sessionValid = (g_gridLoadSessionId == sessionId);
        LeaveCriticalSection(&g_csGridLoad);
        if (sessionValid && g_hGridViewWnd) {
            InvalidateRect(g_hGridViewWnd, NULL, FALSE);
        }
    }

    if (readerOpened) {
        reader.Close();
    }

    // Resume background scanner if this session is still the active one
    EnterCriticalSection(&g_csGridLoad);
    if (g_gridLoadSessionId == sessionId) {
        g_bPauseBackgroundScan = false;
    }
    LeaveCriticalSection(&g_csGridLoad);

    return 0;
}

void UpdateGridItems(const std::vector<ArchiveFileInfo>& files, const std::wstring& archivePath, const std::wstring& password) {
    EnterCriticalSection(&g_csGridLoad);
    g_bCancelGridLoad = true;
    g_gridLoadSessionId++; // Increment session ID so any running thread will ignore its results
    int currentSession = g_gridLoadSessionId;
    HANDLE hPrevLoad = g_hGridLoadThread;
    g_hGridLoadThread = NULL;
    LeaveCriticalSection(&g_csGridLoad);

    // We do NOT block on hPrevLoad! The thread will self-terminate safely.
    if (hPrevLoad) {
        CloseHandle(hPrevLoad);
    }

    EnterCriticalSection(&g_csGridItems);
    for (auto& item : g_gridItems) {
        if (item.pBitmap) delete item.pBitmap;
    }
    g_gridItems.clear();
    g_selectedGridIndex = -1;
    g_hoverGridIndex = -1;
    g_gridScrollPos = 0;

    for (const auto& file : files) {
        if (!file.isDirectory && IsImageFile(file.name)) {
            GridItemCache item;
            item.name = file.name;
            item.internalPath = file.internalPath;
            item.pBitmap = nullptr;
            item.loadAttempted = false;
            g_gridItems.push_back(item);
        }
    }
    LeaveCriticalSection(&g_csGridItems);

    EnterCriticalSection(&g_csGridLoad);
    g_bCancelGridLoad = false;
    GridLoadThreadParams* pLoadParams = new GridLoadThreadParams();
    pLoadParams->archivePath = archivePath;
    pLoadParams->password = password;
    pLoadParams->sessionId = currentSession;
    g_hGridLoadThread = CreateThread(NULL, 0, GridThumbnailLoaderThread, pLoadParams, 0, NULL);
    if (!g_hGridLoadThread) {
        delete pLoadParams;
    }
    LeaveCriticalSection(&g_csGridLoad);
    
    if (g_hGridViewWnd) {
        SCROLLINFO si = { 0 };
        si.cbSize = sizeof(si);
        si.fMask = SIF_POS;
        si.nPos = 0;
        SetScrollInfo(g_hGridViewWnd, SB_VERT, &si, TRUE);
        
        SendMessageW(g_hGridViewWnd, WM_SIZE, 0, 0);
    }
}

// Helper to draw rounded rectangle using GDI+
void DrawRoundedRect(Gdiplus::Graphics& graphics, Gdiplus::Brush& brush, Gdiplus::Pen& pen, REAL x, REAL y, REAL w, REAL h, REAL r) {
    Gdiplus::GraphicsPath path;
    path.AddArc(x, y, r * 2, r * 2, 180, 90);
    path.AddArc(x + w - r * 2, y, r * 2, r * 2, 270, 90);
    path.AddArc(x + w - r * 2, y + h - r * 2, r * 2, r * 2, 0, 90);
    path.AddArc(x, y + h - r * 2, r * 2, r * 2, 90, 90);
    path.CloseFigure();
    graphics.FillPath(&brush, &path);
    graphics.DrawPath(&pen, &path);
}

LRESULT CALLBACK GridViewWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_CREATE: {
            SCROLLINFO si = { 0 };
            si.cbSize = sizeof(si);
            si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
            si.nMin = 0;
            si.nMax = 0;
            si.nPage = 100;
            si.nPos = 0;
            SetScrollInfo(hWnd, SB_VERT, &si, TRUE);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_SIZE: {
            RECT rect;
            GetClientRect(hWnd, &rect);
            int width = rect.right;
            int height = rect.bottom;

            int cols, cardWidth, cardHeight, margin;
            GetGridLayoutParams(width, cols, cardWidth, cardHeight, margin);

            EnterCriticalSection(&g_csGridItems);
            int totalItems = (int)g_gridItems.size();
            LeaveCriticalSection(&g_csGridItems);

            int rows = (totalItems + cols - 1) / cols;
            int totalHeight = rows * (cardHeight + margin) + margin;

            SCROLLINFO si = { 0 };
            si.cbSize = sizeof(si);
            si.fMask = SIF_RANGE | SIF_PAGE;
            si.nMin = 0;
            si.nMax = totalHeight;
            si.nPage = height;
            SetScrollInfo(hWnd, SB_VERT, &si, TRUE);
            
            GetScrollInfo(hWnd, SB_VERT, &si);
            if (si.nPos > si.nMax - (int)si.nPage) {
                si.nPos = si.nMax - (int)si.nPage;
                if (si.nPos < 0) si.nPos = 0;
                si.fMask = SIF_POS;
                SetScrollInfo(hWnd, SB_VERT, &si, TRUE);
                g_gridScrollPos = si.nPos;
            }
            InvalidateRect(hWnd, NULL, TRUE);
            return 0;
        }

        case WM_VSCROLL: {
            SCROLLINFO si = { 0 };
            si.cbSize = sizeof(si);
            si.fMask = SIF_ALL;
            GetScrollInfo(hWnd, SB_VERT, &si);

            int oldPos = si.nPos;
            switch (LOWORD(wParam)) {
                case SB_TOP: si.nPos = si.nMin; break;
                case SB_BOTTOM: si.nPos = si.nMax; break;
                case SB_LINEUP: si.nPos -= 20; break;
                case SB_LINEDOWN: si.nPos += 20; break;
                case SB_PAGEUP: si.nPos -= si.nPage; break;
                case SB_PAGEDOWN: si.nPos += si.nPage; break;
                case SB_THUMBTRACK: si.nPos = si.nTrackPos; break;
            }

            if (si.nPos < si.nMin) si.nPos = si.nMin;
            if (si.nPos > si.nMax - (int)si.nPage) si.nPos = si.nMax - (int)si.nPage;

            if (si.nPos != oldPos) {
                si.fMask = SIF_POS;
                SetScrollInfo(hWnd, SB_VERT, &si, TRUE);
                g_gridScrollPos = si.nPos;
                ScrollWindowEx(hWnd, 0, oldPos - si.nPos, NULL, NULL, NULL, NULL, SW_INVALIDATE | SW_ERASE);
                UpdateWindow(hWnd);
            }
            return 0;
        }

        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            int scrollAmt = -(delta / 120) * 60;
            
            SCROLLINFO si = { 0 };
            si.cbSize = sizeof(si);
            si.fMask = SIF_ALL;
            GetScrollInfo(hWnd, SB_VERT, &si);

            int oldPos = si.nPos;
            si.nPos += scrollAmt;

            if (si.nPos < si.nMin) si.nPos = si.nMin;
            if (si.nPos > si.nMax - (int)si.nPage) si.nPos = si.nMax - (int)si.nPage;

            if (si.nPos != oldPos) {
                si.fMask = SIF_POS;
                SetScrollInfo(hWnd, SB_VERT, &si, TRUE);
                g_gridScrollPos = si.nPos;
                ScrollWindowEx(hWnd, 0, oldPos - si.nPos, NULL, NULL, NULL, NULL, SW_INVALIDATE | SW_ERASE);
                UpdateWindow(hWnd);
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            int x = (int)(short)LOWORD(lParam);
            int y = (int)(short)HIWORD(lParam) + g_gridScrollPos;

            RECT rect;
            GetClientRect(hWnd, &rect);
            int width = rect.right;

            int cols, cardWidth, cardHeight, margin;
            GetGridLayoutParams(width, cols, cardWidth, cardHeight, margin);

            EnterCriticalSection(&g_csGridItems);
            int totalItems = (int)g_gridItems.size();
            LeaveCriticalSection(&g_csGridItems);

            int newHoverIndex = -1;
            for (int i = 0; i < totalItems; ++i) {
                int row = i / cols;
                int col = i % cols;
                int itemX = margin + col * (cardWidth + margin);
                int itemY = margin + row * (cardHeight + margin);

                if (x >= itemX && x <= itemX + cardWidth && y >= itemY && y <= itemY + cardHeight) {
                    newHoverIndex = i;
                    break;
                }
            }

            if (newHoverIndex != g_hoverGridIndex) {
                g_hoverGridIndex = newHoverIndex;
                InvalidateRect(hWnd, NULL, TRUE);

                TRACKMOUSEEVENT tme = { 0 };
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hWnd;
                TrackMouseEvent(&tme);
            }
            return 0;
        }

        case WM_MOUSELEAVE: {
            if (g_hoverGridIndex != -1) {
                g_hoverGridIndex = -1;
                InvalidateRect(hWnd, NULL, TRUE);
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            int x = (int)(short)LOWORD(lParam);
            int y = (int)(short)HIWORD(lParam) + g_gridScrollPos;

            RECT rect;
            GetClientRect(hWnd, &rect);
            int width = rect.right;

            int cols, cardWidth, cardHeight, margin;
            GetGridLayoutParams(width, cols, cardWidth, cardHeight, margin);

            EnterCriticalSection(&g_csGridItems);
            int totalItems = (int)g_gridItems.size();
            LeaveCriticalSection(&g_csGridItems);

            int clickedIndex = -1;
            for (int i = 0; i < totalItems; ++i) {
                int row = i / cols;
                int col = i % cols;
                int itemX = margin + col * (cardWidth + margin);
                int itemY = margin + row * (cardHeight + margin);

                if (x >= itemX && x <= itemX + cardWidth && y >= itemY && y <= itemY + cardHeight) {
                    clickedIndex = i;
                    break;
                }
            }

            if (clickedIndex != g_selectedGridIndex) {
                g_selectedGridIndex = clickedIndex;
                InvalidateRect(hWnd, NULL, TRUE);
            }
            SetFocus(hWnd);
            return 0;
        }

        case WM_LBUTTONDBLCLK: {
            if (g_selectedGridIndex != -1) {
                int archiveIndex = (int)SendMessageW(g_hArchiveList, LB_GETCURSEL, 0, 0);
                if (archiveIndex >= 0) {
                    EnterCriticalSection(&g_csArchives);
                    const auto& archive = g_archives[archiveIndex];
                    std::wstring filePath = archive.filePath;
                    std::wstring tempExtractDir = archive.tempExtractDir;
                    std::wstring currentPassword = archive.currentPassword;
                    LeaveCriticalSection(&g_csArchives);

                    EnterCriticalSection(&g_csGridItems);
                    if (g_selectedGridIndex < (int)g_gridItems.size()) {
                        std::wstring fileName = g_gridItems[g_selectedGridIndex].name;
                        LeaveCriticalSection(&g_csGridItems);

                        EnterCriticalSection(&g_csArchives);
                        int fileIndex = -1;
                        for (int i = 0; i < (int)archive.internalFiles.size(); ++i) {
                            if (archive.internalFiles[i].name == fileName) {
                                fileIndex = i;
                                break;
                            }
                        }
                        LeaveCriticalSection(&g_csArchives);

                        if (fileIndex >= 0) {
                            void OpenArchiveFile(int archiveIndex, int fileIndex);
                            OpenArchiveFile(archiveIndex, fileIndex);
                        }
                    } else {
                        LeaveCriticalSection(&g_csGridItems);
                    }
                }
            }
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);

            RECT rect;
            GetClientRect(hWnd, &rect);
            int width = rect.right;
            int height = rect.bottom;

            HDC hdcMem = CreateCompatibleDC(hdc);
            HBITMAP hbmMem = CreateCompatibleBitmap(hdc, width, height);
            HGDIOBJ hOld = SelectObject(hdcMem, hbmMem);

            FillRect(hdcMem, &rect, g_hPanelBgBrush);

            int cols, cardWidth, cardHeight, margin;
            GetGridLayoutParams(width, cols, cardWidth, cardHeight, margin);

            EnterCriticalSection(&g_csGridItems);
            int totalItems = (int)g_gridItems.size();

            Gdiplus::Graphics graphics(hdcMem);
            // Enable antialiasing for rounded cards
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
            
            Gdiplus::FontFamily fontFamily(L"Segoe UI");
            Gdiplus::Font font(&fontFamily, 9, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
            Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 230, 230, 240));
            Gdiplus::SolidBrush placeholderTextBrush(Gdiplus::Color(255, 140, 140, 150));
            Gdiplus::StringFormat strFormat;
            strFormat.SetAlignment(Gdiplus::StringAlignmentCenter);
            strFormat.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            strFormat.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
            strFormat.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

            for (int i = 0; i < totalItems; ++i) {
                int row = i / cols;
                int col = i % cols;
                int itemX = margin + col * (cardWidth + margin);
                int itemY = margin + row * (cardHeight + margin) - g_gridScrollPos;

                if (itemY + cardHeight >= 0 && itemY <= height) {
                    auto& item = g_gridItems[i];

                    Gdiplus::Color cardBgColor(255, 45, 45, 54);
                    Gdiplus::Color borderColor(255, 65, 65, 75);

                    if (i == g_selectedGridIndex) {
                        cardBgColor = Gdiplus::Color(255, 59, 50, 200); 
                        borderColor = Gdiplus::Color(255, 79, 70, 229);
                    } else if (i == g_hoverGridIndex) {
                        cardBgColor = Gdiplus::Color(255, 55, 55, 66); 
                        borderColor = Gdiplus::Color(255, 100, 100, 115);
                    }

                    Gdiplus::SolidBrush cardBgBrush(cardBgColor);
                    Gdiplus::Pen borderPen(borderColor, 1.0f);
                    
                    // Draw beautiful rounded rect card
                    DrawRoundedRect(graphics, cardBgBrush, borderPen, (REAL)itemX, (REAL)itemY, (REAL)cardWidth, (REAL)cardHeight, 6.0f);

                    if (item.pBitmap) {
                        int imgX = itemX + (cardWidth - item.pBitmap->GetWidth()) / 2;
                        int imgY = itemY + 10 + (100 - item.pBitmap->GetHeight()) / 2;
                        graphics.DrawImage(item.pBitmap, imgX, imgY);
                    } else {
                        Gdiplus::SolidBrush grayBrush(Gdiplus::Color(255, 35, 35, 42));
                        Gdiplus::Pen grayPen(Gdiplus::Color(255, 50, 50, 58), 1.0f);
                        DrawRoundedRect(graphics, grayBrush, grayPen, (REAL)itemX + 10, (REAL)itemY + 10, 100.0f, 100.0f, 4.0f);
                        Gdiplus::RectF placeholderRect((REAL)itemX + 10, (REAL)itemY + 10, 100.0f, 100.0f);
                        graphics.DrawString(L"Loading...", -1, &font, placeholderRect, &strFormat, &placeholderTextBrush);
                    }

                    std::wstring label = fs::path(item.name).filename().wstring();
                    Gdiplus::RectF textRect((REAL)itemX + 5, (REAL)itemY + 115, (REAL)cardWidth - 10, 20.0f);
                    graphics.DrawString(label.c_str(), -1, &font, textRect, &strFormat, &textBrush);
                }
            }
            
            if (totalItems == 0) {
                Gdiplus::FontFamily fontFamily(L"Segoe UI");
                Gdiplus::Font fontLarge(&fontFamily, 12, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
                Gdiplus::SolidBrush statusBrush(Gdiplus::Color(255, 140, 140, 150));
                Gdiplus::RectF layoutRect(0.0f, 0.0f, (REAL)width, (REAL)height);
                
                int selectedIndex = -1;
                if (g_hArchiveList) {
                    selectedIndex = (int)SendMessageW(g_hArchiveList, LB_GETCURSEL, 0, 0);
                }
                
                std::wstring statusMsg = L"Tidak ada arsip terpilih";
                if (selectedIndex >= 0) {
                    bool loaded = false;
                    EnterCriticalSection(&g_csArchives);
                    if (selectedIndex < (int)g_archives.size()) {
                        loaded = g_archives[selectedIndex].metadataLoaded;
                    }
                    LeaveCriticalSection(&g_csArchives);
                    
                    if (!loaded) {
                        statusMsg = L"Membuka arsip...";
                    } else {
                        statusMsg = L"Tidak ada file gambar di dalam arsip ini";
                    }
                }
                graphics.DrawString(statusMsg.c_str(), -1, &fontLarge, layoutRect, &strFormat, &statusBrush);
            }
            
            LeaveCriticalSection(&g_csGridItems);

            BitBlt(hdc, 0, 0, width, height, hdcMem, 0, 0, SRCCOPY);

            SelectObject(hdcMem, hOld);
            DeleteObject(hbmMem);
            DeleteDC(hdcMem);

            EndPaint(hWnd, &ps);
            return 0;
        }

        case WM_DESTROY: {
            EnterCriticalSection(&g_csGridItems);
            for (auto& item : g_gridItems) {
                if (item.pBitmap) delete item.pBitmap;
            }
            g_gridItems.clear();
            LeaveCriticalSection(&g_csGridItems);
            return 0;
        }

        default:
            return DefWindowProcW(hWnd, uMsg, wParam, lParam);
    }
}
