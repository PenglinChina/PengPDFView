// =============================================================================
//  Dialogs.cpp
//  Implementation of the custom modal dialog framework and the two dialogs.
//
//  Every dialog is a frameless popup painted into an off-screen bitmap: the
//  framework owns the title bar, border, close button, fonts and modal loop,
//  while the derived dialog paints a single content rectangle with plain GDI
//  helpers from Ui/Theme.h.
// =============================================================================
#include "Ui/Dialogs.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

#include "Core/Config.h"
#include "Core/Utf.h"
#include "Ui/Theme.h"

namespace pengpdf {
namespace {

// Custom title bar height and frame thickness, in DIPs.  The frame is one DIP
// wide and is also excluded from ContentArea(), so content coordinates never
// overlap the border at any DPI.
constexpr int kTitleBarHeightDip = 44;
constexpr int kDialogBorderDip = 1;

// FrameRect takes a brush rather than a colour, so a temporary brush is needed
// for the one pixel frame.  FrameRect draws inside the rectangle bounds.
void FrameRectWithColor(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = ::CreateSolidBrush(color);
    ::FrameRect(dc, &rect, brush);
    ::DeleteObject(brush);
}

// Radio button: an opaque disc (to hide the row hover wash), a ring whose
// colour reflects the selection, and a filled accent dot when selected.
void DrawRadio(HDC dc, int centerX, int centerY, bool selected, UINT dpi,
               const theme::Palette& colors) {
    const int radius = theme::Scale(7, dpi);
    // Fill first: the ring is stroked afterwards and would otherwise mix with
    // whatever background the row painted below it.
    theme::FillCircleShape(dc, centerX, centerY, radius, colors.pageBackground);
    HPEN pen = ::CreatePen(PS_SOLID, std::max(1, theme::Scale(1, dpi)),
                           selected ? colors.accent : colors.pageBorder);
    HGDIOBJ oldPen = ::SelectObject(dc, pen);
    HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
    ::Ellipse(dc, centerX - radius, centerY - radius, centerX + radius, centerY + radius);
    ::SelectObject(dc, oldBrush);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);
    if (selected) {
        // The inner dot is roughly 4/7 of the outer radius, the usual radio
        // proportion.
        theme::FillCircleShape(dc, centerX, centerY, theme::Scale(4, dpi), colors.accent);
    }
}

// Square checkbox with a white check mark on the accent fill when checked, or
// a neutral outline when unchecked.
void DrawCheckbox(HDC dc, const RECT& box, bool checked, UINT dpi,
                  const theme::Palette& colors) {
    if (checked) {
        theme::FillRound(dc, box, theme::Scale(3, dpi), colors.accent);
        // Inset the check so it never touches the rounded border; the middle
        // vertex is slightly left of centre to mimic a hand drawn tick.
        const int left = box.left + theme::Scale(4, dpi);
        const int right = box.right - theme::Scale(4, dpi);
        const int top = box.top + theme::Scale(4, dpi);
        const int bottom = box.bottom - theme::Scale(4, dpi);
        const POINT check[] = {
            {left, (top + bottom) / 2},
            {(left + right) / 2 - theme::Scale(1, dpi), bottom},
            {right, top},
        };
        HPEN pen = ::CreatePen(PS_SOLID, std::max(1, theme::Scale(2, dpi)),
                               colors.pageBackground);
        HGDIOBJ oldPen = ::SelectObject(dc, pen);
        ::Polyline(dc, check, 3);
        ::SelectObject(dc, oldPen);
        ::DeleteObject(pen);
    } else {
        theme::StrokeRound(dc, box, theme::Scale(3, dpi), colors.pageBorder,
                           std::max(1, theme::Scale(1, dpi)));
    }
}

// Standard dialog button.  `primary` is the default/OK action: filled with the
// accent colour and white text.  Secondary buttons get an outline plus a hover
// wash.  Hover feedback is only supplied by dialogs that track row hover.
void DrawButton(HDC dc, const RECT& rect, const wchar_t* label, bool primary,
                bool hovered, HFONT font, UINT dpi, const theme::Palette& colors) {
    if (primary) {
        // Slightly darker accent on hover; kept literal because the palette has
        // no dedicated "accentPressed" role.
        theme::FillRound(dc, rect, theme::Scale(6, dpi),
                         hovered ? RGB(20, 100, 210) : colors.accent);
        theme::DrawString(dc, label, rect, RGB(255, 255, 255), font, DT_CENTER);
    } else {
        if (hovered) {
            theme::FillRound(dc, rect, theme::Scale(6, dpi), colors.hover);
        }
        theme::StrokeRound(dc, rect, theme::Scale(6, dpi), colors.pageBorder,
                           std::max(1, theme::Scale(1, dpi)));
        theme::DrawString(dc, label, rect, colors.text, font, DT_CENTER);
    }
}

// Half-open containment, matching every other hit test in the UI: a point on
// the right/bottom edge belongs to the next element, not this one.
bool Contains(const RECT& rect, POINT point) {
    return point.x >= rect.left && point.x < rect.right && point.y >= rect.top &&
           point.y < rect.bottom;
}

}  // namespace

