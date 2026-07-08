#define NOMINMAX
#include <windows.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <algorithm>
#include <iostream>

using namespace Gdiplus;

// Link GDI+
#pragma comment(lib, "gdiplus.lib")

std::wstring Utf8ToWString(const std::string& str) {
    if (str.empty()) return L"";
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

Bitmap* CreateThumbnail(Bitmap* pSource, int thumbWidth, int thumbHeight, const std::wstring& filePath) {
    Bitmap* pThumb = new Bitmap(thumbWidth, thumbHeight, PixelFormat32bppARGB);
    {
        Graphics graphics(pThumb);
        graphics.Clear(Color(0, 0, 0, 0));
        
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
        
        graphics.DrawImage(pSource, posX, posY, newWidth, newHeight);

        // Draw the badge ON TOP
        if (newWidth > 0 && newHeight > 0) {
            int badgeWidth = (int)(newWidth * 0.22f);
            int badgeHeight = (int)(newWidth * 0.11f);
            if (badgeWidth < 32) badgeWidth = 32;
            if (badgeHeight < 16) badgeHeight = 16;
            if (badgeWidth > 60) badgeWidth = 60;
            if (badgeHeight > 26) badgeHeight = 26;

            int badgeX = posX + (int)(newWidth * 0.04f);
            int badgeY = posY + (int)(newWidth * 0.04f);

            SolidBrush badgeBrush(Color(220, 79, 70, 229)); 
            graphics.FillRectangle(&badgeBrush, badgeX, badgeY, badgeWidth, badgeHeight);

            Pen borderPen(Color(255, 255, 255, 255), 1.0f);
            graphics.DrawRectangle(&borderPen, badgeX, badgeY, badgeWidth, badgeHeight);

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

            FontFamily fontFamily(L"Segoe UI");
            int fontSize = (int)(badgeHeight * 0.6f);
            if (fontSize < 10) fontSize = 10;
            
            Font font(&fontFamily, (REAL)fontSize, FontStyleBold, UnitPixel);
            SolidBrush textBrush(Color(255, 255, 255, 255));

            StringFormat stringFormat;
            stringFormat.SetAlignment(StringAlignmentCenter);
            stringFormat.SetLineAlignment(StringAlignmentCenter);

            RectF layoutRect((REAL)badgeX, (REAL)badgeY, (REAL)badgeWidth, (REAL)badgeHeight);
            graphics.DrawString(badgeText.c_str(), -1, &font, layoutRect, &stringFormat, &textBrush);
        }
    }
    return pThumb;
}

int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
    UINT num = 0;
    UINT size = 0;
    GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    
    std::vector<ImageCodecInfo> imageCodecInfo(size / sizeof(ImageCodecInfo));
    GetImageEncoders(num, size, &imageCodecInfo[0]);
    
    for (UINT j = 0; j < num; ++j) {
        if (wcscmp(imageCodecInfo[j].MimeType, format) == 0) {
            *pClsid = imageCodecInfo[j].Clsid;
            return j;
        }
    }
    return -1;
}

int main() {
    // Initialize GDI+
    ULONG_PTR gdiplusToken;
    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    // Load original image
    Bitmap* pOrig = Bitmap::FromFile(L"C:\\Users\\Gusti N\\.gemini\\antigravity-ide\\brain\\1b4ebed3-d94b-49b7-89a6-4f22e45ab03d\\test_image_1783514377912.png");
    if (!pOrig || pOrig->GetLastStatus() != Ok) {
        std::cout << "Failed to load original image!" << std::endl;
        GdiplusShutdown(gdiplusToken);
        return 1;
    }

    std::cout << "Original loaded. Width: " << pOrig->GetWidth() << ", Height: " << pOrig->GetHeight() << std::endl;

    // Create 96x96 thumbnail
    Bitmap* pThumb = CreateThumbnail(pOrig, 96, 96, L"test_archive.zip");
    delete pOrig;

    if (!pThumb || pThumb->GetLastStatus() != Ok) {
        std::cout << "Failed to create thumbnail!" << std::endl;
        GdiplusShutdown(gdiplusToken);
        return 1;
    }

    // Save thumbnail to disk
    CLSID pngClsid;
    GetEncoderClsid(L"image/png", &pngClsid);
    Status status = pThumb->Save(L"C:\\Users\\Gusti N\\.gemini\\antigravity-ide\\scratch\\archive-previewer\\scratch\\test_output.png", &pngClsid, NULL);
    delete pThumb;

    if (status == Ok) {
        std::cout << "Saved test_output.png successfully!" << std::endl;
    } else {
        std::cout << "Failed to save test_output.png. GDI+ Status: " << status << std::endl;
    }

    GdiplusShutdown(gdiplusToken);
    return 0;
}
