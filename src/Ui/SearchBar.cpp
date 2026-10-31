// =============================================================================
//  SearchBar.cpp
//  Owner drawn search field with previous / next / close buttons.
// =============================================================================
#include "Ui/SearchBar.h"

#include <windowsx.h>

#include <algorithm>

#include "App/Localization.h"
#include "Ui/Theme.h"

namespace pengpdf {
namespace {

constexpr int kSearchBarHeightDip = 40;
constexpr int kSearchBarWidthDip = 420;
constexpr UINT_PTR kCaretTimerId = 1;

// InflateRect with a negative amount; kept as a helper because the name is
// clearer than a raw InflateRect call with negative values at each use site.
RECT ShrinkRect(const RECT& rect, int dx, int dy) {
    RECT result = rect;
    ::InflateRect(&result, -dx, -dy);
    return result;
}

// Draws the up/down chevron for the previous/next buttons.  The geometry is
// computed from the button box so it scales with DPI.
void DrawChevron(HDC dc, const RECT& box, bool up, COLORREF color, int thickness) {
    const int centerX = (box.left + box.right) / 2;
    const int centerY = (box.top + box.bottom) / 2;
    const int half = std::max(3, static_cast<int>(box.right - box.left) / 6);
    POINT points[3] = {};
    if (up) {
        points[0].x = centerX - half;
        points[0].y = centerY + half / 2;
        points[1].x = centerX;
        points[1].y = centerY - half / 2;
        points[2].x = centerX + half;
        points[2].y = centerY + half / 2;
    } else {
        points[0].x = centerX - half;
        points[0].y = centerY - half / 2;
        points[1].x = centerX;
        points[1].y = centerY + half / 2;
        points[2].x = centerX + half;
        points[2].y = centerY - half / 2;
    }
    HPEN pen = ::CreatePen(PS_SOLID, thickness, color);
    HGDIOBJ oldPen = ::SelectObject(dc, pen);
    ::Polyline(dc, points, 3);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);
}

// Two crossed lines form the close mark; using DrawLine keeps the stroke
// width DPI aware without storing a pen.
void DrawCloseMark(HDC dc, const RECT& box, COLORREF color, int thickness) {
    const int centerX = (box.left + box.right) / 2;
    const int centerY = (box.top + box.bottom) / 2;
    const int arm = std::max(3, static_cast<int>(box.right - box.left) / 6);
    theme::DrawLine(dc, centerX - arm, centerY - arm, centerX + arm, centerY + arm,
                    color, thickness);
    theme::DrawLine(dc, centerX + arm, centerY - arm, centerX - arm, centerY + arm,
                    color, thickness);
}

// Magnifier icon drawn with GDI primitives so the executable needs no image
// resources.
void DrawMagnifier(HDC dc, const RECT& box, COLORREF color, int thickness) {
    const int centerX = (box.left + box.right) / 2 - 1;
    const int centerY = (box.top + box.bottom) / 2 - 1;
    const int radius = std::max(3, static_cast<int>(box.right - box.left) / 3);
    HPEN pen = ::CreatePen(PS_SOLID, thickness, color);
    HGDIOBJ oldPen = ::SelectObject(dc, pen);
    HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
    ::Ellipse(dc, centerX - radius, centerY - radius, centerX + radius, centerY + radius);
    ::SelectObject(dc, oldBrush);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);
    theme::DrawLine(dc, centerX + radius - 1, centerY + radius - 1,
                    centerX + radius + 3, centerY + radius + 3, color, thickness);
}

}  // namespace

// The class uses CS_DROPSHADOW so the floating field separates itself from the
// page without any extra composition work.
bool SearchBar::RegisterWindowClass(HINSTANCE instance, const wchar_t* className) {
    WNDCLASSEXW description = {};
    description.cbSize = sizeof(description);
    description.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
    description.lpfnWndProc = &SearchBar::WindowProc;
    description.hInstance = instance;
    description.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    description.lpszClassName = className;
    return ::RegisterClassExW(&description) != 0;
}