// =============================================================================
//  ModalDialog
// =============================================================================
ModalDialog::~ModalDialog() {
    // Fonts are released by ShowModal (both success and failure paths); the
    // dialog owns no other GDI or user objects.
}

int ModalDialog::Scale(int dip) const {
    // m_dpi is captured from the owner in ShowModal so every layout helper
    // (including hit testing) uses the same scale as the painted window.
    return theme::Scale(dip, m_dpi);
}

int ModalDialog::TitleBarHeight() const {
    return theme::Scale(kTitleBarHeightDip, m_dpi);
}

RECT ModalDialog::ContentRect(const RECT& client) const {
    RECT content = client;
    // Reserve the title bar at the top and the one DIP frame on the remaining
    // three sides; the resulting rectangle is the coordinate space in which the
    // derived dialog paints and hit-tests.
    content.top += TitleBarHeight();
    content.left += Scale(kDialogBorderDip);
    content.right -= Scale(kDialogBorderDip);
    content.bottom -= Scale(kDialogBorderDip);
    return content;
}

RECT ModalDialog::ContentArea() const {
    RECT client = {};
    if (m_hwnd != nullptr) {
        ::GetClientRect(m_hwnd, &client);
    }
    return ContentRect(client);
}

RECT ModalDialog::CloseButtonRect(const RECT& client) const {
    // A 30 DIP square inset 8 DIPs from the right, vertically centred in the
    // title bar.  It must stay inside the title bar because WM_NCHITTEST maps
    // that whole band to HTCAPTION except for this square.
    const int size = Scale(30);
    const int margin = Scale(8);
    RECT rect{client.right - margin - size, client.top + (TitleBarHeight() - size) / 2,
              client.right - margin,
              client.top + (TitleBarHeight() - size) / 2 + size};
    return rect;
}

void ModalDialog::RefreshFonts() {
    // Fonts are cached on the dialog: release the previous generation first,
    // then recreate all four sizes from the current language.
    theme::ReleaseFont(m_titleFont);
    theme::ReleaseFont(m_bodyFont);
    theme::ReleaseFont(m_boldFont);
    theme::ReleaseFont(m_smallFont);

    // Use the Chinese family only when the interface is Chinese; Microsoft YaHei
    // UI has much better CJK glyphs but slightly different Latin metrics.
    const bool chinese = IsChineseInterface();
    m_titleFont = theme::CreateUiFont(m_dpi, 10.5, FW_SEMIBOLD, chinese);
    m_bodyFont = theme::CreateUiFont(m_dpi, 9.5, FW_NORMAL, chinese);
    m_boldFont = theme::CreateUiFont(m_dpi, 9.5, FW_SEMIBOLD, chinese);
    m_smallFont = theme::CreateUiFont(m_dpi, 8.5, FW_NORMAL, chinese);
}

