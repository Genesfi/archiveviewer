#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <thumbcache.h>
#include <gdiplus.h>
#include <vector>
#include <string>
#include <algorithm>
#include <stdarg.h>
#include "archive_reader.h"
#include "resource.h"

using namespace Gdiplus;

// CLSID: {C3861219-5E79-4B52-8706-03761D98357F}
const CLSID CLSID_ArchiveThumbnailProvider = { 0xC3861219, 0x5E79, 0x4B52, { 0x87, 0x06, 0x03, 0x76, 0x1D, 0x98, 0x35, 0x7F } };

// Global DLL references count
long g_cDllRefs = 0;
HINSTANCE g_hInst = NULL;

void LogDebug(const char* format, ...) {
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    std::wstring logFilePath = std::wstring(tempPath) + L"ArchivePreviewerDebug.txt";
    
    FILE* f = _wfopen(logFilePath.c_str(), L"a");
    if (!f) return;
    
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    
    va_list args;
    va_start(args, format);
    vfprintf(f, format, args);
    va_end(args);
    
    fprintf(f, "\n");
    fclose(f);
}

// Critical Section for GDI+ Thread-safety
CRITICAL_SECTION g_csGdiplus;
bool g_bCsInitialized = false;
long g_cGdiplusRefs = 0;
ULONG_PTR g_gdiplusToken = 0;

// Helper to safely startup/shutdown GDI+ across multiple explorer threads
class GdiplusScope {
public:
    GdiplusScope() {
        if (g_bCsInitialized) {
            EnterCriticalSection(&g_csGdiplus);
            if (g_cGdiplusRefs == 0) {
                Gdiplus::GdiplusStartupInput gdiplusStartupInput;
                Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, NULL);
            }
            g_cGdiplusRefs++;
            LeaveCriticalSection(&g_csGdiplus);
        }
    }

    ~GdiplusScope() {
        if (g_bCsInitialized) {
            EnterCriticalSection(&g_csGdiplus);
            g_cGdiplusRefs--;
            if (g_cGdiplusRefs == 0) {
                Gdiplus::GdiplusShutdown(g_gdiplusToken);
                g_gdiplusToken = 0;
            }
            LeaveCriticalSection(&g_csGdiplus);
        }
    }
};

// Helper functions for image loading/scaling
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

Gdiplus::Bitmap* LoadImageFromMemory(const char* data, size_t size) {
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

    Gdiplus::Bitmap* pBitmap = Gdiplus::Bitmap::FromStream(pStream);
    pStream->Release();
    
    if (pBitmap && pBitmap->GetLastStatus() == Gdiplus::Ok) {
        return pBitmap;
    }
    
    delete pBitmap;
    return nullptr;
}

