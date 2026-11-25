#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <d2d1.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kWndClass[] = L"PhotoWidgetIosCardWnd";
constexpr wchar_t kRunRegPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValueName[] = L"PhotoWidget";
constexpr UINT_PTR kTimerGif = 1;
constexpr int kMinSize = 80;
constexpr int kResizeBorder = 10;

enum MenuCmd : UINT {
    kCmdChangePhoto = 1001,
    kCmdDuplicate = 1002,
    kCmdLock = 1003,
    kCmdAutoStart = 1004,
    kCmdRemove = 1005,
    kCmdExitAll = 1006,
    kCmdSizeSmallSq = 1101,
    kCmdSizeMedSq = 1102,
    kCmdSizeLargeSq = 1103,
    kCmdSizeWide = 1104,
    kCmdSizeTall = 1105,
};

struct ComPtrHelper {
    template <typename T>
    static void SafeRelease(T*& p) {
        if (p) {
            p->Release();
            p = nullptr;
        }
    }
};

struct GifFrame {
    std::vector<uint32_t> pixels;  // premultiplied BGRA
    UINT left = 0;
    UINT top = 0;
    UINT w = 0;
    UINT h = 0;
    UINT delayMs = 100;
    uint8_t disposal = 0;
};

struct WidgetCard {
    HWND hwnd = nullptr;
    int x = 120;
    int y = 120;
    int w = 210;
    int h = 210;
    bool locked = false;
    std::wstring photoPath;
    std::wstring loadedPath;
    IWICBitmap* cachedBitmap = nullptr;
    UINT imgW = 0;
    UINT imgH = 0;

    std::vector<GifFrame> gifFrames;
    std::vector<uint32_t> gifCanvas;
    std::vector<uint32_t> gifPrevCanvas;
    size_t gifIndex = 0;
    UINT gifPrevLeft = 0;
    UINT gifPrevTop = 0;
    UINT gifPrevW = 0;
    UINT gifPrevH = 0;
    uint8_t gifPrevDisposal = 0;

    bool dragging = false;
    POINT dragStartScreen{};
    POINT dragStartWnd{};

    ~WidgetCard() { ComPtrHelper::SafeRelease(cachedBitmap); }
};

HINSTANCE g_hInst = nullptr;
HWND g_desktopOwner = nullptr;
IWICImagingFactory* g_wicFactory = nullptr;
ID2D1Factory* g_d2dFactory = nullptr;
std::vector<std::unique_ptr<WidgetCard>> g_cards;
std::wstring g_iniPath;
std::wstring g_exePath;

using FnAllowDarkModeForWindow = BOOL(WINAPI*)(HWND, BOOL);
using FnSetPreferredAppMode = int(WINAPI*)(int);
using FnFlushMenuThemes = void(WINAPI*)();
using FnSetWindowTheme = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);

FnAllowDarkModeForWindow g_allowDarkWnd = nullptr;
FnSetWindowTheme g_setWindowTheme = nullptr;

void InitDarkMenus() {
    HMODULE ux = LoadLibraryW(L"uxtheme.dll");
    if (!ux) return;
    g_allowDarkWnd = reinterpret_cast<FnAllowDarkModeForWindow>(
        reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(133))));
    g_setWindowTheme = reinterpret_cast<FnSetWindowTheme>(
        reinterpret_cast<void*>(GetProcAddress(ux, "SetWindowTheme")));
    if (auto setMode = reinterpret_cast<FnSetPreferredAppMode>(
            reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(135))))) {
        setMode(2);  // ForceDark so Win32 context menus render in native dark mode
    }
    if (auto flush = reinterpret_cast<FnFlushMenuThemes>(
            reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(136))))) {
        flush();
    }
}

std::wstring GetExeDirPath() {
    WCHAR buf[MAX_PATH]{};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    g_exePath.assign(buf, n);
    PathRemoveFileSpecW(buf);
    return buf;
}

HWND FindDesktopOwnerWindow() {
    HWND progman = FindWindowW(L"Progman", L"Program Manager");
    if (!progman) progman = FindWindowW(L"Progman", nullptr);
    return progman;
}