void ModalDialog::Invalidate() {
    if (m_hwnd != nullptr) {
        // FALSE: the WM_PAINT handler redraws the background itself.
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void ModalDialog::CloseModal(bool accepted) {
    m_result = accepted;
    m_running = false;
    if (m_hwnd != nullptr) {
        // Wake the modal message loop so it can notice m_running == false.
        // WM_NULL has no side effects and is discarded by the dispatcher.
        ::PostMessageW(m_hwnd, WM_NULL, 0, 0);
    }
}

void ModalDialog::RefreshTitleText() {
    // DialogTitle() returns a translated string, so the cached copy becomes
    // stale when the language is switched inside the settings dialog.
    m_title = DialogTitle();
    Invalidate();
}

bool ModalDialog::ShowModal(HWND owner, HINSTANCE instance) {
    // Capture everything the layout depends on before creating the window: the
    // owner is used for centring/DPI, the size is converted to physical pixels
    // once and reused by every helper during the dialog's lifetime.
    m_owner = owner;
    m_dpi = theme::DpiOf(owner);
    m_widthPx = Scale(DialogWidthDip());
    m_heightPx = Scale(DialogHeightDip());
    m_result = false;

    // Register the shared dialog class once.  The function-local static also
    // serialises registration if two dialogs are ever created on different
    // threads.  The window has no standard frame; CS_DROPSHADOW supplies the
    // shadow while WS_EX_DLGMODALFRAME requests the modal frame style.
    static const bool classRegistered = [] {
        WNDCLASSEXW description = {};
        description.cbSize = sizeof(description);
        description.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
        description.lpfnWndProc = &ModalDialog::WindowProc;
        description.hInstance = ::GetModuleHandleW(nullptr);
        description.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        description.lpszClassName = kDialogWindowClass;
        return ::RegisterClassExW(&description) != 0;
    }();
    if (!classRegistered) {
        return false;
    }

    // Create the fonts before the window exists so the first paint and all
    // layout helpers have valid handles; WM_CREATE refreshes them after the
    // HWND has been bound.
    RefreshFonts();

    // Centre the dialog over its owner, clamped to the monitor work area.
    RECT ownerRect = {};
    ::GetWindowRect(owner, &ownerRect);
    int x = ownerRect.left + (ownerRect.right - ownerRect.left - m_widthPx) / 2;
    int y = ownerRect.top + (ownerRect.bottom - ownerRect.top - m_heightPx) / 2;

    MONITORINFO monitor = {};
    monitor.cbSize = sizeof(monitor);
    if (::GetMonitorInfoW(::MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor)) {
        // rcWork excludes the taskbar, so the dialog never opens underneath it.
        // On very small work areas (or very high scaling) the dialog may be
        // larger than the monitor itself; clamp the upper bound to the lower
        // one in that case, since std::clamp requires lo <= hi.
        const int minX = static_cast<int>(monitor.rcWork.left);
        const int minY = static_cast<int>(monitor.rcWork.top);
        const int maxX = std::max(minX, static_cast<int>(monitor.rcWork.right) - m_widthPx);
        const int maxY = std::max(minY, static_cast<int>(monitor.rcWork.bottom) - m_heightPx);
        x = std::clamp(x, minX, maxX);
        y = std::clamp(y, minY, maxY);
    }

    // WS_POPUP + WS_EX_TOOLWINDOW: no standard caption (the title bar is owner
    // drawn) and no taskbar/Alt+Tab entry.  `this` is forwarded to WM_CREATE.
    m_hwnd = ::CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOOLWINDOW, kDialogWindowClass,
                               L"", WS_POPUP | WS_CLIPSIBLINGS, x, y, m_widthPx, m_heightPx,
                               owner, nullptr, instance, this);
    if (m_hwnd == nullptr) {
        // RefreshFonts() ran before CreateWindowEx, so clean up here; no WM_PAINT
        // or WM_DESTROY will run for a window that was never created.
        theme::ReleaseFont(m_titleFont);
        theme::ReleaseFont(m_bodyFont);
        theme::ReleaseFont(m_boldFont);
        theme::ReleaseFont(m_smallFont);
        return false;
    }

    // Disable the owner for the duration of the modal loop, then show and
    // activate the dialog.  SetFocus is required for WM_KEYDOWN/WM_CHAR to
    // arrive even though the dialog is the only enabled top level window.
    ::EnableWindow(owner, FALSE);
    ::ShowWindow(m_hwnd, SW_SHOW);
    ::UpdateWindow(m_hwnd);
    ::SetForegroundWindow(m_hwnd);
    ::SetFocus(m_hwnd);

    // Modal loop.  It deliberately uses GetMessage(nullptr, ...) so messages for
    // the disabled owner and for the dialog are all dispatched by this thread.
    // CloseModal posts WM_NULL to wake GetMessage when m_running becomes false.
    m_running = true;
    MSG message = {};
    while (m_running && ::GetMessageW(&message, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }

    // DestroyWindow sends WM_DESTROY, which clears m_hwnd; guard anyway in case
    // the window disappeared through some other path.
    if (m_hwnd != nullptr) {
        ::DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }

    // Fonts live exactly as long as one ShowModal call.
    theme::ReleaseFont(m_titleFont);
    theme::ReleaseFont(m_bodyFont);
    theme::ReleaseFont(m_boldFont);
    theme::ReleaseFont(m_smallFont);

    // Re-enable and reactivate the owner; it may have been destroyed while the
    // dialog was open (for example by a WM_QUIT path).
    if (::IsWindow(owner)) {
        ::EnableWindow(owner, TRUE);
        ::SetActiveWindow(owner);
        ::SetForegroundWindow(owner);
    }

    // A WM_QUIT while the dialog was open must be passed on to the main loop:
    // GetMessage consumed it here, and returning without reposting would make
    // the application hang instead of exiting.
    if (message.message == WM_QUIT) {
        ::PostQuitMessage(static_cast<int>(message.wParam));
    }
    return m_result;
}

void ModalDialog::PaintFrame(HDC dc, const RECT& client) {
    const theme::Palette& colors = theme::Colors();
    // Background and the one pixel frame first; the title and controls are
    // drawn on top of this base.
    theme::FillSolid(dc, client, colors.windowBackground);
    FrameRectWithColor(dc, client, colors.toolbarBorder);

    // Reserve 18 DIPs on the left and 48 DIPs on the right (for the close
    // button), and the full title bar height for the title line.
    RECT titleRect{client.left + Scale(18), client.top,
                   client.right - Scale(48), client.top + TitleBarHeight()};
    theme::DrawString(dc, m_title.empty() ? DialogTitle() : m_title, titleRect,
                      colors.text, m_titleFont, DT_LEFT);

    const RECT closeRect = CloseButtonRect(client);
    if (m_closeHovered) {
        // Rounded hover wash behind the X only while the pointer is over it.
        theme::FillRound(dc, closeRect, Scale(6), colors.hover);
    }
    // The X is two diagonals through the centre; the arm length scales with DPI
    // and the muted colour keeps it secondary to the title.
    const int centerX = (closeRect.left + closeRect.right) / 2;
    const int centerY = (closeRect.top + closeRect.bottom) / 2;
    const int arm = Scale(5);
    theme::DrawLine(dc, centerX - arm, centerY - arm, centerX + arm, centerY + arm,
                    colors.textMuted, std::max(1, Scale(1)));
    theme::DrawLine(dc, centerX + arm, centerY - arm, centerX - arm, centerY + arm,
                    colors.textMuted, std::max(1, Scale(1)));
}

LRESULT CALLBACK ModalDialog::WindowProc(HWND window, UINT message, WPARAM wParam,
                                         LPARAM lParam) {
    ModalDialog* self =
        reinterpret_cast<ModalDialog*>(::GetWindowLongPtrW(window, GWLP_USERDATA));

    if (message == WM_CREATE) {
        // Bind the object to its HWND and initialise the per-window state that
        // depends on the final DPI: cached translated title and fonts.
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<ModalDialog*>(create->lpCreateParams);
        ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self != nullptr) {
            self->m_hwnd = window;
            self->m_title = self->DialogTitle();
            self->RefreshFonts();
        }
        return 0;
    }
    if (self == nullptr) {
        return ::DefWindowProcW(window, message, wParam, lParam);
    }
    // All other messages go through the non-static handler so derived state can
    // be touched directly.
    return self->HandleMessage(message, wParam, lParam);
}

LRESULT ModalDialog::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    const theme::Palette& colors = theme::Colors();

    switch (message) {
        case WM_ERASEBKGND:
            // WM_PAINT renders the full client area into a back buffer.
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT paint = {};
            HDC dc = ::BeginPaint(m_hwnd, &paint);
            RECT client = {};
            ::GetClientRect(m_hwnd, &client);
            if (client.right > 0 && client.bottom > 0) {
                // Double buffering avoids flicker while radio buttons, hover
                // backgrounds and text are drawn in several passes.
                HDC memoryDc = ::CreateCompatibleDC(dc);
                HBITMAP bitmap = ::CreateCompatibleBitmap(dc, client.right, client.bottom);
                HGDIOBJ oldBitmap = ::SelectObject(memoryDc, bitmap);

                // Painting order: frame/title bar, opaque content background,
                // then the derived dialog's controls.
                PaintFrame(memoryDc, client);
                const RECT content = ContentRect(client);
                theme::FillSolid(memoryDc, content, colors.windowBackground);
                PaintContent(memoryDc, content);

                ::BitBlt(dc, 0, 0, client.right, client.bottom, memoryDc, 0, 0, SRCCOPY);
                ::SelectObject(memoryDc, oldBitmap);
                ::DeleteObject(bitmap);
                ::DeleteDC(memoryDc);
            }
            ::EndPaint(m_hwnd, &paint);
            return 0;
        }

        case WM_NCHITTEST: {
            // The window is frameless, so dragging is implemented by reporting
            // HTCAPTION for the title bar.  The close button must stay HTCLIENT:
            // otherwise the non-client code would handle the click and the
            // dialog would never receive WM_LBUTTONDOWN for it.
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ::ScreenToClient(m_hwnd, &point);
            RECT client = {};
            ::GetClientRect(m_hwnd, &client);
            const RECT closeRect = CloseButtonRect(client);
            if (Contains(closeRect, point)) {
                return HTCLIENT;
            }
            if (point.y < TitleBarHeight()) {
                return HTCAPTION;
            }
            return HTCLIENT;
        }

        case WM_MOUSEMOVE: {
            // Arm leave tracking once per entry so the close-button hover state
            // is cleared when the pointer exits the dialog.
            if (!m_trackingMouse) {
                TRACKMOUSEEVENT track = {};
                track.cbSize = sizeof(track);
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = m_hwnd;
                ::TrackMouseEvent(&track);
                m_trackingMouse = true;
            }
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT client = {};
            ::GetClientRect(m_hwnd, &client);
            const bool closeHovered = Contains(CloseButtonRect(client), point);
            if (closeHovered != m_closeHovered) {
                m_closeHovered = closeHovered;
                Invalidate();
            }
            // Forwarded unconditionally; derived hit tests ignore points in the
            // title bar or outside their rows.
            OnContentMouseMove(point);
            return 0;
        }

        case WM_MOUSELEAVE:
            m_trackingMouse = false;
            // Keep the derived dialog's hand-cursor flag untouched here: it is
            // recomputed on the next WM_MOUSEMOVE.
            m_closeHovered = false;
            Invalidate();
            return 0;

        case WM_LBUTTONDOWN: {
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT client = {};
            ::GetClientRect(m_hwnd, &client);
            // The close button is handled by the framework; everything else is
            // offered to the derived dialog.
            if (Contains(CloseButtonRect(client), point)) {
                CloseModal(false);
                return 0;
            }
            OnContentMouseDown(point);
            return 0;
        }

        case WM_LBUTTONUP: {
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            OnContentMouseUp(point);
            return 0;
        }

        case WM_KEYDOWN:
            // Framework shortcuts are handled before the derived dialog: Escape
            // always cancels and Enter triggers the current accept action.
            if (wParam == VK_ESCAPE) {
                CloseModal(false);
                return 0;
            }
            if (wParam == VK_RETURN) {
                OnAccept();
                return 0;
            }
            OnKeyDown(wParam);
            return 0;

        case WM_CHAR:
            // Only printable characters are forwarded; control characters such
            // as Enter/Backspace are handled through WM_KEYDOWN above.
            if (wParam >= 32 && wParam < 0x10000) {
                OnChar(static_cast<wchar_t>(wParam));
            }
            return 0;

        case WM_SETCURSOR: {
            POINT point = {};
            ::GetCursorPos(&point);
            ::ScreenToClient(m_hwnd, &point);
            RECT client = {};
            ::GetClientRect(m_hwnd, &client);
            if (Contains(CloseButtonRect(client), point) || m_contentHandCursor) {
                ::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        }

        case WM_CLOSE:
            // The system close gesture (Alt+F4) and WM_CLOSE from code reject
            // the dialog unless the derived class has already accepted it.
            CloseModal(false);
            return 0;

        case WM_DESTROY:
            m_hwnd = nullptr;
            return 0;

        default:
            break;
    }
    return ::DefWindowProcW(m_hwnd, message, wParam, lParam);
}