Gdiplus::Bitmap* CreateThumbnail(Gdiplus::Bitmap* pSource, int thumbWidth, int thumbHeight, const std::wstring& filePath) {
    Gdiplus::Bitmap* pThumb = new Gdiplus::Bitmap(thumbWidth, thumbHeight, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics graphics(pThumb);
        graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
        
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        
        int srcWidth = pSource->GetWidth();
        int srcHeight = pSource->GetHeight();
        
        float ratioX = (float)thumbWidth / srcWidth;
        float ratioY = (float)thumbHeight / srcHeight;
        float ratio = (std::min)(ratioX, ratioY);
        
        int newWidth = (int)(srcWidth * ratio);
        int newHeight = (int)(srcHeight * ratio);
        
        int posX = (thumbWidth - newWidth) / 2;
        int posY = (thumbHeight - newHeight) / 2;
        
        graphics.DrawImage(pSource, posX, posY, newWidth, newHeight);

        // Draw the badge ON TOP of the actual image content boundary (so it doesn't get cropped out by Windows Explorer!)
        if (newWidth > 0 && newHeight > 0) {
            // Calculate badge dimensions dynamically based on actual scaled image width (newWidth)
            int badgeWidth = (int)(newWidth * 0.22f);
            int badgeHeight = (int)(newWidth * 0.11f);
            if (badgeWidth < 32) badgeWidth = 32;
            if (badgeHeight < 16) badgeHeight = 16;
            if (badgeWidth > 60) badgeWidth = 60;
            if (badgeHeight > 26) badgeHeight = 26;

            // Draw it at the top-left of the actual image content
            int badgeX = posX + (int)(newWidth * 0.04f);
            int badgeY = posY + (int)(newWidth * 0.04f);

            // Draw badge background: Indigo theme with 86% opacity
            Gdiplus::SolidBrush badgeBrush(Gdiplus::Color(220, 79, 70, 229)); 
            graphics.FillRectangle(&badgeBrush, badgeX, badgeY, badgeWidth, badgeHeight);

            // Draw a thin white border around the badge to make it pop
            Gdiplus::Pen borderPen(Gdiplus::Color(255, 255, 255, 255), 1.0f);
            graphics.DrawRectangle(&borderPen, badgeX, badgeY, badgeWidth, badgeHeight);

            // Determine badge text based on extension
            std::wstring ext;
            size_t dotPos = filePath.find_last_of(L'.');
            if (dotPos != std::wstring::npos) {
                ext = filePath.substr(dotPos);
            }
            for (auto& ch : ext) {
                ch = towupper(ch);
            }
            
            std::wstring badgeText = L"ZIP";
            if (ext == L".RAR") badgeText = L"RAR";
            else if (ext == L".7Z") badgeText = L"7Z";
            else if (!ext.empty() && ext[0] == L'.') {
                badgeText = ext.substr(1);
            }

            // Draw the text centered inside the badge
            Gdiplus::FontFamily fontFamily(L"Segoe UI");
            int fontSize = (int)(badgeHeight * 0.6f);
            if (fontSize < 10) fontSize = 10;
            
            Gdiplus::Font font(&fontFamily, (REAL)fontSize, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 255, 255, 255));

            Gdiplus::StringFormat stringFormat;
            stringFormat.SetAlignment(Gdiplus::StringAlignmentCenter);
            stringFormat.SetLineAlignment(Gdiplus::StringAlignmentCenter);

            Gdiplus::RectF layoutRect((REAL)badgeX, (REAL)badgeY, (REAL)badgeWidth, (REAL)badgeHeight);
            graphics.DrawString(badgeText.c_str(), -1, &font, layoutRect, &stringFormat, &textBrush);
        }
    }
    return pThumb;
}

// -------------------------------------------------------------------------
// ArchiveThumbnailProvider Class
// -------------------------------------------------------------------------
class ArchiveThumbnailProvider : public IInitializeWithStream, public IThumbnailProvider {
private:
    long m_cRef;
    std::wstring m_filePath;
    IStream* m_pStream;

public:
    ArchiveThumbnailProvider() : m_cRef(1), m_pStream(nullptr) {
        InterlockedIncrement(&g_cDllRefs);
    }

    ~ArchiveThumbnailProvider() {
        if (m_pStream) {
            m_pStream->Release();
        }
        InterlockedDecrement(&g_cDllRefs);
    }

    // IUnknown methods
    IFACEMETHODIMP QueryInterface(REFIID riid, void **ppv) {
        if (!ppv) return E_POINTER;
        *ppv = NULL;

        if (riid == IID_IUnknown) {
            *ppv = static_cast<IUnknown*>(static_cast<IInitializeWithStream*>(this));
        } else if (riid == IID_IInitializeWithStream) {
            *ppv = static_cast<IInitializeWithStream*>(this);
        } else if (riid == IID_IThumbnailProvider) {
            *ppv = static_cast<IThumbnailProvider*>(this);
        } else {
            return E_NOINTERFACE;
        }

        AddRef();
        return S_OK;
    }

    IFACEMETHODIMP_(ULONG) AddRef() {
        return InterlockedIncrement(&m_cRef);
    }

    IFACEMETHODIMP_(ULONG) Release() {
        ULONG cRef = InterlockedDecrement(&m_cRef);
        if (cRef == 0) {
            delete this;
        }
        return cRef;
    }