bool IsAutoStartEnabled() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegPath, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD size = 0;
    bool exists = (RegQueryValueExW(hKey, kRunValueName, nullptr, &type, nullptr, &size) == ERROR_SUCCESS);
    RegCloseKey(hKey);
    return exists;
}

void SetAutoStartEnabled(bool enable) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegPath, 0, KEY_WRITE, &hKey) != ERROR_SUCCESS) {
        return;
    }
    if (enable && !g_exePath.empty()) {
        std::wstring quoted = L"\"" + g_exePath + L"\"";
        RegSetValueExW(hKey, kRunValueName, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(quoted.c_str()),
                       static_cast<DWORD>((quoted.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(hKey, kRunValueName);
    }
    RegCloseKey(hKey);
}

void SaveConfig() {
    if (g_iniPath.empty()) return;
    DeleteFileW(g_iniPath.c_str());

    WritePrivateProfileStringW(L"App", L"Count", std::to_wstring(g_cards.size()).c_str(), g_iniPath.c_str());
    for (size_t i = 0; i < g_cards.size(); ++i) {
        auto const& c = *g_cards[i];
        std::wstring sec = L"Widget" + std::to_wstring(i);
        WritePrivateProfileStringW(sec.c_str(), L"X", std::to_wstring(c.x).c_str(), g_iniPath.c_str());
        WritePrivateProfileStringW(sec.c_str(), L"Y", std::to_wstring(c.y).c_str(), g_iniPath.c_str());
        WritePrivateProfileStringW(sec.c_str(), L"W", std::to_wstring(c.w).c_str(), g_iniPath.c_str());
        WritePrivateProfileStringW(sec.c_str(), L"H", std::to_wstring(c.h).c_str(), g_iniPath.c_str());
        WritePrivateProfileStringW(sec.c_str(), L"Locked", c.locked ? L"1" : L"0", g_iniPath.c_str());
        WritePrivateProfileStringW(sec.c_str(), L"Photo", c.photoPath.c_str(), g_iniPath.c_str());
    }
}

UINT ReadMetaUInt(IWICMetadataQueryReader* reader, LPCWSTR name, UINT defVal) {
    if (!reader) return defVal;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    UINT out = defVal;
    if (SUCCEEDED(reader->GetMetadataByName(name, &pv))) {
        if (pv.vt == VT_UI2) out = pv.uiVal;
        else if (pv.vt == VT_UI1) out = pv.bVal;
        else if (pv.vt == VT_UI4) out = pv.ulVal;
    }
    PropVariantClear(&pv);
    return out;
}

void ComposeGifFrame(WidgetCard& card, size_t idx) {
    if (card.gifFrames.empty() || card.imgW == 0 || card.imgH == 0) return;

    if (idx == 0) {
        std::fill(card.gifCanvas.begin(), card.gifCanvas.end(), 0u);
        card.gifPrevDisposal = 0;
    } else if (card.gifPrevDisposal == 2) {
        UINT x0 = std::min(card.gifPrevLeft, card.imgW);
        UINT y0 = std::min(card.gifPrevTop, card.imgH);
        UINT x1 = std::min(card.gifPrevLeft + card.gifPrevW, card.imgW);
        UINT y1 = std::min(card.gifPrevTop + card.gifPrevH, card.imgH);
        for (UINT row = y0; row < y1; ++row) {
            std::fill_n(card.gifCanvas.data() + static_cast<size_t>(row) * card.imgW + x0,
                        x1 - x0, 0u);
        }
    } else if (card.gifPrevDisposal == 3 && card.gifPrevCanvas.size() == card.gifCanvas.size()) {
        card.gifCanvas = card.gifPrevCanvas;
    }

    auto const& f = card.gifFrames[idx];
    if (f.disposal == 3) {
        card.gifPrevCanvas = card.gifCanvas;
    }

    UINT maxX = std::min(f.w, (f.left < card.imgW) ? (card.imgW - f.left) : 0u);
    UINT maxY = std::min(f.h, (f.top < card.imgH) ? (card.imgH - f.top) : 0u);
    for (UINT row = 0; row < maxY; ++row) {
        uint32_t const* srcRow = f.pixels.data() + static_cast<size_t>(row) * f.w;
        uint32_t* dstRow = card.gifCanvas.data() + static_cast<size_t>(f.top + row) * card.imgW + f.left;
        for (UINT col = 0; col < maxX; ++col) {
            uint32_t sp = srcRow[col];
            uint32_t sa = sp >> 24;
            if (sa == 255) {
                dstRow[col] = sp;
            } else if (sa > 0) {
                uint32_t dp = dstRow[col];
                uint32_t inv = 255 - sa;
                uint32_t b = (sp & 0xFF) + (((dp & 0xFF) * inv + 127) / 255);
                uint32_t g = ((sp >> 8) & 0xFF) + ((((dp >> 8) & 0xFF) * inv + 127) / 255);
                uint32_t r = ((sp >> 16) & 0xFF) + ((((dp >> 16) & 0xFF) * inv + 127) / 255);
                uint32_t a = sa + (((dp >> 24) * inv + 127) / 255);
                dstRow[col] = (std::min<uint32_t>(a, 255) << 24) |
                              (std::min<uint32_t>(r, 255) << 16) |
                              (std::min<uint32_t>(g, 255) << 8) |
                              std::min<uint32_t>(b, 255);
            }
        }
    }

    card.gifIndex = idx;
    card.gifPrevLeft = f.left;
    card.gifPrevTop = f.top;
    card.gifPrevW = f.w;
    card.gifPrevH = f.h;
    card.gifPrevDisposal = f.disposal;
}

void TrimProcessMemory() {
    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
}

void EnsureCardPhotoLoaded(WidgetCard& card) {
    if (card.loadedPath == card.photoPath && (card.cachedBitmap || !card.gifFrames.empty())) return;
    bool hadGif = !card.gifFrames.empty();
    ComPtrHelper::SafeRelease(card.cachedBitmap);
    card.gifFrames.clear();
    card.gifFrames.shrink_to_fit();
    card.gifCanvas.clear();
    card.gifCanvas.shrink_to_fit();
    card.gifPrevCanvas.clear();
    card.gifPrevCanvas.shrink_to_fit();
    card.gifIndex = 0;
    card.gifPrevDisposal = 0;
    card.loadedPath = card.photoPath;
    card.imgW = 0;
    card.imgH = 0;
    if (card.hwnd) KillTimer(card.hwnd, kTimerGif);
    if (hadGif) TrimProcessMemory();

    if (card.photoPath.empty() || !g_wicFactory) return;

    IWICBitmapDecoder* decoder = nullptr;
    HRESULT hr = g_wicFactory->CreateDecoderFromFilename(
        card.photoPath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr) || !decoder) return;

    UINT frameCount = 0;
    decoder->GetFrameCount(&frameCount);

    if (frameCount > 1) {
        IWICMetadataQueryReader* decMeta = nullptr;
        if (SUCCEEDED(decoder->GetMetadataQueryReader(&decMeta)) && decMeta) {
            card.imgW = ReadMetaUInt(decMeta, L"/logscrdesc/Width", 0);
            card.imgH = ReadMetaUInt(decMeta, L"/logscrdesc/Height", 0);
            decMeta->Release();
        }

        UINT limit = std::min<UINT>(frameCount, 300);  // ponytail: cap extreme GIFs at 300 frames for RAM safety
        for (UINT i = 0; i < limit; ++i) {
            IWICBitmapFrameDecode* frame = nullptr;
            IWICFormatConverter* conv = nullptr;
            if (SUCCEEDED(decoder->GetFrame(i, &frame)) && frame) {
                GifFrame gf{};
                frame->GetSize(&gf.w, &gf.h);

                IWICMetadataQueryReader* fMeta = nullptr;
                if (SUCCEEDED(frame->GetMetadataQueryReader(&fMeta)) && fMeta) {
                    gf.left = ReadMetaUInt(fMeta, L"/imgdesc/Left", 0);
                    gf.top = ReadMetaUInt(fMeta, L"/imgdesc/Top", 0);
                    UINT rawDelay = ReadMetaUInt(fMeta, L"/grctlext/Delay", 10);
                    gf.delayMs = (rawDelay <= 1) ? 100 : rawDelay * 10;
                    gf.disposal = static_cast<uint8_t>(ReadMetaUInt(fMeta, L"/grctlext/Disposal", 0));
                    fMeta->Release();
                }

                card.imgW = std::max(card.imgW, gf.left + gf.w);
                card.imgH = std::max(card.imgH, gf.top + gf.h);

                if (gf.w > 0 && gf.h > 0 &&
                    SUCCEEDED(g_wicFactory->CreateFormatConverter(&conv)) &&
                    SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA,
                                               WICBitmapDitherTypeNone, nullptr, 0.0,
                                               WICBitmapPaletteTypeCustom))) {
                    gf.pixels.resize(static_cast<size_t>(gf.w) * gf.h);
                    if (SUCCEEDED(conv->CopyPixels(
                            nullptr, gf.w * 4, static_cast<UINT>(gf.pixels.size() * 4),
                            reinterpret_cast<BYTE*>(gf.pixels.data())))) {
                        card.gifFrames.push_back(std::move(gf));
                    }
                }
            }
            ComPtrHelper::SafeRelease(conv);
            ComPtrHelper::SafeRelease(frame);
        }

        if (!card.gifFrames.empty() && card.imgW > 0 && card.imgH > 0) {
            card.gifCanvas.assign(static_cast<size_t>(card.imgW) * card.imgH, 0u);
            ComposeGifFrame(card, 0);
            if (card.hwnd) SetTimer(card.hwnd, kTimerGif, card.gifFrames[0].delayMs, nullptr);
        }
        decoder->Release();
        return;
    }

    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    IWICBitmapScaler* scaler = nullptr;

    hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr) && frame) {
        UINT ow = 0, oh = 0;
        frame->GetSize(&ow, &oh);
        IWICBitmapSource* src = frame;

        // Cap decoded bitmap at 1400px long side so huge 4K+ photos use minimal RAM and resize smoothly
        constexpr UINT kMaxDim = 1400;
        if (ow > kMaxDim || oh > kMaxDim) {
            UINT tw = ow, th = oh;
            if (ow >= oh) {
                tw = kMaxDim;
                th = std::max<UINT>(1, static_cast<UINT>(static_cast<double>(oh) * kMaxDim / ow));
            } else {
                th = kMaxDim;
                tw = std::max<UINT>(1, static_cast<UINT>(static_cast<double>(ow) * kMaxDim / oh));
            }
            if (SUCCEEDED(g_wicFactory->CreateBitmapScaler(&scaler)) &&
                SUCCEEDED(scaler->Initialize(frame, tw, th, WICBitmapInterpolationModeFant))) {
                src = scaler;
            }
        }

        if (SUCCEEDED(g_wicFactory->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA,
                                       WICBitmapDitherTypeNone, nullptr, 0.0,
                                       WICBitmapPaletteTypeCustom))) {
            if (SUCCEEDED(g_wicFactory->CreateBitmapFromSource(
                    conv, WICBitmapCacheOnLoad, &card.cachedBitmap)) &&
                card.cachedBitmap) {
                card.cachedBitmap->GetSize(&card.imgW, &card.imgH);
            }
        }
    }

    ComPtrHelper::SafeRelease(conv);
    ComPtrHelper::SafeRelease(scaler);
    ComPtrHelper::SafeRelease(frame);
    ComPtrHelper::SafeRelease(decoder);
}

