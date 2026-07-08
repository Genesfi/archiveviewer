#define NOMINMAX
#include <windows.h>
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
#define WM_USER_SCAN_ITEM  (WM_USER + 101)

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

CRITICAL_SECTION g_csArchives;
HWND g_hMainWnd = NULL;
HANDLE g_hScanThread = NULL;
bool g_bCancelScan = false;

// Extraction globals
CRITICAL_SECTION g_csTempExtract;
HANDLE g_hExtractThread = NULL;
bool g_bCancelExtract = false;
std::wstring g_currentArchiveExtracting; // filePath of archive currently being extracted
std::vector<std::wstring> g_tempDirsCreated; // Keep track of all created temp folders to delete them on exit

// Grid loader globals
CRITICAL_SECTION g_csGridLoad;
HANDLE g_hGridLoadThread = NULL;
bool g_bCancelGridLoad = false;

// Function declarations
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
std::wstring ChooseFolder(HWND hwndParent);
void ScanDirectory(HWND hWnd, const std::wstring& folderPath);
void ClearArchives();
void UpdateContentsList(int archiveIndex);
void ExtractSelectedArchive(int index);
bool IsImageFile(const std::wstring& filename);
Bitmap* LoadImageFromMemory(const char* data, size_t size);
Bitmap* CreateThumbnail(Bitmap* pSource, int thumbWidth, int thumbHeight);
INT_PTR ShowPasswordPromptDialog(HWND hwndParent, const std::wstring& archivePath, std::wstring& outPassword, bool& outSavePassword);
void ShowSettingsDialog(HWND hwndParent);
bool OpenArchiveWithPasswordSupport(ArchiveReader& reader, const std::wstring& archivePath, HWND hwndParent, std::wstring& outPassword);

struct ExtractThreadParams {
    std::wstring archivePath;
    std::wstring password;
    std::wstring tempDir;
    int archiveIndex;
};

DWORD WINAPI ExtractArchiveThread(LPVOID lpParam) {
    ExtractThreadParams* params = (ExtractThreadParams*)lpParam;
    std::wstring archivePath = params->archivePath;
    std::wstring password = params->password;
    std::wstring tempDir = params->tempDir;
    int archiveIndex = params->archiveIndex;
    delete params;

    ArchiveReader reader;
    if (reader.Open(archivePath, password)) {
        reader.ExtractAll(tempDir);
    }

    EnterCriticalSection(&g_csTempExtract);
    EnterCriticalSection(&g_csArchives);
    if (archiveIndex >= 0 && archiveIndex < (int)g_archives.size() && g_archives[archiveIndex].filePath == archivePath) {
        g_archives[archiveIndex].extractionFinished = true;
        if (g_hGridViewWnd) {
            InvalidateRect(g_hGridViewWnd, NULL, TRUE);
        }
    }
    LeaveCriticalSection(&g_csArchives);
    
    if (g_hExtractThread) {
        CloseHandle(g_hExtractThread);
        g_hExtractThread = NULL;
    }
    g_currentArchiveExtracting.clear();
    LeaveCriticalSection(&g_csTempExtract);

    return 0;
}