    // IInitializeWithStream methods
    IFACEMETHODIMP Initialize(IStream *pstream, DWORD grfMode) {
        LogDebug("Initialize called");
        if (m_pStream) {
            m_pStream->Release();
            m_pStream = nullptr;
        }
        
        m_pStream = pstream;
        if (m_pStream) {
            m_pStream->AddRef();
            
            // Try to query file path using Stat
            STATSTG stat = { 0 };
            if (SUCCEEDED(m_pStream->Stat(&stat, STATFLAG_DEFAULT)) && stat.pwcsName) {
                m_filePath = stat.pwcsName;
                CoTaskMemFree(stat.pwcsName);
            }
            LogDebug("Initialize: Stat filePath: %ls", m_filePath.c_str());
            
            // Fallback: try IPersistFile
            if (m_filePath.empty()) {
                IPersistFile* pPersistFile = nullptr;
                if (SUCCEEDED(m_pStream->QueryInterface(IID_IPersistFile, (void**)&pPersistFile))) {
                    LPOLESTR pszPath = nullptr;
                    if (SUCCEEDED(pPersistFile->GetCurFile(&pszPath)) && pszPath) {
                        m_filePath = pszPath;
                        CoTaskMemFree(pszPath);
                    }
                    pPersistFile->Release();
                }
                LogDebug("Initialize: IPersistFile filePath: %ls", m_filePath.c_str());
            }
        }
        return S_OK;
    }

    // IThumbnailProvider methods
    IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP *phbmp, WTS_ALPHATYPE *pdwAlpha) {
        LogDebug("GetThumbnail called. File: %ls, Target Size: %u", m_filePath.c_str(), cx);
        if (!phbmp || !pdwAlpha) return E_INVALIDARG;
        *phbmp = NULL;
        *pdwAlpha = WTSAT_UNKNOWN;

        if (!m_pStream) {
            LogDebug("GetThumbnail: m_pStream is NULL");
            return E_FAIL;
        }

        // 1. Open the archive from stream
        ArchiveReader reader;
        bool opened = false;
        
        std::wstring ext = L".ZIP";
        size_t dotPos = m_filePath.find_last_of(L'.');
        if (dotPos != std::wstring::npos) {
            ext = m_filePath.substr(dotPos);
        }
        LogDebug("GetThumbnail: Ext: %ls", ext.c_str());
        
        if (IsNonFirstMultiPartVolume(m_filePath)) {
            LogDebug("GetThumbnail: Skipping non-first multipart volume: %ls", m_filePath.c_str());
            return E_FAIL;
        }
        
        if (reader.OpenFromStream(m_pStream, ext)) {
            opened = true;
            LogDebug("GetThumbnail: OpenFromStream succeeded without password");
        } else {
            LogDebug("GetThumbnail: OpenFromStream failed, trying password store");
            std::vector<std::wstring> savedPwds = PasswordStore::LoadPasswords();
            LogDebug("GetThumbnail: Loaded %u saved passwords", savedPwds.size());
            for (const auto& pwd : savedPwds) {
                if (reader.OpenFromStream(m_pStream, ext, pwd)) {
                    opened = true;
                    LogDebug("GetThumbnail: OpenFromStream succeeded with saved password");
                    break;
                }
            }
        }

        // 2. Scan internal files to find the first image if opened successfully
        std::vector<char> buffer;
        bool hasImage = false;

        if (opened) {
            std::vector<ArchiveFileInfo> files = reader.ListFiles();
            LogDebug("GetThumbnail: Archive opened. File count: %u", files.size());
            std::string firstImageInternalPath;
            for (const auto& file : files) {
                if (!file.isDirectory && IsImageFile(file.name)) {
                    firstImageInternalPath = file.internalPath;
                    LogDebug("GetThumbnail: Found image item: %s", file.name.c_str());
                    break;
                }
            }

            // 3. Extract the image to a memory buffer if available
            if (!firstImageInternalPath.empty()) {
                if (reader.ExtractFileToMemory(firstImageInternalPath, buffer)) {
                    hasImage = true;
                    LogDebug("GetThumbnail: Successfully extracted image. Size: %u", buffer.size());
                } else {
                    LogDebug("GetThumbnail: Failed to extract image from archive");
                }
            } else {
                LogDebug("GetThumbnail: No image files found in archive");
            }
            reader.Close();
        } else {
            LogDebug("GetThumbnail: Failed to open archive");
        }

        // 4. Create thumbnail using GDI+
        GdiplusScope gdiScope;
        Gdiplus::Bitmap* pThumb = nullptr;

        if (hasImage) {
            Gdiplus::Bitmap* pOrig = LoadImageFromMemory(buffer.data(), buffer.size());
            if (pOrig) {
                LogDebug("GetThumbnail: Loaded GDI+ image. Dim: %ux%u", pOrig->GetWidth(), pOrig->GetHeight());
                pThumb = CreateThumbnail(pOrig, cx, cx, m_filePath);
                delete pOrig;
            } else {
                LogDebug("GetThumbnail: Failed to parse GDI+ image from memory buffer");
            }
        }

        // Fallback: If no image was found or image loading failed, load the transparent folder icon resource!
        if (!pThumb) {
            HICON hIcon = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, cx, cx, LR_DEFAULTCOLOR);
            if (hIcon) {
                Gdiplus::Bitmap* pIconBmp = Gdiplus::Bitmap::FromHICON(hIcon);
                if (pIconBmp) {
                    pThumb = CreateThumbnail(pIconBmp, cx, cx, m_filePath);
                    delete pIconBmp;
                }
                DestroyIcon(hIcon);
            }
        }

        HRESULT hr = E_FAIL;
        if (pThumb) {
            LogDebug("GetThumbnail: Calling GetHBITMAP...");
            Gdiplus::Status status = pThumb->GetHBITMAP(Gdiplus::Color(0, 0, 0, 0), phbmp);
            LogDebug("GetThumbnail: GetHBITMAP status: %d, HBITMAP: %p", status, *phbmp);
            if (status == Gdiplus::Ok) {
                *pdwAlpha = WTSAT_ARGB;
                hr = S_OK;
                LogDebug("GetThumbnail: Successfully returning S_OK");
            }
            delete pThumb;
        } else {
            LogDebug("GetThumbnail: pThumb is NULL");
        }

        return hr;
    }
};

