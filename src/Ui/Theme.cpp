// =============================================================================
//  Theme.cpp
//  Palette, DPI helpers and the small GDI primitives used by every owner-drawn
//  view.  Each helper is deliberately self contained: it creates the GDI
//  objects it needs, restores the DC, and returns, so no state leaks into the
//  caller's painting code.
// =============================================================================
#include "Ui/Theme.h"

#include <algorithm>
#include <cstdint>

namespace pengpdf {
namespace theme {
namespace {

// Zero initialised once; the default member initialisers of Palette supply the
// actual colours.  It is read-only after static initialisation, so no locking
// is needed on the UI thread.
Palette g_palette;

// Restricts a rectangle to a sane minimum so GDI never sees inverted boxes.
RECT Normalize(const RECT& rect) {
    RECT result = rect;
    if (result.right < result.left) {
        std::swap(result.left, result.right);
    }
    if (result.bottom < result.top) {
        std::swap(result.top, result.bottom);
    }
    return result;
}

}  // namespace

const Palette& Colors() {
    return g_palette;
}

UINT DpiOf(HWND window) {
    // GetDpiForWindow exists since Windows 10 1607; the manifest declares
    // per-monitor awareness so the value can change while the app runs.
    if (window != nullptr) {
        const UINT dpi = ::GetDpiForWindow(window);
        if (dpi != 0) {
            return dpi;
        }
    }
    const HDC screen = ::GetDC(nullptr);
    if (screen != nullptr) {
        const int dpi = ::GetDeviceCaps(screen, LOGPIXELSX);
        ::ReleaseDC(nullptr, screen);
        if (dpi > 0) {
            return static_cast<UINT>(dpi);
        }
    }
    return 96;
}

int Scale(int dip, UINT dpi) {
    // MulDiv rounds to the nearest integer, so a 1.5 px result becomes 2 px
    // instead of being truncated to 1 px (which would shrink 1 px hairlines at
    // fractional scaling factors).
    return ::MulDiv(dip, static_cast<int>(dpi), 96);
}

HFONT CreateUiFont(UINT dpi, double points, int weight, bool useChineseFamily) {
    // GDI expects a negative lfHeight to mean "character height"; the positive
    // form would be the cell height and include internal leading.  Points are
    // converted with 72 typographic points per inch, matching the LOGFONT unit.
    const int pixelHeight = -static_cast<int>(points * static_cast<double>(dpi) / 72.0);
    const wchar_t* family = useChineseFamily ? L"Microsoft YaHei UI" : L"Segoe UI";

    LOGFONTW description = {};
    description.lfHeight = pixelHeight;
    description.lfWeight = weight;
    description.lfCharSet = DEFAULT_CHARSET;
    description.lfOutPrecision = OUT_TT_PRECIS;
    description.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    // ClearType keeps small UI labels crisp on the light backgrounds; the font
    // family is chosen by the caller because Chinese and Latin labels have
    // different preferred faces.
    description.lfQuality = CLEARTYPE_QUALITY;
    description.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    wcscpy_s(description.lfFaceName, family);

    // Returns null if the font cannot be created; callers treat a null font as
    // "use the DC's current font", so painting still succeeds.
    HFONT font = ::CreateFontIndirectW(&description);
    return font;
}

void ReleaseFont(HFONT& font) {
    if (font != nullptr) {
        ::DeleteObject(font);
        font = nullptr;
    }
}

void FillSolid(HDC dc, const RECT& rect, COLORREF color) {
    const RECT target = Normalize(rect);
    // The brush is created per call; it is deleted immediately after FillRect,
    // which copies the pixels and does not retain the handle.
    HBRUSH brush = ::CreateSolidBrush(color);
    if (brush == nullptr) {
        return;
    }
    ::FillRect(dc, &target, brush);
    ::DeleteObject(brush);
}

void FillRound(HDC dc, const RECT& rect, int radius, COLORREF color) {
    const RECT target = Normalize(rect);
    // RoundRect silently draws nothing for an empty rectangle, but skipping it
    // also avoids creating a brush that would be thrown away.
    if (target.right <= target.left || target.bottom <= target.top) {
        return;
    }
    HBRUSH brush = ::CreateSolidBrush(color);
    HGDIOBJ oldBrush = ::SelectObject(dc, brush);
    // RoundRect would also stroke the outline with the current pen; selecting
    // the stock NULL_PEN makes this a pure fill.
    HGDIOBJ oldPen = ::SelectObject(dc, ::GetStockObject(NULL_PEN));
    // RoundRect expects the width/height of the corner ellipse, hence radius*2.
    ::RoundRect(dc, target.left, target.top, target.right, target.bottom, radius * 2,
                radius * 2);
    ::SelectObject(dc, oldPen);
    ::SelectObject(dc, oldBrush);
    ::DeleteObject(brush);
}

void StrokeRound(HDC dc, const RECT& rect, int radius, COLORREF color, int thickness) {
    const RECT target = Normalize(rect);
    if (target.right <= target.left || target.bottom <= target.top) {
        return;
    }
    // NULL_BRUSH keeps the interior transparent; the pen straddles the outline,
    // so half of a thick stroke extends outside `target`.
    HPEN pen = ::CreatePen(PS_SOLID, thickness, color);
    HGDIOBJ oldPen = ::SelectObject(dc, pen);
    HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
    ::RoundRect(dc, target.left, target.top, target.right, target.bottom, radius * 2,
                radius * 2);
    ::SelectObject(dc, oldBrush);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);
}

void FillCircleShape(HDC dc, int centerX, int centerY, int radius, COLORREF color) {
    // `radius` is in physical pixels and is not DPI scaled here: callers pass
    // an already scaled value.  A non-positive radius produces an empty shape.
    HBRUSH brush = ::CreateSolidBrush(color);
    HGDIOBJ oldBrush = ::SelectObject(dc, brush);
    HGDIOBJ oldPen = ::SelectObject(dc, ::GetStockObject(NULL_PEN));
    ::Ellipse(dc, centerX - radius, centerY - radius, centerX + radius, centerY + radius);
    ::SelectObject(dc, oldPen);
    ::SelectObject(dc, oldBrush);
    ::DeleteObject(brush);
}

namespace {

// A one pixel 32bpp DIB is reused for every alpha fill.  Stretching it with
// AlphaBlend is much cheaper than creating a bitmap per rectangle, which
// matters while dragging a selection across a page of text.
//
// The context lives for the whole process and is only touched from the UI
// thread; the bitmap stays selected into `dc` for its entire lifetime so the
// `pixels` pointer remains writable.
struct BlendContext {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previousBitmap = nullptr;
    uint32_t* pixels = nullptr;
};

BlendContext g_blend;

bool EnsureBlendContext(HDC target) {
    // Lazy one-time initialisation: subsequent alpha fills reuse the context.
    if (g_blend.dc != nullptr) {
        return true;
    }
    g_blend.dc = ::CreateCompatibleDC(target);
    if (g_blend.dc == nullptr) {
        return false;
    }

    // A DIB section (rather than a compatible bitmap) is required because the
    // pixel value has to be written directly before each blend.  The negative
    // height requests top-down scan lines; for 1x1 it only standardises the
    // memory layout.
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = 1;
    info.bmiHeader.biHeight = -1;  // top down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    g_blend.bitmap =
        ::CreateDIBSection(g_blend.dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (g_blend.bitmap == nullptr || pixels == nullptr) {
        // A half-initialised context would make the next call return true and
        // dereference a null pixel pointer, so tear everything down here.
        if (g_blend.bitmap != nullptr) {
            ::DeleteObject(g_blend.bitmap);
            g_blend.bitmap = nullptr;
        }
        ::DeleteDC(g_blend.dc);
        g_blend.dc = nullptr;
        return false;
    }
    g_blend.pixels = static_cast<uint32_t*>(pixels);
    g_blend.previousBitmap = ::SelectObject(g_blend.dc, g_blend.bitmap);
    return true;
}

}  // namespace

void FillTranslucent(HDC dc, const RECT& rect, COLORREF color, int alpha) {
    const RECT target = Normalize(rect);
    if (target.right <= target.left || target.bottom <= target.top) {
        return;
    }
    const int clampedAlpha = std::clamp(alpha, 0, 255);
    if (clampedAlpha == 0) {
        return;
    }
    if (!EnsureBlendContext(dc)) {
        return;
    }

    // AlphaBlend expects premultiplied colour components.
    const uint32_t a = static_cast<uint32_t>(clampedAlpha);
    const uint32_t r = (static_cast<uint32_t>(GetRValue(color)) * a) / 255u;
    const uint32_t g = (static_cast<uint32_t>(GetGValue(color)) * a) / 255u;
    const uint32_t b = (static_cast<uint32_t>(GetBValue(color)) * a) / 255u;
    *g_blend.pixels = (a << 24) | (r << 16) | (g << 8) | b;
    // GDI batches drawing operations; flush before AlphaBlend reads the DIB so
    // the CPU written pixel is guaranteed to be visible to the blitter.
    ::GdiFlush();

    // SourceConstantAlpha is 255 because the per-pixel alpha already encodes
    // the translucency; the 1x1 bitmap is stretched over the whole rectangle.
    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    ::AlphaBlend(dc, target.left, target.top, target.right - target.left,
                 target.bottom - target.top, g_blend.dc, 0, 0, 1, 1, blend);
}

void DrawLine(HDC dc, int x1, int y1, int x2, int y2, COLORREF color, int thickness) {
    // A cosmetic pen (thickness 1) is used for hairlines; wider values produce
    // a geometric pen.  The previous pen is restored before the new one is
    // deleted, because deleting an object that is still selected is invalid.
    HPEN pen = ::CreatePen(PS_SOLID, thickness, color);
    HGDIOBJ oldPen = ::SelectObject(dc, pen);
    ::MoveToEx(dc, x1, y1, nullptr);
    ::LineTo(dc, x2, y2);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);
}

namespace {

// Shared implementation of the DrawString* helpers.  It saves and restores the
// three pieces of DC state it changes (background mode, text colour and font)
// so nested painting cannot leak styling into the caller.
int DrawFormatted(HDC dc, std::wstring_view text, RECT rect, COLORREF color,
                  HFONT font, UINT format) {
    if (text.empty()) {
        return 0;
    }
    // TRANSPARENT stops DrawText from painting the opaque background box; the
    // background was already painted by the view.
    const int previousMode = ::SetBkMode(dc, TRANSPARENT);
    const COLORREF previousColor = ::SetTextColor(dc, color);
    HGDIOBJ previousFont = nullptr;
    if (font != nullptr) {
        previousFont = ::SelectObject(dc, font);
    }
    // DT_NOPREFIX is always added: label text such as "&File" must be rendered
    // literally instead of treating '&' as a mnemonic marker.
    const int height = ::DrawTextW(dc, text.data(), static_cast<int>(text.size()), &rect,
                                   format | DT_NOPREFIX);
    if (font != nullptr) {
        ::SelectObject(dc, previousFont);
    }
    ::SetTextColor(dc, previousColor);
    ::SetBkMode(dc, previousMode);
    return height;
}

}  // namespace

void DrawString(HDC dc, std::wstring_view text, RECT rect, COLORREF color, HFONT font,
                UINT format) {
    // Single line vertical centring is the common case for buttons and badges,
    // so it is baked in; callers only choose the horizontal alignment.
    DrawFormatted(dc, text, rect, color, font, format | DT_SINGLELINE | DT_VCENTER);
}

void DrawStringEllipsis(HDC dc, std::wstring_view text, RECT rect, COLORREF color,
                        HFONT font, UINT format) {
    // DT_END_ELLIPSIS replaces the clipped tail with "..." and keeps the text
    // inside a fixed-width column such as an outline node.
    DrawFormatted(dc, text, rect, color, font,
                  format | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

void DrawStringBlock(HDC dc, std::wstring_view text, RECT rect, COLORREF color,
                     HFONT font, UINT format) {
    // No flags are added: callers need control over wrapping and vertical
    // alignment for empty-state text blocks.
    DrawFormatted(dc, text, rect, color, font, format);
}

int TextWidth(HDC dc, std::wstring_view text, HFONT font) {
    if (text.empty()) {
        return 0;
    }
    HGDIOBJ previousFont = nullptr;
    if (font != nullptr) {
        previousFont = ::SelectObject(dc, font);
    }
    SIZE size = {};
    // GetTextExtentPoint32W applies the currently selected font, hence the
    // temporary SelectObject above.
    ::GetTextExtentPoint32W(dc, text.data(), static_cast<int>(text.size()), &size);
    if (font != nullptr) {
        ::SelectObject(dc, previousFont);
    }
    return size.cx;
}

}  // namespace theme
}  // namespace pengpdf


