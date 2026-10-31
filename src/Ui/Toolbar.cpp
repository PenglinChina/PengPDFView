// =============================================================================
//  Toolbar.cpp
//  Owner-drawn command bar.
//
//  All icons are drawn with GDI primitives (lines, polygons, ellipses) so the
//  toolbar does not depend on an icon font being installed and remains crisp at
//  every DPI.
// =============================================================================
#include "Ui/Toolbar.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include "App/Commands.h"
#include "Core/Config.h"
#include "Core/Utf.h"
#include "Ui/Theme.h"

namespace pengpdf {
namespace {

// Returns the pen width for hairline icons, scaled for the monitor DPI.
// GDI treats a zero width as a one device pixel cosmetic pen, but scaling the
// width explicitly keeps the strokes visually equal at every DPI.
int StrokeWidth(UINT dpi) {
    return std::max(1, theme::Scale(1, dpi));
}

POINT MakePoint(int x, int y) {
    POINT point;
    point.x = x;
    point.y = y;
    return point;
}

// Open polyline with round GDI joins; used for glyph outlines (document, fold,
// chevrons, star outline, arrow heads).
void StrokePolyline(HDC dc, const POINT* points, int count, COLORREF color, int width) {
    // Create/select/restore/delete: the pen must be deselected before it can be
    // deleted, otherwise the delete silently fails and leaks the GDI object.
    HPEN pen = ::CreatePen(PS_SOLID, width, color);
    HGDIOBJ oldPen = ::SelectObject(dc, pen);
    ::Polyline(dc, points, count);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);
}

// Filled polygon with a transparent outline; GDI closes the polygon implicitly,
// so callers do not repeat the first vertex.
void FillPolygon(HDC dc, const POINT* points, int count, COLORREF color) {
    HBRUSH brush = ::CreateSolidBrush(color);
    HGDIOBJ oldBrush = ::SelectObject(dc, brush);
    HGDIOBJ oldPen = ::SelectObject(dc, ::GetStockObject(NULL_PEN));
    ::Polygon(dc, points, count);
    ::SelectObject(dc, oldPen);
    ::SelectObject(dc, oldBrush);
    ::DeleteObject(brush);
}

// Circle outline used by the magnifier; the settings faders' knobs are filled
// separately with theme::FillCircleShape.
void StrokeCircle(HDC dc, int centerX, int centerY, int radius, COLORREF color,
                  int width) {
    HPEN pen = ::CreatePen(PS_SOLID, width, color);
    HGDIOBJ oldPen = ::SelectObject(dc, pen);
    HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
    ::Ellipse(dc, centerX - radius, centerY - radius, centerX + radius,
              centerY + radius);
    ::SelectObject(dc, oldBrush);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);
}

// Maps a command id to the localised status bar hint.  Returns false for parts
// that have no hint (badges) or for unknown ids, letting the caller fall back
// to the "ready" text.
bool HintForCommand(int command, Str& hint) {
    switch (command) {
        case command::FileOpen: hint = Str::HintOpen; return true;
        case command::ViewToggleSidebar: hint = Str::HintSidebar; return true;
        case command::GoPreviousPage: hint = Str::HintPreviousPage; return true;
        case command::GoNextPage: hint = Str::HintNextPage; return true;
        case command::ViewZoomOut: hint = Str::HintZoomOut; return true;
        case command::ViewZoomIn: hint = Str::HintZoomIn; return true;
        case command::ViewFitWidth: hint = Str::HintFitWidth; return true;
        case command::ViewFitPage: hint = Str::HintFitPage; return true;
        case command::BookmarksAdd: hint = Str::HintBookmark; return true;
        case command::SettingsPreferences: hint = Str::HintSettings; return true;
        case command::GoToPage: hint = Str::HintGoToPage; return true;
        default: return false;
    }
}

}  // namespace