void RenderCard(WidgetCard& card) {
    if (!card.hwnd || !g_d2dFactory || card.w < 1 || card.h < 1) return;
    EnsureCardPhotoLoaded(card);

    HDC screenDc = GetDC(nullptr);
    HDC memDc = CreateCompatibleDC(screenDc);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = card.w;
    bmi.bmiHeader.biHeight = -card.h;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib) {
        HGDIOBJ oldBmp = SelectObject(memDc, dib);

        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        ID2D1DCRenderTarget* rt = nullptr;
        if (SUCCEEDED(g_d2dFactory->CreateDCRenderTarget(&props, &rt)) && rt) {
            RECT rc{0, 0, card.w, card.h};
            if (SUCCEEDED(rt->BindDC(memDc, &rc))) {
                rt->BeginDraw();
                rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                rt->Clear(D2D1::ColorF(0, 0.0f));

                float fw = static_cast<float>(card.w);
                float fh = static_cast<float>(card.h);
                float radius = std::clamp(std::min(fw, fh) * 0.14f, 12.0f, 30.0f);
                D2D1_ROUNDED_RECT rrect = D2D1::RoundedRect(D2D1::RectF(0.0f, 0.0f, fw, fh), radius, radius);

                ID2D1Bitmap* d2dBmp = nullptr;
                if (card.imgW > 0 && card.imgH > 0) {
                    if (!card.gifCanvas.empty()) {
                        D2D1_BITMAP_PROPERTIES bmpProps = D2D1::BitmapProperties(
                            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                        rt->CreateBitmap(D2D1::SizeU(card.imgW, card.imgH), card.gifCanvas.data(),
                                         card.imgW * 4, bmpProps, &d2dBmp);
                    } else if (card.cachedBitmap) {
                        rt->CreateBitmapFromWicBitmap(card.cachedBitmap, nullptr, &d2dBmp);
                    }
                }
                if (d2dBmp) {
                    float scaleX = fw / static_cast<float>(card.imgW);
                    float scaleY = fh / static_cast<float>(card.imgH);
                    float scale = std::max(scaleX, scaleY);  // aspect-fill cover
                    float drawW = static_cast<float>(card.imgW) * scale;
                    float drawH = static_cast<float>(card.imgH) * scale;
                    float offX = (fw - drawW) * 0.5f;
                    float offY = (fh - drawH) * 0.5f;

                    ID2D1BitmapBrush* brush = nullptr;
                    D2D1_BITMAP_BRUSH_PROPERTIES bbp = D2D1::BitmapBrushProperties(
                        D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
                        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    D2D1_BRUSH_PROPERTIES bp = D2D1::BrushProperties(
                        1.0f,
                        D2D1::Matrix3x2F::Scale(scale, scale) *
                            D2D1::Matrix3x2F::Translation(offX, offY));
                    if (SUCCEEDED(rt->CreateBitmapBrush(d2dBmp, bbp, bp, &brush)) && brush) {
                        rt->FillRoundedRectangle(rrect, brush);
                        brush->Release();
                    }
                    d2dBmp->Release();
                } else {
                    // Minimal borderless translucent iOS placeholder with centered '+' glyph
                    ID2D1SolidColorBrush* bgBrush = nullptr;
                    ID2D1SolidColorBrush* fgBrush = nullptr;
                    if (SUCCEEDED(rt->CreateSolidColorBrush(D2D1::ColorF(0.14f, 0.14f, 0.16f, 0.78f), &bgBrush))) {
                        rt->FillRoundedRectangle(rrect, bgBrush);
                        bgBrush->Release();
                    }
                    if (SUCCEEDED(rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.55f), &fgBrush))) {
                        float cx = fw * 0.5f;
                        float cy = fh * 0.5f;
                        float arm = std::clamp(std::min(fw, fh) * 0.10f, 10.0f, 22.0f);
                        rt->DrawLine(D2D1::Point2F(cx - arm, cy), D2D1::Point2F(cx + arm, cy), fgBrush, 2.5f);
                        rt->DrawLine(D2D1::Point2F(cx, cy - arm), D2D1::Point2F(cx, cy + arm), fgBrush, 2.5f);
                        fgBrush->Release();
                    }
                }
                rt->EndDraw();
            }
            rt->Release();
        }

        POINT ptDst{card.x, card.y};
        SIZE sz{card.w, card.h};
        POINT ptSrc{0, 0};
        BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(card.hwnd, screenDc, &ptDst, &sz, memDc, &ptSrc, 0, &bf, ULW_ALPHA);

        SelectObject(memDc, oldBmp);
        DeleteObject(dib);
    }

    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);
}

