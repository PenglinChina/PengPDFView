// =============================================================================
//  Theme.h
//  Central palette, DPI helpers and small GDI drawing utilities.
//
//  The complete UI is owner-drawn, so every view pulls its colours and fonts
//  from this module.  Keeping the values in one place makes the application
//  look consistent and future theme changes trivial.
//
//  Drawing helpers create and destroy their own pens and brushes, so callers
//  only need a valid HDC.  Fonts are different: they are expensive and remain
//  caller owned, and must be released with ReleaseFont() when the DPI or the
//  interface language changes and on WM_DESTROY.
// =============================================================================
#pragma once

#include <windows.h>

#include <string>
#include <string_view>

namespace pengpdf {
namespace theme {

// The light palette used by every window of the reader.  Hover and pressed
// shades are only a few steps darker than the surfaces, keeping the chrome
// unobtrusive around the document.
struct Palette {
    // Surfaces, from window chrome down to the rendered page.
    COLORREF windowBackground = RGB(252, 252, 253);
    COLORREF toolbarBackground = RGB(252, 252, 253);
    COLORREF toolbarBorder = RGB(228, 228, 233);
    COLORREF sidebarBackground = RGB(250, 250, 251);
    COLORREF canvasBackground = RGB(240, 240, 243);
    COLORREF pageBackground = RGB(255, 255, 255);
    COLORREF pageBorder = RGB(214, 214, 221);
    COLORREF pageShadow = RGB(205, 205, 213);
    // Primary text, secondary labels and controls that cannot be invoked.
    COLORREF text = RGB(32, 33, 36);
    COLORREF textMuted = RGB(96, 100, 105);
    COLORREF textDisabled = RGB(160, 163, 168);
    // Accent for interactive highlights; accentSoft is the pale wash behind
    // highlighted text blocks, selection is the outline/row highlight.
    COLORREF accent = RGB(26, 115, 232);
    COLORREF accentSoft = RGB(232, 241, 254);
    COLORREF selection = RGB(227, 238, 253);
    // Transient pointer feedback shared by toolbar buttons and list rows.
    COLORREF hover = RGB(241, 243, 245);
    COLORREF pressed = RGB(224, 230, 236);
    // Scrollbar thumb, in its idle and hovered/dragged states.
    COLORREF scrollbarThumb = RGB(198, 198, 206);
    COLORREF scrollbarThumbHover = RGB(170, 170, 180);
    // One pixel dividers and the neutral background of the toolbar badges.
    COLORREF separator = RGB(232, 232, 237);
    COLORREF badge = RGB(238, 240, 243);
};

// Returns the single process wide palette; the reference stays valid forever.
const Palette& Colors();

// DPI handling ----------------------------------------------------------------
// Returns the effective DPI of a window (96 when the API is unavailable).
// `window` may be null; the function then falls back to the screen DC, which is
// what controls created before their HWND exists need.
UINT DpiOf(HWND window);
// Converts a device independent pixel value to physical pixels.
// Uses MulDiv, so the result is rounded to the nearest pixel rather than
// truncated; do not replace it with plain integer multiplication.
int Scale(int dip, UINT dpi);

// Fonts -----------------------------------------------------------------------
// `points` is the typographic size; `weight` is a GDI weight such as FW_NORMAL.
// Chinese text uses Microsoft YaHei UI, which ships with every Windows 10
// installation and has noticeably better glyphs than the fallback font.
// The returned HFONT is caller owned and must be deleted with ReleaseFont().
HFONT CreateUiFont(UINT dpi, double points, int weight, bool useChineseFamily);
// Small helper that deletes a font and clears the handle.
void ReleaseFont(HFONT& font);

// Drawing helpers -------------------------------------------------------------
// All coordinates are physical pixels in the target HDC.  Inverted rectangles
// are normalised internally, and every helper restores the DC state before it
// returns, so callers can chain calls without saving/restoring objects.
void FillSolid(HDC dc, const RECT& rect, COLORREF color);
// `radius` is the corner radius (the GDI RoundRect diameters are derived from
// it); one pixel strokes are centred on the rectangle outline.
void FillRound(HDC dc, const RECT& rect, int radius, COLORREF color);
void StrokeRound(HDC dc, const RECT& rect, int radius, COLORREF color, int thickness = 1);
void FillCircleShape(HDC dc, int centerX, int centerY, int radius, COLORREF color);
// Alpha blends a solid colour over the target.  Search highlights and the text
// selection use this so the glyphs below the rectangle stay readable.
// `alpha` is 0 (no-op) to 255 (opaque); callers pass physical pixel rectangles.
void FillTranslucent(HDC dc, const RECT& rect, COLORREF color, int alpha);
// Straight line; `thickness` is in physical pixels.  The pen is created for the
// call and released afterwards, so no DC state is left behind.
void DrawLine(HDC dc, int x1, int y1, int x2, int y2, COLORREF color, int thickness = 1);

// Draws a single line of text.  DT_SINGLELINE|DT_VCENTER is always ORed into
// `format`, so only horizontal alignment (DT_LEFT/DT_CENTER/DT_RIGHT) is
// meaningful at the call site.  The text is clipped to `rect`.
void DrawString(HDC dc, std::wstring_view text, RECT rect, COLORREF color,
                HFONT font, UINT format);
// Same as DrawString but truncates with an ellipsis instead of clipping.
void DrawStringEllipsis(HDC dc, std::wstring_view text, RECT rect, COLORREF color,
                        HFONT font, UINT format);
// Multi-line text block; the caller controls alignment and wrapping flags
// (typically DT_CENTER | DT_WORDBREAK | DT_TOP).  No flags are added here.
void DrawStringBlock(HDC dc, std::wstring_view text, RECT rect, COLORREF color,
                     HFONT font, UINT format);
// Width of a single line in physical pixels with `font` temporarily selected.
int TextWidth(HDC dc, std::wstring_view text, HFONT font);

}  // namespace theme
}  // namespace pengpdf