bool Toolbar::RegisterWindowClass(HINSTANCE instance, const wchar_t* className) {
    WNDCLASSEXW description = {};
    description.cbSize = sizeof(description);
    // Full-window repaints are cheap here (everything is redrawn into an
    // off-screen bitmap), so no region-based invalidation is needed.
    description.style = CS_HREDRAW | CS_VREDRAW;
    description.lpfnWndProc = &Toolbar::WindowProc;
    description.hInstance = instance;
    description.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    description.lpszClassName = className;
    return ::RegisterClassExW(&description) != 0;
}

bool Toolbar::Create(HWND parent, HINSTANCE instance, const wchar_t* className,
                     UINT dpi) {
    // Set before CreateWindowEx so Layout(), which can be reached from WM_SIZE
    // during creation, already uses the target DPI.  `this` travels through
    // lpCreateParams and is stored in GWLP_USERDATA by WM_CREATE.
    m_dpi = dpi;
    m_hwnd = ::CreateWindowExW(
        0, className, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent,
        nullptr, instance, this);
    if (m_hwnd == nullptr) {
        return false;
    }
    SetDpi(dpi);
    return true;
}

int Toolbar::Height() const {
    // The frame sizes the toolbar with this value and positions the content
    // below it; keep it in sync with kToolbarHeightDip.
    return theme::Scale(kToolbarHeightDip, m_dpi);
}