bool PickImageFile(HWND owner, std::wstring& outPath) {
    WCHAR fileBuf[MAX_PATH]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Imágenes (*.png;*.jpg;*.jpeg;*.webp;*.gif;*.bmp)\0*.png;*.jpg;*.jpeg;*.webp;*.gif;*.bmp\0Todos (*.*)\0*.*\0";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    bool picked = GetOpenFileNameW(&ofn) != FALSE;
    if (picked) outPath = fileBuf;
    TrimProcessMemory();
    return picked;
}

WidgetCard* FindCardByHwnd(HWND hwnd) {
    for (auto const& c : g_cards) {
        if (c->hwnd == hwnd) return c.get();
    }
    return nullptr;
}

WidgetCard* SpawnCard(int x, int y, int w, int h, bool locked, std::wstring const& photoPath);

void ResizeCard(WidgetCard& card, int newW, int newH) {
    card.w = std::max(kMinSize, newW);
    card.h = std::max(kMinSize, newH);
    SetWindowPos(card.hwnd, HWND_BOTTOM, card.x, card.y, card.w, card.h,
                 SWP_NOACTIVATE);
    RenderCard(card);
    SaveConfig();
}

void ShowCardMenu(WidgetCard& card, int screenX, int screenY) {
    MENUINFO mi{};
    mi.cbSize = sizeof(mi);
    mi.fMask = MIM_STYLE | MIM_APPLYTOSUBMENUS;
    mi.dwStyle = MNS_NOCHECK;  // remove wide unused left check/bitmap column

    HMENU sizeMenu = CreatePopupMenu();
    SetMenuInfo(sizeMenu, &mi);
    AppendMenuW(sizeMenu, MF_STRING, kCmdSizeSmallSq, L"Cuadrado chico (150 x 150)");
    AppendMenuW(sizeMenu, MF_STRING, kCmdSizeMedSq, L"Cuadrado mediano (210 x 210)");
    AppendMenuW(sizeMenu, MF_STRING, kCmdSizeLargeSq, L"Cuadrado grande (300 x 300)");
    AppendMenuW(sizeMenu, MF_STRING, kCmdSizeWide, L"Horizontal (320 x 160)");
    AppendMenuW(sizeMenu, MF_STRING, kCmdSizeTall, L"Vertical (180 x 260)");

    HMENU menu = CreatePopupMenu();
    SetMenuInfo(menu, &mi);

    AppendMenuW(menu, MF_STRING, kCmdChangePhoto, L"Cambiar foto...");
    AppendMenuW(menu, MF_STRING, kCmdDuplicate, L"Duplicar widget");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sizeMenu), L"Tamaño");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdLock,
                card.locked ? L"\u2713 Bloquear posición y tamaño" : L"Bloquear posición y tamaño");
    AppendMenuW(menu, MF_STRING, kCmdAutoStart,
                IsAutoStartEnabled() ? L"\u2713 Iniciar con Windows" : L"Iniciar con Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdRemove, L"Quitar este widget");
    AppendMenuW(menu, MF_STRING, kCmdExitAll, L"Cerrar todos");

    SetForegroundWindow(card.hwnd);
    UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenX, screenY, card.hwnd, nullptr);
    DestroyMenu(menu);  // also destroys submenu

    SetWindowPos(card.hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    switch (cmd) {
        case kCmdChangePhoto: {
            std::wstring path;
            if (PickImageFile(card.hwnd, path)) {
                card.photoPath = std::move(path);
                RenderCard(card);
                SaveConfig();
            }
            break;
        }
        case kCmdDuplicate: {
            int offset = 28;
            SpawnCard(card.x + offset, card.y + offset, card.w, card.h, false, card.photoPath);
            SaveConfig();
            break;
        }
        case kCmdSizeSmallSq: ResizeCard(card, 150, 150); break;
        case kCmdSizeMedSq: ResizeCard(card, 210, 210); break;
        case kCmdSizeLargeSq: ResizeCard(card, 300, 300); break;
        case kCmdSizeWide: ResizeCard(card, 320, 160); break;
        case kCmdSizeTall: ResizeCard(card, 180, 260); break;
        case kCmdLock:
            card.locked = !card.locked;
            SaveConfig();
            break;
        case kCmdAutoStart:
            SetAutoStartEnabled(!IsAutoStartEnabled());
            break;
        case kCmdRemove: {
            HWND target = card.hwnd;
            DestroyWindow(target);
            g_cards.erase(std::remove_if(g_cards.begin(), g_cards.end(),
                                         [target](auto const& p) { return p->hwnd == target; }),
                          g_cards.end());
            SaveConfig();
            if (g_cards.empty()) PostQuitMessage(0);
            else TrimProcessMemory();
            break;
        }
        case kCmdExitAll:
            PostQuitMessage(0);
            break;
        default:
            break;
    }
}

