// =============================================================================
//  Toolbar.h
//  Owner-drawn command bar at the top of the window.
//
//  The toolbar does not store application state: the frame pushes everything
//  it needs (document loaded, page position, zoom text, bookmark state) and
//  receives ordinary WM_COMMAND notifications back.  Hovering a part posts
//  WM_APP_HINT so the frame can show the localised description in the status
//  bar; the toolbar itself has no text labels and no status bar.
// =============================================================================
#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "App/Settings.h"
#include "App/Localization.h"
#include "Ui/UiMessages.h"

namespace pengpdf {

class Toolbar {
public:
    // Registers the window class once per process; must succeed before Create.
    static bool RegisterWindowClass(HINSTANCE instance, const wchar_t* className);

    // Creates the child window.  The Toolbar object must outlive the window;
    // WM_CREATE attaches it through CREATESTRUCT::lpCreateParams.
    bool Create(HWND parent, HINSTANCE instance, const wchar_t* className, UINT dpi);
    HWND Handle() const { return m_hwnd; }
    // Fixed bar height in physical pixels (kToolbarHeightDip scaled by DPI).
    int Height() const;

    void SetDpi(UINT dpi);
    void ApplyLanguage();

    void SetDocumentState(bool hasDocument);
    void SetPageState(int currentPage, int pageCount);
    void SetZoomState(ZoomMode mode, const std::wstring& zoomText);
    void SetBookmarked(bool bookmarked);
    void SetSidebarVisible(bool visible);

private:
    // Icons are painted with GDI primitives in DrawIcon(); adding an enumerator
    // here requires a matching switch case there.
    enum class Icon {
        Sidebar,
        Open,
        Previous,
        Next,
        ZoomOut,
        ZoomIn,
        FitWidth,
        FitPage,
        Star,
        Sliders,
    };

    // Visual/interaction class of a part.  Badges are drawn by their own
    // helpers; PageBadge is clickable (go to page), ZoomBadge only displays.
    enum class PartKind {
        Icon,
        PageBadge,
        ZoomBadge,
    };

    // One hit-testable element.  `rect` and the enabled/active flags are cached
    // by Layout(), so paint and hit testing never recompute state.
    struct Part {
        int command = 0;  // command id reported through WM_COMMAND, 0 for none
        Icon icon = Icon::Open;
        PartKind kind = PartKind::Icon;
        RECT rect{};
        bool enabled = true;
        bool active = false;
    };

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam,
                                       LPARAM lParam);

    void Layout();
    void Paint(HDC dc, const RECT& client);
    // Draws `icon` centred in `box`; `active` fills the star and tints it with
    // the accent colour, other icons ignore it.
    void DrawIcon(HDC dc, const RECT& box, Icon icon, COLORREF color,
                  bool active) const;
    void DrawPageBadge(HDC dc, const RECT& box) const;
    void DrawZoomBadge(HDC dc, const RECT& box) const;

    // Returns the part under `point`, including disabled parts (callers decide
    // whether they are interactive).  The zoom badge is display only and is
    // never returned, so clicks on it fall through.
    const Part* HitTest(POINT point) const;
    void SetHovered(int command);
    void NotifyHint(int command);

    HWND m_hwnd = nullptr;
    UINT m_dpi = 96;

    // Rebuilt left to right by Layout(); m_separators holds the x positions of
    // the vertical group dividers.
    std::vector<Part> m_parts;
    std::vector<int> m_separators;  // x positions of the vertical dividers

    bool m_hasDocument = false;
    int m_currentPage = -1;
    int m_pageCount = 0;
    bool m_bookmarked = false;
    bool m_sidebarVisible = true;
    ZoomMode m_zoomMode = ZoomMode::FitWidth;
    std::wstring m_zoomText;

    int m_hovered = 0;   // command id, 0 when the pointer is over no part
    int m_pressed = 0;   // command id captured on left button down
    bool m_trackingMouse = false;

    // Fonts are rebuilt by SetDpi() and released in WM_DESTROY.
    HFONT m_font = nullptr;
    HFONT m_badgeFont = nullptr;
};

}  // namespace pengpdf