// =============================================================================
//  SettingsDialog
// =============================================================================
SettingsDialog::SettingsDialog(const AppSettings& settings, Lang currentLanguage)
    // Keep a private copy: the dialog edits zoom/sidebar/language locally and
    // only the frame decides whether to commit them.
    : m_settings(settings),
      m_language(currentLanguage),
      m_originalLanguage(currentLanguage) {}

std::wstring SettingsDialog::DialogTitle() const {
    // Called through the base class on creation and after a language switch.
    return Tr(Str::SettingsTitle);
}

int SettingsDialog::LanguageRowTop() const {
    // Fixed rhythm: 44 DIPs leave room for the section label (drawn at +16)
    // before the first radio row starts.
    return ContentArea().top + Scale(44);
}

int SettingsDialog::ZoomRowTop() const {
    // The language group (label, two rows, hint) occupies the space above this.
    return ContentArea().top + Scale(176);
}

RECT SettingsDialog::LanguageRowRect(int index) const {
    // Radio rows are stacked 34 DIPs apart and inset 14 DIPs from the frame;
    // the entire row is clickable, not just the radio circle.
    const RECT content = ContentArea();
    const int top = LanguageRowTop() + index * Scale(34);
    RECT rect{content.left + Scale(14), top, content.right - Scale(14),
              top + Scale(34)};
    return rect;
}