LRESULT CALLBACK CardWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    WidgetCard* card = FindCardByHwnd(hwnd);

    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_WINDOWPOSCHANGING: {
            auto* wp = reinterpret_cast<WINDOWPOS*>(lParam);
            if (!(wp->flags & SWP_NOZORDER)) {
                wp->hwndInsertAfter = HWND_BOTTOM;
            }
            return 0;
        }

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize = {kMinSize, kMinSize};
            return 0;
        }

        case WM_NCHITTEST: {
            if (!card || card->locked) return HTCLIENT;
            RECT rc{};
            GetWindowRect(hwnd, &rc);
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            bool left = (x - rc.left) < kResizeBorder;
            bool right = (rc.right - x) < kResizeBorder;
            bool top = (y - rc.top) < kResizeBorder;
            bool bottom = (rc.bottom - y) < kResizeBorder;

            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
            return HTCLIENT;
        }

        case WM_WINDOWPOSCHANGED: {
            if (card) {
                auto const* wp = reinterpret_cast<WINDOWPOS const*>(lParam);
                bool moved = !(wp->flags & SWP_NOMOVE);
                bool sized = !(wp->flags & SWP_NOSIZE);
                if (moved) {
                    card->x = wp->x;
                    card->y = wp->y;
                }
                if (sized && (wp->cx != card->w || wp->cy != card->h)) {
                    card->w = std::max(kMinSize, wp->cx);
                    card->h = std::max(kMinSize, wp->cy);
                    RenderCard(*card);
                }
            }
            return 0;
        }

        case WM_EXITSIZEMOVE:
            if (card) {
                RECT rc{};
                GetWindowRect(hwnd, &rc);
                card->x = rc.left;
                card->y = rc.top;
                card->w = rc.right - rc.left;
                card->h = rc.bottom - rc.top;
                RenderCard(*card);
                SaveConfig();
            }
            return 0;

        case WM_LBUTTONDOWN:
            if (card && !card->locked) {
                card->dragging = true;
                GetCursorPos(&card->dragStartScreen);
                card->dragStartWnd = {card->x, card->y};
                SetCapture(hwnd);
            }
            return 0;

        case WM_MOUSEMOVE:
            if (card && card->dragging && (wParam & MK_LBUTTON)) {
                POINT cur{};
                GetCursorPos(&cur);
                int nx = card->dragStartWnd.x + (cur.x - card->dragStartScreen.x);
                int ny = card->dragStartWnd.y + (cur.y - card->dragStartScreen.y);
                card->x = nx;
                card->y = ny;
                SetWindowPos(hwnd, HWND_BOTTOM, nx, ny, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
            }
            return 0;

        case WM_LBUTTONUP:
            if (card && card->dragging) {
                card->dragging = false;
                ReleaseCapture();
                SaveConfig();
            }
            return 0;

        case WM_LBUTTONDBLCLK:
            if (card) {
                std::wstring path;
                if (PickImageFile(hwnd, path)) {
                    card->photoPath = std::move(path);
                    RenderCard(*card);
                    SaveConfig();
                }
            }
            return 0;

        case WM_TIMER:
            if (wParam == kTimerGif && card && !card->gifFrames.empty()) {
                KillTimer(hwnd, kTimerGif);
                size_t next = (card->gifIndex + 1) % card->gifFrames.size();
                ComposeGifFrame(*card, next);
                RenderCard(*card);
                SetTimer(hwnd, kTimerGif, card->gifFrames[card->gifIndex].delayMs, nullptr);
            }
            return 0;

        case WM_RBUTTONUP:
            if (card) {
                POINT pt{};
                GetCursorPos(&pt);
                ShowCardMenu(*card, pt.x, pt.y);
            }
            return 0;

        case WM_DROPFILES:
            if (card) {
                auto hDrop = reinterpret_cast<HDROP>(wParam);
                WCHAR dropped[MAX_PATH]{};
                if (DragQueryFileW(hDrop, 0, dropped, MAX_PATH)) {
                    card->photoPath = dropped;
                    RenderCard(*card);
                    SaveConfig();
                }
                DragFinish(hDrop);
            }
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

WidgetCard* SpawnCard(int x, int y, int w, int h, bool locked, std::wstring const& photoPath) {
    auto card = std::make_unique<WidgetCard>();
    card->x = x;
    card->y = y;
    card->w = std::max(kMinSize, w);
    card->h = std::max(kMinSize, h);
    card->locked = locked;
    card->photoPath = photoPath;

    if (!g_desktopOwner) g_desktopOwner = FindDesktopOwnerWindow();

    // Owned by Progman keeps layered cards visible on Win+D while staying behind normal app windows
    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kWndClass, L"", WS_POPUP | WS_VISIBLE,
        card->x, card->y, card->w, card->h,
        g_desktopOwner, nullptr, g_hInst, nullptr);
    if (!hwnd) return nullptr;

    if (g_allowDarkWnd) g_allowDarkWnd(hwnd, TRUE);
    if (g_setWindowTheme) g_setWindowTheme(hwnd, L"DarkMode_Explorer", nullptr);
    DragAcceptFiles(hwnd, TRUE);
    card->hwnd = hwnd;
    WidgetCard* raw = card.get();
    g_cards.push_back(std::move(card));
    RenderCard(*raw);
    SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    return raw;
}