// -------------------------------------------------------------------------
// ArchiveClassFactory Class
// -------------------------------------------------------------------------
class ArchiveClassFactory : public IClassFactory {
private:
    long m_cRef;

public:
    ArchiveClassFactory() : m_cRef(1) {
        InterlockedIncrement(&g_cDllRefs);
    }

    ~ArchiveClassFactory() {
        InterlockedDecrement(&g_cDllRefs);
    }

    // IUnknown methods
    IFACEMETHODIMP QueryInterface(REFIID riid, void **ppv) {
        if (!ppv) return E_POINTER;
        *ppv = NULL;

        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    IFACEMETHODIMP_(ULONG) AddRef() {
        return InterlockedIncrement(&m_cRef);
    }

    IFACEMETHODIMP_(ULONG) Release() {
        ULONG cRef = InterlockedDecrement(&m_cRef);
        if (cRef == 0) {
            delete this;
        }
        return cRef;
    }

    // IClassFactory methods
    IFACEMETHODIMP CreateInstance(IUnknown *pUnkOuter, REFIID riid, void **ppv) {
        if (pUnkOuter != NULL) return CLASS_E_NOAGGREGATION;

        ArchiveThumbnailProvider *pProvider = new ArchiveThumbnailProvider();
        HRESULT hr = pProvider->QueryInterface(riid, ppv);
        pProvider->Release();
        return hr;
    }

    IFACEMETHODIMP LockServer(BOOL fLock) {
        if (fLock) {
            InterlockedIncrement(&g_cDllRefs);
        } else {
            InterlockedDecrement(&g_cDllRefs);
        }
        return S_OK;
    }
};

// -------------------------------------------------------------------------
// COM DLL entry points
// -------------------------------------------------------------------------

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH:
            g_hInst = hModule;
            DisableThreadLibraryCalls(hModule);
            InitializeCriticalSection(&g_csGdiplus);
            g_bCsInitialized = true;
            break;

        case DLL_PROCESS_DETACH:
            g_bCsInitialized = false;
            DeleteCriticalSection(&g_csGdiplus);
            break;
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void **ppv) {
    if (!ppv) return E_POINTER;
    *ppv = NULL;

    if (rclsid == CLSID_ArchiveThumbnailProvider) {
        ArchiveClassFactory *pFactory = new ArchiveClassFactory();
        HRESULT hr = pFactory->QueryInterface(riid, ppv);
        pFactory->Release();
        return hr;
    }
    return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow() {
    return (g_cDllRefs == 0) ? S_OK : S_FALSE;
}