RECT SettingsDialog::ZoomRowRect(int index) const {
    // Same geometry as the language rows, anchored below the second section.
    const RECT content = ContentArea();
    const int top = ZoomRowTop() + index * Scale(34);
    RECT rect{content.left + Scale(14), top, content.right - Scale(14),
              top + Scale(34)};
    return rect;
}

RECT SettingsDialog::CheckboxRect() const {
    // The checkbox sits 16 DIPs below the last zoom row (three rows of 34);
    // only the checkbox and its label are painted, but the whole strip is
    // clickable.
    const RECT content = ContentArea();
    const int top = ZoomRowTop() + 3 * Scale(34) + Scale(16);
    RECT rect{content.left + Scale(14), top, content.right - Scale(14),
              top + Scale(30)};
    return rect;
}

RECT SettingsDialog::CancelButtonRect() const {
    // Buttons are anchored to the bottom right corner: the dialog grows by
    // changing DialogHeightDip, and the buttons stay 20 DIPs from the edges.
    const RECT content = ContentArea();
    const int height = Scale(34);
    const int width = Scale(92);
    const int bottom = content.bottom - Scale(20);
    RECT rect{content.right - Scale(20) - width, bottom - height,
              content.right - Scale(20), bottom};
    return rect;
}

RECT SettingsDialog::OkButtonRect() const {
    // OK is the same size as Cancel, shifted left by its own width plus a
    // 10 DIP gap so the pair stays together at the bottom right.
    RECT rect = CancelButtonRect();
    ::OffsetRect(&rect, -(Scale(92) + Scale(10)), 0);
    return rect;
}