// Struct for background thread parameters
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

    for (size_t i = 0; i < filePaths.size(); ++i) {
        // Stop scanning if cancelled, folder has changed, or app is closing
        bool shouldExit = false;
        bool shouldSkip = false;
        EnterCriticalSection(&g_csArchives);
        if (g_bCancelScan || g_currentFolder != folderPath) {
            shouldExit = true;
        } else if (i < g_archives.size() && !g_archives[i].internalFiles.empty()) {
            shouldSkip = true;
        }
        LeaveCriticalSection(&g_csArchives);
        if (shouldExit) break;
        if (shouldSkip) continue;

        std::wstring filePath = filePaths[i];
        
        ArchiveReader reader;
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
                    Bitmap* pOrig = LoadImageFromMemory(buffer.data(), buffer.size());
                    if (pOrig) {
                        thumbnail = CreateThumbnail(pOrig, 64, 64);
                        delete pOrig;
                    }
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
                            Bitmap* pOrig = LoadImageFromMemory(buffer.data(), buffer.size());
                            if (pOrig) {
                                thumbnail = CreateThumbnail(pOrig, 64, 64);
                                delete pOrig;
                            }
                        }
                    }
                    reader.Close();
                    break;
                }
            }
        }

        // Save scanned details safely
        EnterCriticalSection(&g_csArchives);
        if (!g_bCancelScan && g_currentFolder == folderPath && i < g_archives.size()) {
            g_archives[i].isEncrypted = isEncrypted;
            g_archives[i].internalFiles = internalFiles;
            g_archives[i].thumbnail = thumbnail;
            g_archives[i].currentPassword = matchedPwd;
            
            // Send message to main window to redraw listbox item
            PostMessageW(hWnd, WM_USER_SCAN_ITEM, (WPARAM)i, 0);
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
        std::wstring customPath = std::wstring(userProfile) + L"\\Pictures\\ArchivePreviewerTemp";
        fs::create_directories(customPath);
        return customPath;
    }
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    std::wstring customPath = std::wstring(tempPath) + L"ArchivePreviewerTemp";
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
    g_bCancelExtract = true;
    HANDLE hThread = g_hExtractThread;
    g_hExtractThread = NULL;
    LeaveCriticalSection(&g_csTempExtract);

    if (hThread) {
        WaitForSingleObject(hThread, INFINITE);
        CloseHandle(hThread);
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

LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_CREATE: {
            g_hPathEdit = CreateWindowExW(0, L"EDIT", L"", 
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
                10, 10, 400, 25, hWnd, (HMENU)IDC_PATH_EDIT, NULL, NULL);

            g_hBrowseBtn = CreateWindowExW(0, L"BUTTON", L"Browse...", 
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                420, 10, 100, 25, hWnd, (HMENU)IDC_BROWSE_BTN, NULL, NULL);

            g_hViewToggleBtn = CreateWindowExW(0, L"BUTTON", L"Grid View", 
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                530, 10, 100, 25, hWnd, (HMENU)IDC_VIEW_TOGGLE_BTN, NULL, NULL);

            HWND hSettingsBtn = CreateWindowExW(0, L"BUTTON", L"Settings", 
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                640, 10, 100, 25, hWnd, (HMENU)IDC_SETTINGS_BTN, NULL, NULL);

            g_hExtractBtn = CreateWindowExW(0, L"BUTTON", L"Extract Selected", 
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                750, 10, 150, 25, hWnd, (HMENU)IDC_EXTRACT_BTN, NULL, NULL);

            g_hArchiveList = CreateWindowExW(0, L"LISTBOX", NULL,
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_OWNERDRAWFIXED | LBS_NOTIFY | WS_BORDER,
                10, 45, 340, 500, hWnd, (HMENU)IDC_ARCHIVE_LIST, NULL, NULL);
            SendMessage(g_hArchiveList, LB_SETITEMHEIGHT, 0, 80);

            g_hContentsList = CreateWindowExW(0, L"LISTBOX", NULL,
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER | LBS_NOTIFY,
                360, 45, 510, 500, hWnd, (HMENU)IDC_CONTENTS_LIST, NULL, NULL);

            g_hGridViewWnd = CreateWindowExW(0, L"ArchiveGridViewClass", NULL,
                WS_CHILD | WS_VSCROLL | WS_BORDER,
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

            MoveWindow(g_hPathEdit, 10, 10, (width - 480 > 100) ? (width - 480) : 100, 25, TRUE);
            MoveWindow(g_hBrowseBtn, width - 460, 10, 90, 25, TRUE);
            MoveWindow(g_hViewToggleBtn, width - 360, 10, 90, 25, TRUE);
            HWND hSettings = GetDlgItem(hWnd, IDC_SETTINGS_BTN);
            MoveWindow(hSettings, width - 260, 10, 90, 25, TRUE);
            MoveWindow(g_hExtractBtn, width - 160, 10, 150, 25, TRUE);

            int leftWidth = g_splitPos - 10;
            int rightX = g_splitPos + 10;
            int rightWidth = width - g_splitPos - 20;

            MoveWindow(g_hArchiveList, 10, 45, leftWidth, height - 60, TRUE);
            MoveWindow(g_hContentsList, rightX, 45, rightWidth, height - 60, TRUE);
            if (g_hGridViewWnd) {
                MoveWindow(g_hGridViewWnd, rightX, 45, rightWidth, height - 60, TRUE);
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
                    g_splitPos = x - 5;
                    SendMessage(hWnd, WM_SIZE, 0, MAKELPARAM(rect.right, rect.bottom));
                    InvalidateRect(hWnd, NULL, TRUE);
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

Bitmap* LoadImageFromMemory(const char* data, size_t size) {
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

    Bitmap* pBitmap = Bitmap::FromStream(pStream);
    pStream->Release();
    
    if (pBitmap && pBitmap->GetLastStatus() == Ok) {
        return pBitmap;
    }
    
    delete pBitmap;
    return nullptr;
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
        if (!g_hScanThread) {
            delete params;
        }
    }
    
    LeaveCriticalSection(&g_csArchives);
}

void UpdateContentsList(int archiveIndex) {
    if (!g_hContentsList) return;
    SendMessage(g_hContentsList, LB_RESETCONTENT, 0, 0);

    EnterCriticalSection(&g_csArchives);
    if (archiveIndex < 0 || archiveIndex >= (int)g_archives.size()) {
        LeaveCriticalSection(&g_csArchives);
        return;
    }

    auto& archive = g_archives[archiveIndex];
    
    // If archive is not yet loaded/scanned (internalFiles is empty)
    if (archive.internalFiles.empty()) {
        std::wstring filePath = archive.filePath;
        LeaveCriticalSection(&g_csArchives); // Release critical section to avoid deadlocks

        ArchiveReader reader;
        bool opened = false;
        std::wstring enteredPwd;

        if (reader.Open(filePath)) {
            opened = true;
        } else if (reader.IsEncrypted()) {
            if (OpenArchiveWithPasswordSupport(reader, filePath, g_hMainWnd, enteredPwd)) {
                opened = true;
            }
        }

        if (opened) {
            std::vector<ArchiveFileInfo> files = reader.ListFiles();
            bool isEncrypted = reader.IsEncrypted();
            
            // Extract thumbnail
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
                    Bitmap* pOrig = LoadImageFromMemory(buffer.data(), buffer.size());
                    if (pOrig) {
                        thumbnail = CreateThumbnail(pOrig, 64, 64);
                        delete pOrig;
                    }
                }
            }
            reader.Close();
            
            EnterCriticalSection(&g_csArchives);
            // Verify path is still valid
            if (archiveIndex < (int)g_archives.size() && g_archives[archiveIndex].filePath == filePath) {
                g_archives[archiveIndex].isEncrypted = isEncrypted;
                g_archives[archiveIndex].currentPassword = enteredPwd;
                g_archives[archiveIndex].internalFiles = files;
                if (thumbnail) {
                    if (g_archives[archiveIndex].thumbnail) delete g_archives[archiveIndex].thumbnail;
                    g_archives[archiveIndex].thumbnail = thumbnail;
                }
                
                // Force redraw the listbox item
                RECT rect;
                SendMessage(g_hArchiveList, LB_GETITEMRECT, archiveIndex, (LPARAM)&rect);
                InvalidateRect(g_hArchiveList, &rect, TRUE);
            } else {
                if (thumbnail) delete thumbnail;
            }
        } else {
            EnterCriticalSection(&g_csArchives);
            SendMessage(g_hArchiveList, LB_SETCURSEL, (WPARAM)-1, 0);
            LeaveCriticalSection(&g_csArchives);
            return;
        }
    }

    // Trigger background pre-extraction if not already started
    std::wstring filePath = archive.filePath;
    std::wstring password = archive.currentPassword;
    std::wstring tempDirToCreate = L"";
    bool needStartExtraction = false;

    EnterCriticalSection(&g_csTempExtract);
    if (archive.tempExtractDir.empty()) {
        needStartExtraction = true;
        std::wstring tempRoot = GetAppTempRoot();
        std::wstring stemName = fs::path(filePath).stem().wstring();
        for (auto& ch : stemName) {
            if (ch == L' ' || ch == L'.' || ch == L',' || ch == L'[' || ch == L']' || ch == L'(' || ch == L')') {
                ch = L'_';
            }
        }
        tempDirToCreate = tempRoot + L"\\AP_Ext_" + std::to_wstring(GetTickCount64()) + L"_" + stemName;
    }
    LeaveCriticalSection(&g_csTempExtract);

    if (needStartExtraction) {
        // Create the directory
        fs::create_directories(tempDirToCreate);

        // Update the archive item under g_csArchives
        archive.tempExtractDir = tempDirToCreate;
        archive.extractionFinished = false;

        // Cancel previous extraction thread. We release g_csArchives before waiting to avoid deadlocks!
        LeaveCriticalSection(&g_csArchives);

        EnterCriticalSection(&g_csTempExtract);
        g_tempDirsCreated.push_back(tempDirToCreate);
        g_bCancelExtract = true;
        HANDLE hPrev = g_hExtractThread;
        g_hExtractThread = NULL;
        g_currentArchiveExtracting.clear();
        LeaveCriticalSection(&g_csTempExtract);

        if (hPrev) {
            WaitForSingleObject(hPrev, INFINITE);
            CloseHandle(hPrev);
        }

        EnterCriticalSection(&g_csTempExtract);
        g_bCancelExtract = false;
        g_currentArchiveExtracting = filePath;

        ExtractThreadParams* extParams = new ExtractThreadParams();
        extParams->archivePath = filePath;
        extParams->password = password;
        extParams->tempDir = tempDirToCreate;
        extParams->archiveIndex = archiveIndex;

        g_hExtractThread = CreateThread(NULL, 0, ExtractArchiveThread, extParams, 0, NULL);
        if (!g_hExtractThread) {
            delete extParams;
            g_currentArchiveExtracting.clear();
        }
        LeaveCriticalSection(&g_csTempExtract);

        // Re-enter g_csArchives
        EnterCriticalSection(&g_csArchives);
        
        // Need to re-bind archive reference after releasing/re-entering critical section
        if (archiveIndex < 0 || archiveIndex >= (int)g_archives.size() || g_archives[archiveIndex].filePath != filePath) {
            LeaveCriticalSection(&g_csArchives);
            return;
        }
    }

    void UpdateGridItems(const std::vector<ArchiveFileInfo>& files, const std::wstring& tempDir);
    UpdateGridItems(archive.internalFiles, archive.tempExtractDir);

    for (const auto& file : archive.internalFiles) {
        std::wstring itemText = file.name;
        if (file.isDirectory) {
            itemText += L"  [Directory]";
        } else {
            wchar_t sizeStr[64];
            swprintf_s(sizeStr, L"  (%u KB)", (unsigned int)(file.fileSize / 1024));
            itemText += sizeStr;
        }
        SendMessageW(g_hContentsList, LB_ADDSTRING, 0, (LPARAM)itemText.c_str());
    }
    LeaveCriticalSection(&g_csArchives);
}

bool WaitForExtraction(int archiveIndex, HWND hwndParent) {
    EnterCriticalSection(&g_csArchives);
    if (archiveIndex < 0 || archiveIndex >= (int)g_archives.size()) {
        LeaveCriticalSection(&g_csArchives);
        return false;
    }
    bool finished = g_archives[archiveIndex].extractionFinished;
    LeaveCriticalSection(&g_csArchives);

    if (finished) return true;

    HCURSOR hOldCursor = SetCursor(LoadCursor(NULL, IDC_WAIT));
    
    DWORD start = GetTickCount();
    while (GetTickCount() - start < 5000) {
        EnterCriticalSection(&g_csArchives);
        finished = g_archives[archiveIndex].extractionFinished;
        LeaveCriticalSection(&g_csArchives);

        if (finished) break;

        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        Sleep(50);
    }
    SetCursor(hOldCursor);
    return finished;
}

void OpenArchiveFile(int archiveIndex, int fileIndex) {
    WaitForExtraction(archiveIndex, g_hMainWnd);

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

    // 1. Check if the file is ready in the pre-extracted temp directory
    std::wstring preExtPath = tempExtractDir + L"\\" + fileName;
    for (auto& ch : preExtPath) {
        if (ch == L'/') ch = L'\\';
    }

    bool fileReady = false;
    if (!tempExtractDir.empty()) {
        for (int i = 0; i < 50; ++i) { // Wait up to 5 seconds
            if (fs::exists(preExtPath)) {
                std::error_code ec;
                auto sz = fs::file_size(preExtPath, ec);
                if (!ec && sz > 0) {
                    fileReady = true;
                    break;
                }
            }
            Sleep(100);
        }
    }

    if (fileReady) {
        ShellExecuteW(NULL, L"open", preExtPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
        return;
    }

    // 2. Fallback: Extract file singly to temp folder (same as before)
    std::wstring appTempDir = GetAppTempRoot() + L"\\SingleExtract";
    fs::create_directories(appTempDir);
    std::wstring destPath = appTempDir + L"\\" + fs::path(fileName).filename().wstring();

    ArchiveReader reader;
    if (reader.Open(filePath, currentPassword)) {
        bool success = reader.ExtractFileToDisk(internalPath, destPath);
        reader.Close();
        if (success) {
            ShellExecuteW(NULL, L"open", destPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
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

CRITICAL_SECTION g_csGridItems;
struct GridItemCache {
    std::wstring name;
    std::wstring fullDiskPath;
    Gdiplus::Bitmap* pBitmap;
    bool loadAttempted;
};
std::vector<GridItemCache> g_gridItems;
int g_selectedGridIndex = -1;
int g_hoverGridIndex = -1;
int g_gridScrollPos = 0;

struct GridLoadThreadParams {
    std::wstring tempDir;
};

// Background thumbnail loader thread function
DWORD WINAPI GridThumbnailLoaderThread(LPVOID lpParam) {
    GridLoadThreadParams* params = (GridLoadThreadParams*)lpParam;
    std::wstring tempDir = params->tempDir;
    delete params;

    while (true) {
        EnterCriticalSection(&g_csGridLoad);
        bool cancel = g_bCancelGridLoad;
        LeaveCriticalSection(&g_csGridLoad);

        if (cancel) break;

        // Find next item to load
        EnterCriticalSection(&g_csGridItems);
        size_t total = g_gridItems.size();
        GridItemCache* pItem = nullptr;
        int itemIndex = -1;
        
        for (size_t i = 0; i < total; ++i) {
            if (!g_gridItems[i].pBitmap && !g_gridItems[i].loadAttempted) {
                pItem = &g_gridItems[i];
                itemIndex = (int)i;
                break;
            }
        }
        LeaveCriticalSection(&g_csGridItems);

        if (!pItem) {
            Sleep(100);
            
            bool extractionFinished = false;
            int archiveIndex = (int)SendMessageW(g_hArchiveList, LB_GETCURSEL, 0, 0);
            if (archiveIndex >= 0) {
                EnterCriticalSection(&g_csArchives);
                extractionFinished = g_archives[archiveIndex].extractionFinished;
                LeaveCriticalSection(&g_csArchives);
            }
            
            EnterCriticalSection(&g_csGridItems);
            bool allAttempted = true;
            for (const auto& item : g_gridItems) {
                if (!item.pBitmap && !item.loadAttempted) {
                    allAttempted = false;
                    break;
                }
            }
            LeaveCriticalSection(&g_csGridItems);
            
            if (extractionFinished && allAttempted) {
                break;
            }
            continue;
        }

        if (fs::exists(pItem->fullDiskPath)) {
            std::error_code ec;
            auto sz = fs::file_size(pItem->fullDiskPath, ec);
            if (!ec && sz > 0) {
                Gdiplus::Bitmap* pOrig = Gdiplus::Bitmap::FromFile(pItem->fullDiskPath.c_str());
                Gdiplus::Bitmap* pThumb = nullptr;
                if (pOrig && pOrig->GetLastStatus() == Gdiplus::Ok) {
                    pThumb = CreateThumbnail(pOrig, 100, 100);
                    delete pOrig;
                } else {
                    if (pOrig) delete pOrig;
                }

                EnterCriticalSection(&g_csGridItems);
                if (itemIndex < (int)g_gridItems.size() && g_gridItems[itemIndex].name == pItem->name) {
                    if (pThumb) {
                        g_gridItems[itemIndex].pBitmap = pThumb;
                    } else {
                        g_gridItems[itemIndex].loadAttempted = true;
                    }
                } else {
                    if (pThumb) delete pThumb;
                }
                LeaveCriticalSection(&g_csGridItems);

                if (g_hGridViewWnd) {
                    InvalidateRect(g_hGridViewWnd, NULL, FALSE);
                }
            } else {
                Sleep(50);
            }
        } else {
            Sleep(100);
        }
    }
    return 0;
}

void UpdateGridItems(const std::vector<ArchiveFileInfo>& files, const std::wstring& tempDir) {
    EnterCriticalSection(&g_csGridLoad);
    g_bCancelGridLoad = true;
    HANDLE hPrevLoad = g_hGridLoadThread;
    g_hGridLoadThread = NULL;
    LeaveCriticalSection(&g_csGridLoad);

    if (hPrevLoad) {
        WaitForSingleObject(hPrevLoad, INFINITE);
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
            item.fullDiskPath = tempDir + L"\\" + file.name;
            for (auto& ch : item.fullDiskPath) {
                if (ch == L'/') ch = L'\\';
            }
            item.pBitmap = nullptr;
            item.loadAttempted = false;
            g_gridItems.push_back(item);
        }
    }
    LeaveCriticalSection(&g_csGridItems);

    EnterCriticalSection(&g_csGridLoad);
    g_bCancelGridLoad = false;
    GridLoadThreadParams* pLoadParams = new GridLoadThreadParams();
    pLoadParams->tempDir = tempDir;
    g_hGridLoadThread = CreateThread(NULL, 0, GridThumbnailLoaderThread, pLoadParams, 0, NULL);
    if (!g_hGridLoadThread) {
        delete pLoadParams;
    }
    LeaveCriticalSection(&g_csGridLoad);
    
    if (g_hGridViewWnd) {
        SCROLLINFO si = { 0 };
        si.cbSize = sizeof(si);
        si.fMask = SIF_POS | SIF_RANGE;
        si.nPos = 0;
        si.nMin = 0;
        si.nMax = 0;
        SetScrollInfo(g_hGridViewWnd, SB_VERT, &si, TRUE);
        InvalidateRect(g_hGridViewWnd, NULL, TRUE);
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

            int cardWidth = 120;
            int cardHeight = 140;
            int margin = 15;
            int cols = (width - margin) / (cardWidth + margin);
            if (cols < 1) cols = 1;

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

            int cardWidth = 120;
            int cardHeight = 140;
            int margin = 15;
            int cols = (width - margin) / (cardWidth + margin);
            if (cols < 1) cols = 1;

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

            int cardWidth = 120;
            int cardHeight = 140;
            int margin = 15;
            int cols = (width - margin) / (cardWidth + margin);
            if (cols < 1) cols = 1;

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

            int cardWidth = 120;
            int cardHeight = 140;
            int margin = 15;
            int cols = (width - margin) / (cardWidth + margin);
            if (cols < 1) cols = 1;

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