// Creates the hidden child window; the frame shows and positions it later.
bool SearchBar::Create(HWND parent, HINSTANCE instance, const wchar_t* className,
                       UINT dpi) {
    m_dpi = dpi;
    m_hwnd = ::CreateWindowExW(0, className, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0,
                               0, 0, parent, nullptr, instance, this);
    if (m_hwnd == nullptr) {
        return false;
    }
    SetDpi(dpi);
    return true;
}

// Visible state is used by Escape handling in the frame.
bool SearchBar::IsBarVisible() const {
    // IsWindowVisible also succeeds for child windows whose parent is hidden;
    // the frame only calls this while the main window is visible.
    return m_hwnd != nullptr && ::IsWindowVisible(m_hwnd) != FALSE;
}

// Height in physical pixels for the current DPI.
int SearchBar::Height() const {
    return theme::Scale(kSearchBarHeightDip, m_dpi);
}

// Preferred width; the frame clamps it when the canvas is narrow.
int SearchBar::PreferredWidth() const {
    return theme::Scale(kSearchBarWidthDip, m_dpi);
}

// Recreates the two fonts for the new DPI and recomputes all rectangles.
void SearchBar::SetDpi(UINT dpi) {
    m_dpi = dpi;
    theme::ReleaseFont(m_queryFont);
    theme::ReleaseFont(m_statusFont);
    m_queryFont = theme::CreateUiFont(dpi, 9.5, FW_NORMAL, IsChineseInterface());
    m_statusFont = theme::CreateUiFont(dpi, 8.5, FW_NORMAL, IsChineseInterface());
    Layout();
    if (m_hwnd != nullptr) {
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

// Language change only affects the fonts and placeholder text; the query and
// status strings are owned by the frame/control and stay as they are.
void SearchBar::ApplyLanguage() {
    // Recreating the fonts also re-runs Layout, which is what picks up the
    // different text metrics of the Chinese UI font.
    SetDpi(m_dpi);
}

// Shows the bar and prepares the "type to replace" selection state.
void SearchBar::ShowForSearch(const std::wstring& initialQuery) {
    if (m_hwnd == nullptr) {
        return;
    }
    if (!initialQuery.empty()) {
        m_query = initialQuery;
    }
    // A prefilled query (for example a selected word) is replaced by the next
    // typed character, which is the behaviour of common search fields.
    m_replaceOnNextCharacter = !m_query.empty();
    m_caretVisible = true;
    Layout();
    ::ShowWindow(m_hwnd, SW_SHOW);
    ::SetFocus(m_hwnd);
    ::SetTimer(m_hwnd, kCaretTimerId, 530, nullptr);
    ::InvalidateRect(m_hwnd, nullptr, FALSE);
}

// Hides the bar and stops the caret timer; focus is restored by the frame.
void SearchBar::HideBar() {
    if (m_hwnd == nullptr) {
        return;
    }
    ::KillTimer(m_hwnd, kCaretTimerId);
    m_focused = false;
    m_hovered = Button::None;
    ::ShowWindow(m_hwnd, SW_HIDE);
}

// Updates the right-aligned status text ("3 of 57", "No results", ...).
void SearchBar::SetStatus(const std::wstring& text, bool isError) {
    m_status = text;
    m_statusIsError = isError;
    // The status area is small; repaint the bar instead of tracking its rect.
    if (m_hwnd != nullptr) {
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

// Computes all child rectangles from the current client size.
void SearchBar::Layout() {
    if (m_hwnd == nullptr) {
        return;
    }
    RECT client = {};
    ::GetClientRect(m_hwnd, &client);
    const int padding = theme::Scale(8, m_dpi);
    const int button = theme::Scale(24, m_dpi);
    const int centerY = (client.top + client.bottom) / 2;

    // Buttons are right aligned in the order close, next, previous.
    m_closeRect = RECT{client.right - padding - button, centerY - button / 2,
                       client.right - padding, centerY + button / 2};
    m_nextRect = RECT{m_closeRect.left - button, m_closeRect.top,
                      m_closeRect.left, m_closeRect.bottom};
    m_previousRect = RECT{m_nextRect.left - button, m_nextRect.top, m_nextRect.left,
                         m_nextRect.bottom};

    // Fixed width status area leaves room for "999 of 999" without reflowing
    // the field while a search is running.
    const int statusWidth = theme::Scale(96, m_dpi);
    m_statusRect = RECT{m_previousRect.left - statusWidth, client.top,
                        m_previousRect.left - theme::Scale(2, m_dpi), client.bottom};

    const int fieldLeft = client.left + theme::Scale(30, m_dpi);
    m_fieldRect = RECT{fieldLeft, client.top + theme::Scale(4, m_dpi),
                       m_statusRect.left - theme::Scale(6, m_dpi),
                       client.bottom - theme::Scale(4, m_dpi)};
}

// Draws the complete control into the panel rectangle (already shrunk for the
// internal shadow by the WM_PAINT caller).
void SearchBar::Paint(HDC dc, const RECT& client) {
    const theme::Palette& colors = theme::Colors();
    const int radius = theme::Scale(7, m_dpi);

    // Soft drop shadow and rounded panel.  The shadow is drawn inside the
    // client area, which is why WM_PAINT leaves a few pixels at the edge.
    RECT shadow = client;
    ::OffsetRect(&shadow, theme::Scale(1, m_dpi), theme::Scale(2, m_dpi));
    theme::FillRound(dc, shadow, radius, colors.pageShadow);
    theme::FillRound(dc, client, radius, colors.windowBackground);
    theme::StrokeRound(dc, client, radius, colors.pageBorder, 1);

    const int thickness = std::max(1, theme::Scale(1, m_dpi));
    const int centerY = (client.top + client.bottom) / 2;

    RECT icon{client.left + theme::Scale(9, m_dpi), centerY - theme::Scale(6, m_dpi),
              client.left + theme::Scale(21, m_dpi), centerY + theme::Scale(6, m_dpi)};
    DrawMagnifier(dc, icon, colors.textMuted, thickness);

    // Query field.  Long queries scroll horizontally so the caret always stays
    // visible; the drawing is clipped to the field rectangle.
    ::SaveDC(dc);
    ::IntersectClipRect(dc, m_fieldRect.left, m_fieldRect.top, m_fieldRect.right,
                        m_fieldRect.bottom);
    const int fieldWidth = m_fieldRect.right - m_fieldRect.left;
    if (m_query.empty()) {
        RECT placeholder = m_fieldRect;
        placeholder.left += theme::Scale(2, m_dpi);
        theme::DrawStringEllipsis(dc, Tr(Str::SearchPlaceholder), placeholder,
                                  colors.textDisabled, m_queryFont, DT_LEFT);
    } else {
        const int textWidth = theme::TextWidth(dc, m_query, m_queryFont);
        const int offset = std::max(0, textWidth - fieldWidth + theme::Scale(4, m_dpi));

        if (m_replaceOnNextCharacter && m_focused) {
            RECT selection{m_fieldRect.left, m_fieldRect.top + theme::Scale(4, m_dpi),
                           m_fieldRect.left + std::min(textWidth, fieldWidth),
                           m_fieldRect.bottom - theme::Scale(4, m_dpi)};
            theme::FillRound(dc, selection, theme::Scale(3, m_dpi), colors.accentSoft);
        }

        RECT textRect = m_fieldRect;
        textRect.left -= offset;
        theme::DrawString(dc, m_query, textRect, colors.text, m_queryFont, DT_LEFT);

        if (m_focused && m_caretVisible) {
            const int caretX = m_fieldRect.left + std::min(textWidth - offset + 1,
                                                           fieldWidth - 1);
            theme::DrawLine(dc, caretX, m_fieldRect.top + theme::Scale(4, m_dpi),
                            caretX, m_fieldRect.bottom - theme::Scale(4, m_dpi),
                            colors.text, thickness);
        }
    }
    ::RestoreDC(dc, -1);

    // Status text (match count / progress / error).
    if (!m_status.empty()) {
        RECT statusRect = m_statusRect;
        statusRect.right -= theme::Scale(2, m_dpi);
        theme::DrawString(dc, m_status, statusRect,
                          m_statusIsError ? RGB(200, 70, 60) : colors.textMuted,
                          m_statusFont, DT_RIGHT);
    }

    // Buttons with hover feedback.
    auto paintButton = [&](const RECT& rect, Button button) {
        if (m_hovered == button) {
            theme::FillRound(dc, ShrinkRect(rect, theme::Scale(1, m_dpi),
                                          theme::Scale(3, m_dpi)),
                             theme::Scale(4, m_dpi), colors.hover);
        }
    };
    paintButton(m_previousRect, Button::Previous);
    paintButton(m_nextRect, Button::Next);
    paintButton(m_closeRect, Button::Close);
    DrawChevron(dc, m_previousRect, true, colors.textMuted, thickness);
    DrawChevron(dc, m_nextRect, false, colors.textMuted, thickness);
    DrawCloseMark(dc, m_closeRect, colors.textMuted, thickness);
}

// Maps a client point to one of the three buttons.
SearchBar::Button SearchBar::HitTest(POINT point) const {
    auto contains = [point](const RECT& rect) {
        return point.x >= rect.left && point.x < rect.right && point.y >= rect.top &&
               point.y < rect.bottom;
    };
    // The field itself is not a button: clicks there only focus the control,
    // there is no text caret placement in this single-line editor yet.
    if (contains(m_previousRect)) {
        return Button::Previous;
    }
    if (contains(m_nextRect)) {
        return Button::Next;
    }
    if (contains(m_closeRect)) {
        return Button::Close;
    }
    return Button::None;
}

// Sends a search notification to the frame without blocking.
void SearchBar::Notify(UINT message) {
    // Asynchronous so the control never re-enters the frame's command handler
    // from inside its own message processing.
    if (m_hwnd != nullptr) {
        ::PostMessageW(::GetParent(m_hwnd), message, 0, 0);
    }
}

// Hover feedback only repaints when the hovered button changes.
void SearchBar::SetHovered(Button button) {
    if (m_hovered == button) {
        return;
    }
    m_hovered = button;
    if (m_hwnd != nullptr) {
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

// Appends one typed character and tells the frame that the query changed.
void SearchBar::AppendCharacter(wchar_t character) {
    // Replacing the initial selection happens on the first printable keystroke.
    if (m_replaceOnNextCharacter) {
        m_query.clear();
        m_replaceOnNextCharacter = false;
    }
    if (m_query.size() >= 256) {
        return;
    }
    m_query.push_back(character);
    m_caretVisible = true;
    Notify(WM_APP_SEARCH_TEXT_CHANGED);
    if (m_hwnd != nullptr) {
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

// Handles Backspace, including the "clear prefilled query" case.
void SearchBar::DeleteLastCharacter() {
    // Backspace on a prefilled selection clears it instead of deleting one
    // character, mirroring the replace-on-type behaviour.
    if (m_replaceOnNextCharacter) {
        m_query.clear();
        m_replaceOnNextCharacter = false;
    } else if (!m_query.empty()) {
        m_query.pop_back();
    }
    m_caretVisible = true;
    Notify(WM_APP_SEARCH_TEXT_CHANGED);
    if (m_hwnd != nullptr) {
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

LRESULT CALLBACK SearchBar::WindowProc(HWND window, UINT message, WPARAM wParam,
                                       LPARAM lParam) {
    SearchBar* self =
        reinterpret_cast<SearchBar*>(::GetWindowLongPtrW(window, GWLP_USERDATA));

    if (message == WM_CREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<SearchBar*>(create->lpCreateParams);
        ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self != nullptr) {
            self->m_hwnd = window;
        }
        return 0;
    }
    if (self == nullptr) {
        return ::DefWindowProcW(window, message, wParam, lParam);
    }

    switch (message) {
        case WM_ERASEBKGND:
            return 1;

        case WM_SIZE:
            self->Layout();
            ::InvalidateRect(window, nullptr, FALSE);
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT paint = {};
            HDC dc = ::BeginPaint(window, &paint);
            RECT client = {};
            ::GetClientRect(window, &client);
            // Leave one pixel for the shadow drawn inside the client area.
            RECT panel{client.left, client.top, client.right - theme::Scale(2, self->m_dpi),
                       client.bottom - theme::Scale(3, self->m_dpi)};
            if (panel.right > panel.left && panel.bottom > panel.top) {
                HDC memoryDc = ::CreateCompatibleDC(dc);
                HBITMAP bitmap = ::CreateCompatibleBitmap(dc, client.right, client.bottom);
                HGDIOBJ oldBitmap = ::SelectObject(memoryDc, bitmap);
                RECT full = client;
                theme::FillSolid(memoryDc, full, theme::Colors().windowBackground);
                self->Paint(memoryDc, panel);
                ::BitBlt(dc, 0, 0, client.right, client.bottom, memoryDc, 0, 0, SRCCOPY);
                ::SelectObject(memoryDc, oldBitmap);
                ::DeleteObject(bitmap);
                ::DeleteDC(memoryDc);
            }
            ::EndPaint(window, &paint);
            return 0;
        }

        case WM_CHAR:
            if (wParam >= 32 && wParam < 0x10000 && (wParam & 0xFFFF) != 127) {
                self->AppendCharacter(static_cast<wchar_t>(wParam));
            }
            return 0;

        case WM_KEYDOWN:
            if (wParam == VK_BACK) {
                self->DeleteLastCharacter();
            } else if (wParam == VK_RETURN) {
                const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
                self->Notify(shift ? WM_APP_SEARCH_PREVIOUS : WM_APP_SEARCH_NEXT);
            } else if (wParam == VK_ESCAPE) {
                self->Notify(WM_APP_SEARCH_CLOSE);
            }
            return 0;

        case WM_LBUTTONDOWN: {
            ::SetFocus(window);
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            switch (self->HitTest(point)) {
                case Button::Previous:
                    self->Notify(WM_APP_SEARCH_PREVIOUS);
                    return 0;
                case Button::Next:
                    self->Notify(WM_APP_SEARCH_NEXT);
                    return 0;
                case Button::Close:
                    self->Notify(WM_APP_SEARCH_CLOSE);
                    return 0;
                default:
                    break;
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (!self->m_trackingMouse) {
                TRACKMOUSEEVENT track = {};
                track.cbSize = sizeof(track);
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = window;
                ::TrackMouseEvent(&track);
                self->m_trackingMouse = true;
            }
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            self->SetHovered(self->HitTest(point));
            return 0;
        }

        case WM_MOUSELEAVE:
            self->m_trackingMouse = false;
            self->SetHovered(Button::None);
            return 0;

        case WM_SETFOCUS:
            self->m_focused = true;
            self->m_caretVisible = true;
            ::SetTimer(window, kCaretTimerId, 530, nullptr);
            ::InvalidateRect(window, nullptr, FALSE);
            return 0;

        case WM_KILLFOCUS:
            self->m_focused = false;
            ::KillTimer(window, kCaretTimerId);
            ::InvalidateRect(window, nullptr, FALSE);
            return 0;

        case WM_TIMER:
            if (wParam == kCaretTimerId) {
                self->m_caretVisible = !self->m_caretVisible;
                ::InvalidateRect(window, nullptr, FALSE);
            }
            return 0;

        case WM_SETCURSOR: {
            POINT point = {};
            ::GetCursorPos(&point);
            ::ScreenToClient(window, &point);
            if (self->HitTest(point) != Button::None) {
                ::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            ::SetCursor(::LoadCursorW(nullptr, IDC_IBEAM));
            return TRUE;
        }

        case WM_DESTROY:
            ::KillTimer(window, kCaretTimerId);
            theme::ReleaseFont(self->m_queryFont);
            theme::ReleaseFont(self->m_statusFont);
            self->m_hwnd = nullptr;
            return 0;

        default:
            break;
    }
    return ::DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace pengpdf