SettingsDialog::Target SettingsDialog::HitTest(POINT point) const {
    // Buttons and the checkbox are tested before the option rows because they
    // sit below them and must win if geometry ever overlaps after a future
    // layout tweak.
    if (Contains(OkButtonRect(), point)) {
        return Target::ButtonOk;
    }
    if (Contains(CancelButtonRect(), point)) {
        return Target::ButtonCancel;
    }
    if (Contains(CheckboxRect(), point)) {
        return Target::ShowSidebar;
    }
    for (int index = 0; index < 2; ++index) {
        if (Contains(LanguageRowRect(index), point)) {
            return index == 0 ? Target::LanguageEnglish : Target::LanguageChinese;
        }
    }
    for (int index = 0; index < 3; ++index) {
        if (Contains(ZoomRowRect(index), point)) {
            // Row order matches the visible list: fit width, fit page, actual
            // size (100%).
            switch (index) {
                case 0: return Target::ZoomFitWidth;
                case 1: return Target::ZoomFitPage;
                default: return Target::ZoomActualSize;
            }
        }
    }
    return Target::None;
}

void SettingsDialog::PaintContent(HDC dc, const RECT& content) {
    const theme::Palette& colors = theme::Colors();

    // Section captions share a fixed 20 DIP line and the 22 DIP content margin.
    auto sectionLabel = [&](const wchar_t* text, int top) {
        RECT rect{content.left + Scale(22), top, content.right - Scale(22),
                  top + Scale(20)};
        theme::DrawString(dc, text, rect, colors.textMuted, BoldFont(), DT_LEFT);
    };

    // One radio option row: optional rounded hover wash, the radio itself at a
    // fixed 22 DIP offset, and the label 20 DIPs to its right.
    auto optionRow = [&](const RECT& row, bool selected, const wchar_t* label,
                         bool hovered) {
        if (hovered) {
            RECT background = row;
            // Inset so adjacent hovered rows never merge into one block.
            ::InflateRect(&background, -Scale(4), -Scale(1));
            theme::FillRound(dc, background, Scale(5), colors.hover);
        }
        DrawRadio(dc, row.left + Scale(22), (row.top + row.bottom) / 2, selected, Dpi(),
                  colors);
        RECT textRect{row.left + Scale(42), row.top, row.right, row.bottom};
        theme::DrawString(dc, label, textRect, colors.text, BodyFont(), DT_LEFT);
    };

    sectionLabel(Tr(Str::SettingsLanguage), content.top + Scale(16));
    optionRow(LanguageRowRect(0), m_language == Lang::English,
              Tr(Str::MenuLanguageEnglish), m_hovered == Target::LanguageEnglish);
    optionRow(LanguageRowRect(1), m_language == Lang::Chinese,
              Tr(Str::MenuLanguageChinese), m_hovered == Target::LanguageChinese);

    // Hint line aligned with the option labels, below the two language rows
    // (rows end 68 DIPs below LanguageRowTop, so 70 leaves a small gap).
    RECT hint{content.left + Scale(42), LanguageRowTop() + Scale(70),
              content.right - Scale(22), LanguageRowTop() + Scale(92)};
    theme::DrawString(dc, Tr(Str::SettingsLanguageHint), hint, colors.textDisabled,
                      SmallFont(), DT_LEFT);

    sectionLabel(Tr(Str::SettingsDefaultZoom), content.top + Scale(148));
    optionRow(ZoomRowRect(0), m_settings.zoomMode == ZoomMode::FitWidth,
              Tr(Str::ZoomFitWidthShort), m_hovered == Target::ZoomFitWidth);
    optionRow(ZoomRowRect(1), m_settings.zoomMode == ZoomMode::FitPage,
              Tr(Str::ZoomFitPageShort), m_hovered == Target::ZoomFitPage);
    // "Actual size" is represented as a custom zoom of exactly 1.0; the epsilon
    // comparison tolerates floating point round trips through the settings file.
    optionRow(ZoomRowRect(2), m_settings.zoomMode == ZoomMode::Custom &&
                                  std::fabs(m_settings.zoom - 1.0) < 0.001,
              Tr(Str::ZoomActualSizeShort), m_hovered == Target::ZoomActualSize);

    // Checkbox for the sidebar.  The box is 17 DIPs square, vertically centred
    // in its 30 DIP row and offset 8 DIPs from the row's left edge; the label
    // starts 10 DIPs after the box.
    const RECT checkboxRow = CheckboxRect();
    const int boxSize = Scale(17);
    RECT box{checkboxRow.left + Scale(8),
             (checkboxRow.top + checkboxRow.bottom - boxSize) / 2,
             checkboxRow.left + Scale(8) + boxSize,
             (checkboxRow.top + checkboxRow.bottom + boxSize) / 2};
    DrawCheckbox(dc, box, m_settings.sidebarVisible, Dpi(), colors);
    RECT checkboxLabel{box.right + Scale(10), checkboxRow.top, checkboxRow.right,
                       checkboxRow.bottom};
    theme::DrawString(dc, Tr(Str::SettingsShowSidebar), checkboxLabel, colors.text,
                      BodyFont(), DT_LEFT);

    // Dialog buttons.  OK is painted as the primary (accent) action.
    DrawButton(dc, CancelButtonRect(), Tr(Str::SettingsCancel), false,
               m_hovered == Target::ButtonCancel, BodyFont(), Dpi(), colors);
    DrawButton(dc, OkButtonRect(), Tr(Str::SettingsOk), true,
               m_hovered == Target::ButtonOk, BodyFont(), Dpi(), colors);
}

