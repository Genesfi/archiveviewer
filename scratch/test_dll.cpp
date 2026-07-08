#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <thumbcache.h>
#include <gdiplus.h>
#include <shlwapi.h>
#include <iostream>
#include <string>

using namespace Gdiplus;

// CLSID: {C3861219-5E79-4B52-8706-03761D98357F}
const CLSID CLSID_ArchiveThumbnailProvider = { 0xC3861219, 0x5E79, 0x4B52, { 0x87, 0x06, 0x03, 0x76, 0x1D, 0x98, 0x35, 0x7F } };

// Helper to save HBITMAP to file using GDI+
int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
    UINT num = 0;
    UINT size = 0;
    GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    ImageCodecInfo* pImageCodecInfo = (ImageCodecInfo*)(malloc(size));
    if (pImageCodecInfo == NULL) return -1;
    GetImageEncoders(num, size, pImageCodecInfo);
    for (UINT j = 0; j < num; ++j) {
        if (wcscmp(pImageCodecInfo[j].MimeType, format) == 0) {
            *pClsid = pImageCodecInfo[j].Clsid;
            free(pImageCodecInfo);
            return j;
        }
    }
    free(pImageCodecInfo);
    return -1;
}

#include "archive_reader.h"

int main() {
    // Initialize COM and GDI+
    CoInitialize(NULL);
    ULONG_PTR gdiToken;
    GdiplusStartupInput gdiInput;
    GdiplusStartup(&gdiToken, &gdiInput, NULL);

    std::cout << "=== Testing ArchiveReader directly ===\n";
    std::wstring path = L"C:\\Users\\Gusti N\\Desktop\\test_badge_archive.zip";
    DWORD attr = GetFileAttributesW(path.c_str());
    std::cout << "Attributes: " << std::hex << attr << ", LastError: " << std::dec << GetLastError() << "\n";

    ArchiveReader directReader;
    if (directReader.Open(path)) {
        std::cout << "Direct Open: SUCCESS!\n";
        auto list = directReader.ListFiles();
        std::cout << "Files inside: " << list.size() << "\n";
        for (const auto& item : list) {
            std::wcout << L"  - " << item.name << L"\n";
        }
        directReader.Close();
    } else {
        std::cout << "Direct Open: FAILED!\n";
    }
    std::cout << "======================================\n\n";

    std::wstring dllPath = L"ArchiveThumbnailProvider.dll";
    HMODULE hDll = LoadLibraryW(dllPath.c_str());
    if (!hDll) {
        std::cout << "Failed to load DLL. Error: " << GetLastError() << "\n";
        return 1;
    }

    typedef HRESULT (WINAPI *DllGetClassObjectFunc)(REFCLSID, REFIID, void**);
    DllGetClassObjectFunc pDllGetClassObject = (DllGetClassObjectFunc)GetProcAddress(hDll, "DllGetClassObject");
    if (!pDllGetClassObject) {
        std::cout << "Failed to get DllGetClassObject\n";
        return 1;
    }

    IClassFactory* pFactory = nullptr;
    HRESULT hr = pDllGetClassObject(CLSID_ArchiveThumbnailProvider, IID_IClassFactory, (void**)&pFactory);
    if (FAILED(hr)) {
        std::cout << "Failed DllGetClassObject. HR: " << std::hex << hr << "\n";
        return 1;
    }

    IUnknown* pUnk = nullptr;
    hr = pFactory->CreateInstance(NULL, IID_IUnknown, (void**)&pUnk);
    if (FAILED(hr)) {
        std::cout << "Failed CreateInstance. HR: " << std::hex << hr << "\n";
        return 1;
    }

    IInitializeWithStream* pInit = nullptr;
    hr = pUnk->QueryInterface(IID_IInitializeWithStream, (void**)&pInit);
    if (FAILED(hr)) {
        std::cout << "Failed QI IInitializeWithStream. HR: " << std::hex << hr << "\n";
        return 1;
    }

    std::wstring targetFile = L"F:\\PF\\MV Aegis\\hand-drawn-flat-ui-kit-collection.zip";
    IStream* pStream = nullptr;
    HRESULT hrStream = SHCreateStreamOnFileEx(targetFile.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, FILE_ATTRIBUTE_NORMAL, FALSE, NULL, &pStream);
    if (FAILED(hrStream)) {
        std::cout << "Failed SHCreateStreamOnFileEx. HR: " << std::hex << hrStream << "\n";
        return 1;
    }

    hr = pInit->Initialize(pStream, STGM_READ);
    pStream->Release();
    if (FAILED(hr)) {
        std::cout << "Failed Initialize. HR: " << std::hex << hr << "\n";
        return 1;
    }

    IThumbnailProvider* pProvider = nullptr;
    hr = pUnk->QueryInterface(IID_IThumbnailProvider, (void**)&pProvider);
    if (FAILED(hr)) {
        std::cout << "Failed QI IThumbnailProvider. HR: " << std::hex << hr << "\n";
        return 1;
    }

    HBITMAP hBmp = NULL;
    WTS_ALPHATYPE alphaType = WTSAT_UNKNOWN;
    hr = pProvider->GetThumbnail(256, &hBmp, &alphaType);
    if (FAILED(hr)) {
        std::cout << "Failed GetThumbnail. HR: " << std::hex << hr << "\n";
        return 1;
    }

    std::cout << "GetThumbnail Succeeded! Alpha type: " << alphaType << "\n";

    if (hBmp) {
        Bitmap* pBmp = Bitmap::FromHBITMAP(hBmp, NULL);
        if (pBmp) {
            CLSID pngClsid;
            GetEncoderClsid(L"image/png", &pngClsid);
            std::wstring outPath = L"C:\\Users\\Gusti N\\.gemini\\antigravity-ide\\scratch\\archive-previewer\\scratch\\test_out.png";
            pBmp->Save(outPath.c_str(), &pngClsid, NULL);
            delete pBmp;
            std::cout << "Saved output image to " << "scratch/test_out.png\n";
        }
        DeleteObject(hBmp);
    }

    pProvider->Release();
    pInit->Release();
    pUnk->Release();
    pFactory->Release();
    FreeLibrary(hDll);

    GdiplusShutdown(gdiToken);
    CoUninitialize();
    return 0;
}