void LoadConfigOrDefault() {
    int count = GetPrivateProfileIntW(L"App", L"Count", 0, g_iniPath.c_str());
    if (count <= 0) {
        int sw = GetSystemMetrics(SM_CXSCREEN);
        SpawnCard(std::max(40, sw - 260), 60, 210, 210, false, L"");
        SaveConfig();
        return;
    }
    count = std::min(count, 24);
    for (int i = 0; i < count; ++i) {
        std::wstring sec = L"Widget" + std::to_wstring(i);
        int x = GetPrivateProfileIntW(sec.c_str(), L"X", 120 + i * 28, g_iniPath.c_str());
        int y = GetPrivateProfileIntW(sec.c_str(), L"Y", 120 + i * 28, g_iniPath.c_str());
        int w = GetPrivateProfileIntW(sec.c_str(), L"W", 210, g_iniPath.c_str());
        int h = GetPrivateProfileIntW(sec.c_str(), L"H", 210, g_iniPath.c_str());
        bool locked = GetPrivateProfileIntW(sec.c_str(), L"Locked", 0, g_iniPath.c_str()) != 0;
        WCHAR photo[MAX_PATH]{};
        GetPrivateProfileStringW(sec.c_str(), L"Photo", L"", photo, MAX_PATH, g_iniPath.c_str());
        SpawnCard(x, y, w, h, locked, photo);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    HANDLE singleInst = CreateMutexW(nullptr, FALSE, L"Local\\PhotoWidgetSingleInstance");
    if (singleInst && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(singleInst);
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_hInst = hInst;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    InitDarkMenus();

    g_iniPath = GetExeDirPath() + L"\\PhotoWidget.ini";
    if (IsAutoStartEnabled()) SetAutoStartEnabled(true);

    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&g_wicFactory));
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2dFactory);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = CardWndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWndClass;
    RegisterClassExW(&wc);

    LoadConfigOrDefault();

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    SaveConfig();
    for (auto const& c : g_cards) {
        if (c->hwnd) DestroyWindow(c->hwnd);
    }
    g_cards.clear();

    ComPtrHelper::SafeRelease(g_d2dFactory);
    ComPtrHelper::SafeRelease(g_wicFactory);
    CoUninitialize();
    if (singleInst) CloseHandle(singleInst);
    return 0;
}