void SettingsDialog::OnContentMouseMove(POINT point) {
    const Target hovered = HitTest(point);
    if (hovered != m_hovered) {
        // Hover changes both the radio row wash and the button background.
        m_hovered = hovered;
        Invalidate();
    }
    // Any interactive target, including whole rows, shows the hand cursor.
    const bool hand = hovered != Target::None;
    SetContentHandCursor(hand);
}

void SettingsDialog::OnContentMouseDown(POINT point) {
    // Clicking applies the change immediately; there is no separate "draft"
    // state except for the language, which the frame restores on cancel.
    const Target target = HitTest(point);
    switch (target) {
        case Target::LanguageEnglish:
            m_language = Lang::English;
            // Preview the language live: SetLanguage switches the translation
            // table, then the hook rebuilds fonts and the cached title.
            SetLanguage(Lang::English);
            OnLanguageChanged();
            break;
        case Target::LanguageChinese:
            m_language = Lang::Chinese;
            SetLanguage(Lang::Chinese);
            OnLanguageChanged();
            break;
        case Target::ZoomFitWidth:
            m_settings.zoomMode = ZoomMode::FitWidth;
            break;
        case Target::ZoomFitPage:
            m_settings.zoomMode = ZoomMode::FitPage;
            break;
        case Target::ZoomActualSize:
            // Actual size is stored as a custom zoom of 100%.
            m_settings.zoomMode = ZoomMode::Custom;
            m_settings.zoom = 1.0;
            break;
        case Target::ShowSidebar:
            m_settings.sidebarVisible = !m_settings.sidebarVisible;
            break;
        case Target::ButtonCancel:
            // The frame restores the original language after a rejected dialog.
            CloseModal(false);
            return;
        case Target::ButtonOk:
            OnAccept();
            return;
        case Target::None:
        default:
            return;
    }
    // Repaint for the new radio/checkbox state (language changes already did).
    Invalidate();
}

void SettingsDialog::OnAccept() {
    // Persist the language as a stable tag so the setting survives a locale
    // change of the operating system.  The zoom/sidebar values are already in
    // m_settings; the frame copies them out through UpdatedSettings().
    m_settings.languageTag = (m_language == Lang::Chinese) ? L"zh" : L"en";
    CloseModal(true);
}

void SettingsDialog::OnLanguageChanged() {
    // Recreate fonts for the new script and repaint every label.  Translations
    // are read through Tr() during each paint, so no other cache is stale.
    RefreshFonts();
    RefreshTitleText();
}

// =============================================================================
//  GoToPageDialog
// =============================================================================
GoToPageDialog::GoToPageDialog(int pageCount, int currentPage)
    // The dialog is only opened for an open document, but clamp defensively so
    // the range check in Submit() always has a valid upper bound.
    : m_pageCount(std::max(1, pageCount)), m_currentPage(std::max(0, currentPage)) {
    // Start on the current page and show it selected, so typing a new number
    // replaces it instead of appending.
    m_selectedPage = m_currentPage;
    m_text = std::to_wstring(m_currentPage + 1);
}

std::wstring GoToPageDialog::DialogTitle() const {
    return Tr(Str::GoToPageTitle);
}

RECT GoToPageDialog::InputRect() const {
    // A 40 DIP tall field, 24 DIPs in from each side, starting 56 DIPs below
    // the content top (leaving room for the prompt line above it).
    const RECT content = ContentArea();
    RECT rect{content.left + Scale(24), content.top + Scale(56),
              content.right - Scale(24), content.top + Scale(96)};
    return rect;
}

RECT GoToPageDialog::CancelButtonRect() const {
    // 88x32 DIP buttons anchored to the bottom right, mirroring the settings
    // dialog but with the smaller heights used by this compact dialog.
    const RECT content = ContentArea();
    const int height = Scale(32);
    const int width = Scale(88);
    const int bottom = content.bottom - Scale(18);
    RECT rect{content.right - Scale(20) - width, bottom - height,
              content.right - Scale(20), bottom};
    return rect;
}

RECT GoToPageDialog::OkButtonRect() const {
    // OK sits one button width plus a 10 DIP gap left of Cancel.
    RECT rect = CancelButtonRect();
    ::OffsetRect(&rect, -(Scale(88) + Scale(10)), 0);
    return rect;
}