void Toolbar::SetDpi(UINT dpi) {
    m_dpi = dpi;
    theme::ReleaseFont(m_font);
    theme::ReleaseFont(m_badgeFont);
    m_font = theme::CreateUiFont(dpi, 9.0, FW_NORMAL, IsChineseInterface());
    m_badgeFont = theme::CreateUiFont(dpi, 9.0, FW_SEMIBOLD, IsChineseInterface());
    // All part rectangles are DPI derived, so a DPI change is also a relayout.
    Layout();
    if (m_hwnd != nullptr) {
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void Toolbar::ApplyLanguage() {
    // Only the font family changes with the language; all labels come from the
    // frame through WM_APP_HINT, so there is nothing else to update here.
    SetDpi(m_dpi);
}

void Toolbar::SetDocumentState(bool hasDocument) {
    if (m_hasDocument == hasDocument) {
        return;
    }
    m_hasDocument = hasDocument;
    if (!hasDocument) {
        // Page/zoom/bookmark state is meaningless once the document is closed;
        // reset it so the badges fall back to their placeholder text.
        m_currentPage = -1;
        m_pageCount = 0;
        m_bookmarked = false;
    }
    Layout();
    if (m_hwnd != nullptr) {
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void Toolbar::SetPageState(int currentPage, int pageCount) {
    // Always relayout: besides the badge text this recomputes the enabled state
    // of the previous/next buttons (and refreshes the cached flags of every
    // other part).
    m_currentPage = currentPage;
    m_pageCount = pageCount;
    Layout();
    if (m_hwnd != nullptr) {
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void Toolbar::SetZoomState(ZoomMode mode, const std::wstring& zoomText) {
    // The fit-width/fit-page icons become accent coloured when their mode is
    // active, and that flag is cached inside Part by Layout().
    m_zoomMode = mode;
    m_zoomText = zoomText;
    Layout();
    if (m_hwnd != nullptr) {
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void Toolbar::SetBookmarked(bool bookmarked) {
    if (m_bookmarked == bookmarked) {
        return;
    }
    m_bookmarked = bookmarked;
    // The star's filled/outline state is cached in the Part built by Layout(),
    // so a relayout is required, not just a repaint.
    Layout();
    if (m_hwnd != nullptr) {
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void Toolbar::SetSidebarVisible(bool visible) {
    if (m_sidebarVisible == visible) {
        return;
    }
    m_sidebarVisible = visible;
    // The sidebar icon uses Part::active, hence the relayout, not just a paint.
    Layout();
    if (m_hwnd != nullptr) {
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void Toolbar::Layout() {
    // Parts can be touched from WM_SIZE/WM_CREATE before creation finishes.
    if (m_hwnd == nullptr) {
        return;
    }
    RECT client = {};
    ::GetClientRect(m_hwnd, &client);

    // Rebuilding invalidates every Part* previously returned by HitTest; that
    // is safe because pointers are only held for the duration of one message.
    m_parts.clear();
    m_separators.clear();

    // Subtract the one pixel bottom border so the icons are optically centred
    // in the remaining content area.
    const int centerY = (client.bottom - theme::Scale(1, m_dpi)) / 2;
    const int buttonWidth = theme::Scale(34, m_dpi);
    const int buttonHeight = theme::Scale(30, m_dpi);
    const int badgeHeight = theme::Scale(26, m_dpi);
    int x = theme::Scale(10, m_dpi);

    // Appends a square icon button and advances the left-to-right cursor.  The
    // enabled/active flags are captured here, so state changes must relayout.
    auto addIcon = [&](int command, Icon icon, bool enabled, bool active) {
        Part part;
        part.command = command;
        part.icon = icon;
        part.kind = PartKind::Icon;
        part.enabled = enabled;
        part.active = active;
        part.rect.left = x;
        part.rect.top = centerY - buttonHeight / 2;
        part.rect.right = x + buttonWidth;
        part.rect.bottom = centerY + buttonHeight / 2;
        m_parts.push_back(part);
        x += buttonWidth;
    };

    // Five DIPs before the divider line, eight DIPs after it, so the line sits
    // in the middle of the gap between two icon groups.
    auto addSeparator = [&]() {
        x += theme::Scale(5, m_dpi);
        m_separators.push_back(x);
        x += theme::Scale(8, m_dpi);
    };

    const bool hasDocument = m_hasDocument;
    // Page stepping is disabled at the first/last page rather than wrapping.
    const bool hasPrevious = hasDocument && m_currentPage > 0;
    const bool hasNext = hasDocument && m_currentPage + 1 < m_pageCount;

    addIcon(command::ViewToggleSidebar, Icon::Sidebar, true, m_sidebarVisible);
    addIcon(command::FileOpen, Icon::Open, true, false);
    addSeparator();

    addIcon(command::GoPreviousPage, Icon::Previous, hasPrevious, false);
    addIcon(command::GoNextPage, Icon::Next, hasNext, false);

    // The page badge is a single clickable part (opens the Go To Page dialog);
    // its width is fixed so the neighbouring icons do not shift with the page
    // number.
    const int pageBadgeWidth = theme::Scale(82, m_dpi);
    Part pageBadge;
    pageBadge.command = command::GoToPage;
    pageBadge.kind = PartKind::PageBadge;
    pageBadge.enabled = hasDocument;
    pageBadge.rect.left = x;
    pageBadge.rect.top = centerY - badgeHeight / 2;
    pageBadge.rect.right = x + pageBadgeWidth;
    pageBadge.rect.bottom = centerY + badgeHeight / 2;
    m_parts.push_back(pageBadge);
    x += pageBadgeWidth;
    addSeparator();

    addIcon(command::ViewZoomOut, Icon::ZoomOut, hasDocument, false);

    // Zoom percentage badge: display only, hence HitTest() skips it and the
    // command stays 0.
    const int zoomBadgeWidth = theme::Scale(56, m_dpi);
    Part zoomBadge;
    zoomBadge.kind = PartKind::ZoomBadge;
    zoomBadge.enabled = hasDocument;
    zoomBadge.rect.left = x;
    zoomBadge.rect.top = centerY - badgeHeight / 2;
    zoomBadge.rect.right = x + zoomBadgeWidth;
    zoomBadge.rect.bottom = centerY + badgeHeight / 2;
    m_parts.push_back(zoomBadge);
    x += zoomBadgeWidth;

    addIcon(command::ViewZoomIn, Icon::ZoomIn, hasDocument, false);
    addIcon(command::ViewFitWidth, Icon::FitWidth, hasDocument,
            m_zoomMode == ZoomMode::FitWidth);
    addIcon(command::ViewFitPage, Icon::FitPage, hasDocument,
            m_zoomMode == ZoomMode::FitPage);
    addSeparator();
    addIcon(command::BookmarksAdd, Icon::Star, hasDocument, m_bookmarked);

    // Settings stays pinned to the right edge, independent of the group layout
    // above.  On windows narrower than the fixed part sequence requires, this
    // can overlap the left-hand groups; the frame enforces a minimum width.
    const int rightEdge = client.right - theme::Scale(10, m_dpi);
    Part settings;
    settings.command = command::SettingsPreferences;
    settings.icon = Icon::Sliders;
    settings.kind = PartKind::Icon;
    settings.enabled = true;
    settings.rect.left = rightEdge - buttonWidth;
    settings.rect.top = centerY - buttonHeight / 2;
    settings.rect.right = rightEdge;
    settings.rect.bottom = centerY + buttonHeight / 2;
    m_parts.push_back(settings);
}

void Toolbar::Paint(HDC dc, const RECT& client) {
    const theme::Palette& colors = theme::Colors();
    // Painting is always done into a fresh back buffer, but filling first also
    // makes the function safe if it is ever called directly on the window DC.
    theme::FillSolid(dc, client, colors.toolbarBackground);

    // Vertical dividers between logical groups.  They are inset from the top and
    // bottom edges so they read as group separators rather than a full border.
    const int dividerTop = client.top + theme::Scale(13, m_dpi);
    const int dividerBottom = client.bottom - theme::Scale(13, m_dpi);
    for (int x : m_separators) {
        // One physical pixel wide regardless of DPI: a scaled hairline would be
        // blurry or too heavy on high density screens.
        theme::FillSolid(dc, RECT{x, dividerTop, x + 1, dividerBottom}, colors.separator);
    }

    for (const Part& part : m_parts) {
        // Badges own their full appearance (background, text, hover), so they
        // bypass the icon-button decoration below.
        if (part.kind == PartKind::PageBadge) {
            DrawPageBadge(dc, part.rect);
            continue;
        }
        if (part.kind == PartKind::ZoomBadge) {
            DrawZoomBadge(dc, part.rect);
            continue;
        }

        // Hover / pressed background for icon buttons.  Disabled buttons never
        // show feedback, since they cannot be activated.
        const bool hovered = part.enabled && m_hovered == part.command;
        const bool pressed = part.enabled && m_pressed == part.command;
        if (hovered || pressed) {
            // Inset the rounded highlight inside the hit rectangle so adjacent
            // buttons keep a visible gap.
            RECT background = part.rect;
            ::InflateRect(&background, -theme::Scale(2, m_dpi), -theme::Scale(2, m_dpi));
            theme::FillRound(dc, background, theme::Scale(6, m_dpi),
                             pressed ? colors.pressed : colors.hover);
        }

        // Colour precedence: disabled (grey) beats active (accent), which beats
        // the normal text colour.
        COLORREF color = colors.text;
        if (!part.enabled) {
            color = colors.textDisabled;
        } else if (part.active) {
            color = colors.accent;
        }
        DrawIcon(dc, part.rect, part.icon, color, part.active);
    }

    // Bottom border of the toolbar.  It is painted last so neither the icons nor
    // the group separators can overwrite it.
    const int borderY = client.bottom - theme::Scale(1, m_dpi);
    theme::FillSolid(dc, RECT{client.left, borderY, client.right, client.bottom},
                     colors.toolbarBorder);
}

void Toolbar::DrawPageBadge(HDC dc, const RECT& box) const {
    const theme::Palette& colors = theme::Colors();
    RECT background = box;
    // A one DIP vertical inset keeps the pill visually lighter than its hit
    // rectangle without changing the clickable area.
    ::InflateRect(&background, 0, -theme::Scale(1, m_dpi));

    // Hover is only possible with a document: without one the badge is disabled
    // and the pointer hit is ignored by WM_MOUSEMOVE.
    const bool hovered = m_hovered == command::GoToPage && m_hasDocument;
    theme::FillRound(dc, background, theme::Scale(6, m_dpi),
                     hovered ? colors.hover : colors.badge);

    // Em dashes communicate "no document" without needing a localised label.
    std::wstring text = L"\u2014 / \u2014";
    if (m_pageCount > 0 && m_currentPage >= 0) {
        // The model stores a zero based page index; users see one based numbers.
        text = Format(L"%d / %d", m_currentPage + 1, m_pageCount);
    }
    theme::DrawString(dc, text, background,
                      m_hasDocument ? colors.text : colors.textDisabled, m_badgeFont,
                      DT_CENTER);
}

void Toolbar::DrawZoomBadge(HDC dc, const RECT& box) const {
    const theme::Palette& colors = theme::Colors();
    // The percentage text is supplied by the frame (it includes a localised
    // format), so the toolbar only needs a fallback for the closed-document case.
    const std::wstring text = m_hasDocument && !m_zoomText.empty() ? m_zoomText : L"\u2014";
    theme::DrawString(dc, text, box, m_hasDocument ? colors.textMuted : colors.textDisabled,
                      m_badgeFont, DT_CENTER);
}

void Toolbar::DrawIcon(HDC dc, const RECT& box, Icon icon, COLORREF color,
                       bool active) const {
    const int centerX = (box.left + box.right) / 2;
    const int centerY = (box.top + box.bottom) / 2;
    // `unit` is one DIP in physical pixels; all icon coordinates below are
    // expressed in these units relative to the box centre, so the shapes stay
    // crisp and correctly sized at any DPI and inside any button size.
    const int unit = theme::Scale(1, m_dpi);
    const int stroke = StrokeWidth(m_dpi);

    switch (icon) {
        case Icon::Sidebar: {
            // Rounded panel outline with a vertical divider (the sidebar).
            RECT panel{centerX - 7 * unit, centerY - 6 * unit, centerX + 7 * unit,
                       centerY + 6 * unit};
            theme::StrokeRound(dc, panel, 2 * unit, color, stroke);
            theme::DrawLine(dc, centerX - 2 * unit, centerY - 6 * unit,
                            centerX - 2 * unit, centerY + 6 * unit, color, stroke);
            break;
        }
        case Icon::Open: {
            // A page with a folded corner.
            const POINT page[] = {
                MakePoint(centerX - 6 * unit, centerY - 7 * unit),
                MakePoint(centerX + 2 * unit, centerY - 7 * unit),
                MakePoint(centerX + 6 * unit, centerY - 3 * unit),
                MakePoint(centerX + 6 * unit, centerY + 7 * unit),
                MakePoint(centerX - 6 * unit, centerY + 7 * unit),
                MakePoint(centerX - 6 * unit, centerY - 7 * unit),
            };
            StrokePolyline(dc, page, 6, color, stroke);
            const POINT fold[] = {
                MakePoint(centerX + 2 * unit, centerY - 7 * unit),
                MakePoint(centerX + 2 * unit, centerY - 3 * unit),
                MakePoint(centerX + 6 * unit, centerY - 3 * unit),
            };
            StrokePolyline(dc, fold, 3, color, stroke);
            break;
        }
        case Icon::Previous:
        case Icon::Next: {
            // One chevron definition serves both directions: multiplying the x
            // offsets by direction mirrors it around the centre.
            const bool left = icon == Icon::Previous;
            const int direction = left ? -1 : 1;
            const POINT chevron[] = {
                MakePoint(centerX - 2 * unit * direction, centerY - 5 * unit),
                MakePoint(centerX + 3 * unit * direction, centerY),
                MakePoint(centerX - 2 * unit * direction, centerY + 5 * unit),
            };
            StrokePolyline(dc, chevron, 3, color, stroke);
            break;
        }
        case Icon::ZoomIn:
        case Icon::ZoomOut: {
            // Magnifier with a plus or minus inside.
            const int radius = 5 * unit;
            StrokeCircle(dc, centerX - 1 * unit, centerY - 1 * unit, radius, color,
                         stroke);
            theme::DrawLine(dc, centerX + 3 * unit, centerY + 3 * unit,
                            centerX + 7 * unit, centerY + 7 * unit, color, stroke);
            theme::DrawLine(dc, centerX - 1 * unit - 2 * unit, centerY - 1 * unit,
                            centerX - 1 * unit + 2 * unit, centerY - 1 * unit, color,
                            stroke);
            if (icon == Icon::ZoomIn) {
                theme::DrawLine(dc, centerX - 1 * unit, centerY - 1 * unit - 2 * unit,
                                centerX - 1 * unit, centerY - 1 * unit + 2 * unit,
                                color, stroke);
            }
            break;
        }
        case Icon::FitWidth: {
            // A page above a horizontal double arrow.
            RECT page{centerX - 5 * unit, centerY - 7 * unit, centerX + 5 * unit,
                      centerY + 1 * unit};
            theme::StrokeRound(dc, page, unit, color, stroke);
            theme::DrawLine(dc, centerX - 8 * unit, centerY + 5 * unit,
                            centerX + 8 * unit, centerY + 5 * unit, color, stroke);
            const POINT leftHead[] = {
                MakePoint(centerX - 6 * unit, centerY + 3 * unit),
                MakePoint(centerX - 8 * unit, centerY + 5 * unit),
                MakePoint(centerX - 6 * unit, centerY + 7 * unit),
            };
            const POINT rightHead[] = {
                MakePoint(centerX + 6 * unit, centerY + 3 * unit),
                MakePoint(centerX + 8 * unit, centerY + 5 * unit),
                MakePoint(centerX + 6 * unit, centerY + 7 * unit),
            };
            StrokePolyline(dc, leftHead, 3, color, stroke);
            StrokePolyline(dc, rightHead, 3, color, stroke);
            break;
        }
        case Icon::FitPage: {
            // Four corner brackets: the usual "fit to page" symbol.
            const int left = centerX - 7 * unit;
            const int right = centerX + 7 * unit;
            const int top = centerY - 7 * unit;
            const int bottom = centerY + 7 * unit;
            const int arm = theme::Scale(4, m_dpi);
            theme::DrawLine(dc, left, top + arm, left, top, color, stroke);
            theme::DrawLine(dc, left, top, left + arm, top, color, stroke);
            theme::DrawLine(dc, right - arm, top, right, top, color, stroke);
            theme::DrawLine(dc, right, top, right, top + arm, color, stroke);
            theme::DrawLine(dc, left, bottom - arm, left, bottom, color, stroke);
            theme::DrawLine(dc, left, bottom, left + arm, bottom, color, stroke);
            theme::DrawLine(dc, right - arm, bottom, right, bottom, color, stroke);
            theme::DrawLine(dc, right, bottom - arm, right, bottom, color, stroke);
            break;
        }
        case Icon::Star: {
            // Five pointed star; filled when the page is bookmarked.
            // Ten vertices alternate between the outer and inner radius;
            // starting at -pi/2 puts one point straight up.
            POINT points[10] = {};
            const double outerRadius = 7.0 * unit;
            const double innerRadius = 3.0 * unit;
            for (int index = 0; index < 10; ++index) {
                const double angle =
                    -3.14159265358979323846 / 2.0 + index * 3.14159265358979323846 / 5.0;
                const double radius = (index % 2 == 0) ? outerRadius : innerRadius;
                points[index].x = centerX + static_cast<int>(std::lround(std::cos(angle) * radius));
                points[index].y = centerY + static_cast<int>(std::lround(std::sin(angle) * radius));
            }
            if (active) {
                FillPolygon(dc, points, 10, color);
            } else {
                POINT closed[11] = {};
                for (int index = 0; index < 10; ++index) {
                    closed[index] = points[index];
                }
                closed[10] = points[0];
                StrokePolyline(dc, closed, 11, color, stroke);
            }
            break;
        }
        case Icon::Sliders: {
            // Three faders, the usual "settings" metaphor.  The knob offsets are
            // deliberately different so the rows do not look mechanically
            // aligned, and the rows are evenly spaced around centerY.
            const int left = centerX - 7 * unit;
            const int right = centerX + 7 * unit;
            const int rows[3] = {centerY - 4 * unit, centerY, centerY + 4 * unit};
            const int knobs[3] = {centerX + 2 * unit, centerX - 3 * unit, centerX + 5 * unit};
            for (int row = 0; row < 3; ++row) {
                theme::DrawLine(dc, left, rows[row], right, rows[row], color, stroke);
                theme::FillCircleShape(dc, knobs[row], rows[row], 2 * unit, color);
            }
            break;
        }
    }
}

const Toolbar::Part* Toolbar::HitTest(POINT point) const {
    // Parts are laid out left to right and never overlap (except at very small
    // window widths), so the first hit is returned.  Disabled parts are still
    // returned; the caller decides whether they react.  The zoom badge is
    // skipped because it is display only.
    for (const Part& part : m_parts) {
        if (part.kind == PartKind::ZoomBadge) {
            continue;
        }
        if (point.x >= part.rect.left && point.x < part.rect.right &&
            point.y >= part.rect.top && point.y < part.rect.bottom) {
            return &part;
        }
    }
    return nullptr;
}

void Toolbar::SetHovered(int command) {
    // Repaint only when the highlighted part actually changes; WM_MOUSEMOVE
    // arrives at a much higher rate than the visual state changes.
    if (m_hovered == command) {
        return;
    }
    m_hovered = command;
    if (m_hwnd != nullptr) {
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void Toolbar::NotifyHint(int command) {
    if (m_hwnd == nullptr) {
        return;
    }
    // Posting (rather than sending) keeps the status bar update out of the
    // toolbar's mouse message; wParam tells the frame whether to show a hint or
    // restore the default text, lParam carries the Str identifier.
    Str hint = Str::StatusReady;
    const bool haveHint = command != 0 && HintForCommand(command, hint);
    ::PostMessageW(::GetParent(m_hwnd), WM_APP_HINT, haveHint ? 1u : 0u,
                   static_cast<LPARAM>(hint));
}

LRESULT CALLBACK Toolbar::WindowProc(HWND window, UINT message, WPARAM wParam,
                                     LPARAM lParam) {
    Toolbar* self =
        reinterpret_cast<Toolbar*>(::GetWindowLongPtrW(window, GWLP_USERDATA));

    if (message == WM_CREATE) {
        // WM_CREATE is the only message that carries lpCreateParams, so this is
        // where the C++ object is bound to its HWND.  Every later message finds
        // it through GWLP_USERDATA.
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<Toolbar*>(create->lpCreateParams);
        ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self != nullptr) {
            self->m_hwnd = window;
        }
        return 0;
    }

    if (self == nullptr) {
        // The object was already detached (WM_DESTROY) or creation failed.
        return ::DefWindowProcW(window, message, wParam, lParam);
    }

    switch (message) {
        case WM_ERASEBKGND:
            // The double-buffered WM_PAINT below paints every pixel; letting
            // Windows erase first would only cause flicker.
            return 1;

        case WM_SIZE:
            // Part rectangles are absolute and derived from the client width,
            // so a resize always requires a relayout.
            self->Layout();
            ::InvalidateRect(window, nullptr, FALSE);
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT paint = {};
            HDC dc = ::BeginPaint(window, &paint);
            RECT client = {};
            ::GetClientRect(window, &client);
            if (client.right > 0 && client.bottom > 0) {
                // Paint the whole bar off screen and blit once: the many small
                // GDI primitives of the icons would otherwise flicker.
                HDC memoryDc = ::CreateCompatibleDC(dc);
                HBITMAP bitmap = ::CreateCompatibleBitmap(
                    dc, client.right - client.left, client.bottom - client.top);
                HGDIOBJ oldBitmap = ::SelectObject(memoryDc, bitmap);
                self->Paint(memoryDc, client);
                ::BitBlt(dc, 0, 0, client.right, client.bottom, memoryDc, 0, 0, SRCCOPY);
                ::SelectObject(memoryDc, oldBitmap);
                ::DeleteObject(bitmap);
                ::DeleteDC(memoryDc);
            }
            ::EndPaint(window, &paint);
            return 0;
        }

        case WM_MOUSEMOVE: {
            // TrackMouseEvent is one shot: it must be re-armed on the first
            // move after entering the window, and WM_MOUSELEAVE resets the flag.
            if (!self->m_trackingMouse) {
                TRACKMOUSEEVENT track = {};
                track.cbSize = sizeof(track);
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = window;
                ::TrackMouseEvent(&track);
                self->m_trackingMouse = true;
            }
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const Part* part = self->HitTest(point);
            // Disabled parts map to command 0, i.e. "no hover" and the default
            // status hint.
            const int command = (part != nullptr && part->enabled) ? part->command : 0;
            if (command != self->m_hovered) {
                self->SetHovered(command);
                self->NotifyHint(command);
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            self->m_trackingMouse = false;
            self->SetHovered(0);
            self->NotifyHint(0);
            return 0;

        case WM_LBUTTONDOWN: {
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const Part* part = self->HitTest(point);
            if (part != nullptr && part->enabled) {
                // Capture the mouse so the matching WM_LBUTTONUP arrives even if
                // the pointer leaves the toolbar while the button is down.
                self->m_pressed = part->command;
                ::SetCapture(window);
                ::InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            // Read the pressed command *before* releasing the capture: the
            // ReleaseCapture call delivers WM_CAPTURECHANGED, which clears it.
            const int pressed = self->m_pressed;
            self->m_pressed = 0;
            if (::GetCapture() == window) {
                ::ReleaseCapture();
            }
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const Part* part = self->HitTest(point);
            ::InvalidateRect(window, nullptr, FALSE);
            // A click only counts when press and release happen on the same
            // enabled part; sliding off the button cancels the action.
            if (pressed != 0 && part != nullptr && part->enabled &&
                part->command == pressed) {
                // lParam identifies the sender, matching the WM_COMMAND
                // convention used by the frame's other child controls.
                ::SendMessageW(::GetParent(window), WM_COMMAND,
                               MAKEWPARAM(pressed, 0), reinterpret_cast<LPARAM>(window));
            }
            return 0;
        }

        case WM_CAPTURECHANGED:
            // Capture was stolen (for example by a modal dialog opening); the
            // pending press must not fire later.
            self->m_pressed = 0;
            ::InvalidateRect(window, nullptr, FALSE);
            return 0;

        case WM_SETCURSOR: {
            POINT point = {};
            ::GetCursorPos(&point);
            ::ScreenToClient(window, &point);
            const Part* part = self->HitTest(point);
            if (part != nullptr && part->enabled) {
                ::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        }

        case WM_DESTROY:
            // The HWND is going away but the Toolbar object stays alive; fonts
            // must be released here because no further WM_PAINT can use them.
            theme::ReleaseFont(self->m_font);
            theme::ReleaseFont(self->m_badgeFont);
            self->m_hwnd = nullptr;
            return 0;

        default:
            break;
    }
    return ::DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace pengpdf