void GoToPageDialog::PaintContent(HDC dc, const RECT& content) {
    const theme::Palette& colors = theme::Colors();

    // Prompt: "<page count>" formatted so the valid range is visible before
    // the user types anything.
    RECT promptRect{content.left + Scale(24), content.top + Scale(18),
                    content.right - Scale(24), content.top + Scale(42)};
    theme::DrawString(dc, Format(Tr(Str::GoToPagePrompt), m_pageCount), promptRect,
                      colors.text, BodyFont(), DT_LEFT);

    // Field background first, then a border that turns red after a failed
    // submit; the text and caret are drawn on top.
    const RECT input = InputRect();
    theme::FillRound(dc, input, Scale(5), colors.windowBackground);
    theme::StrokeRound(dc, input, Scale(5),
                       m_invalid ? RGB(210, 70, 60) : colors.pageBorder,
                       std::max(1, Scale(1)));

    RECT textRect = input;
    textRect.left += Scale(10);
    textRect.right -= Scale(10);
    theme::DrawString(dc, m_text, textRect, colors.text, BodyFont(), DT_LEFT);

    // Caret: placed after the text using the measured width.  When the whole
    // number is still selected the caret is hidden, mirroring a text selection.
    if (!m_replaceOnNextChar) {
        const int textWidth = theme::TextWidth(dc, m_text, BodyFont());
        // +1 places the caret just after the last glyph rather than on it.
        const int caretX = input.left + Scale(10) + textWidth + Scale(1);
        theme::DrawLine(dc, caretX, input.top + Scale(8), caretX,
                        input.bottom - Scale(8), colors.text, std::max(1, Scale(1)));
    } else if (!m_text.empty()) {
        // Simulated "select all": a pale accent block behind the number.  It is
        // measured with the same font, widened by 8 DIPs for padding, and the
        // text is drawn again so the glyphs stay on top of the translucent wash.
        RECT selection = input;
        selection.left += Scale(6);
        selection.right = selection.left + theme::TextWidth(dc, m_text, BodyFont()) +
                          Scale(8);
        selection.top += Scale(6);
        selection.bottom -= Scale(6);
        theme::FillRound(dc, selection, Scale(3), colors.accentSoft);
        theme::DrawString(dc, m_text, textRect, colors.text, BodyFont(), DT_LEFT);
    }

    // Validation message occupies the space between the field and the buttons;
    // the field border turning red is the primary cue.
    if (m_invalid) {
        RECT errorRect{content.left + Scale(24), input.bottom + Scale(8),
                       content.right - Scale(24), input.bottom + Scale(30)};
        theme::DrawString(dc, Tr(Str::GoToPageInvalid), errorRect, RGB(210, 70, 60),
                          SmallFont(), DT_LEFT);
    }

    // This dialog does not track per-button hover (OnContentMouseMove is not
    // overridden), so both buttons are painted in their idle state.
    DrawButton(dc, CancelButtonRect(), Tr(Str::SettingsCancel), false, false,
               BodyFont(), Dpi(), colors);
    DrawButton(dc, OkButtonRect(), Tr(Str::SettingsOk), true, false, BodyFont(), Dpi(),
               colors);
}

void GoToPageDialog::OnContentMouseDown(POINT point) {
    if (Contains(OkButtonRect(), point)) {
        Submit();
    } else if (Contains(CancelButtonRect(), point)) {
        CloseModal(false);
    } else if (Contains(InputRect(), point)) {
        // Clicking the field re-selects the whole number, like focusing a text
        // box for replacement.
        m_replaceOnNextChar = true;
        Invalidate();
    }
}

void GoToPageDialog::OnKeyDown(WPARAM key) {
    if (key == VK_BACK) {
        // Editing the number cancels the "selected" state; a subsequent Backspace
        // then removes digits instead of clearing the whole field.
        m_replaceOnNextChar = false;
        if (!m_text.empty()) {
            m_text.pop_back();
            // Old error state no longer applies to the shorter text.
            m_invalid = false;
            Invalidate();
        }
        return;
    }
}

void GoToPageDialog::OnChar(wchar_t character) {
    // Digits only: the page number is the sole input, and filtering here keeps
    // Submit() free of parsing edge cases.
    if (character < L'0' || character > L'9') {
        return;
    }
    if (m_replaceOnNextChar) {
        // First keystroke replaces the pre-selected current page number.
        m_text.clear();
        m_replaceOnNextChar = false;
    }
    // Six digits are more than enough for any real document and prevent
    // overflow-style input; the page count check below remains authoritative.
    if (m_text.size() >= 6) {
        return;
    }
    if (m_text == L"0") {
        m_text.clear();  // avoid leading zeros
    }
    m_text.push_back(character);
    m_invalid = false;
    Invalidate();
}

void GoToPageDialog::Submit() {
    if (m_text.empty()) {
        m_invalid = true;
        Invalidate();
        return;
    }
    // _wtoi stops at the first non-digit, but OnChar only allows digits, so the
    // conversion is exact for anything typed through the UI.
    const int value = _wtoi(m_text.c_str());
    // A user visible page number is one based; reject out of range values
    // instead of clamping, so a typo is obvious.
    if (value < 1 || value > m_pageCount) {
        m_invalid = true;
        Invalidate();
        return;
    }
    // Internally pages are zero based.
    m_selectedPage = value - 1;
    CloseModal(true);
}

void GoToPageDialog::OnAccept() {
    Submit();
}

}  // namespace pengpdf
