// =============================================================================
//  SearchBar.h
//  Slim search field that floats over the page canvas.
//
//  The control only collects input: the actual search is performed by the
//  TextService and driven by the main window.  It reports edits and navigation
//  requests with the WM_APP_SEARCH_* messages so the frame stays in charge of
//  the document state.
// =============================================================================
#pragma once

#include <windows.h>

#include <string>

#include "Ui/UiMessages.h"

namespace pengpdf {

class SearchBar {
public:
    static bool RegisterWindowClass(HINSTANCE instance, const wchar_t* className);

    bool Create(HWND parent, HINSTANCE instance, const wchar_t* className, UINT dpi);
    HWND Handle() const { return m_hwnd; }
    // True while the bar is shown; the frame uses this to decide whether Escape
    // closes search or clears the text selection.
    bool IsBarVisible() const;

    // Height in physical pixels for the current DPI.
    int Height() const;
    // Preferred width in physical pixels; the frame may shrink it on narrow
    // windows.
    int PreferredWidth() const;

    // Recreates fonts and recomputes the button/field rectangles.
    void SetDpi(UINT dpi);
    void ApplyLanguage();

    // Shows the bar, optionally replaces the query, focuses the field and
    // starts the caret timer.  The frame raises the window in z-order.
    void ShowForSearch(const std::wstring& initialQuery);
    void HideBar();

    // Status text on the right ("3 of 57", "Searching...", "No results").
    // `isError` switches to the warning colour.
    void SetStatus(const std::wstring& text, bool isError);

    const std::wstring& Query() const { return m_query; }

private:
    // Clickable parts of the bar.  HitTest maps a point to one of them.
    enum class Button {
        None,
        Previous,
        Next,
        Close,
    };

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam,
                                       LPARAM lParam);

    // Computes the field/status/button rectangles from the client size.
    void Layout();
    // Draws the rounded panel, query text, caret and icons into `client`.
    void Paint(HDC dc, const RECT& client);
    Button HitTest(POINT point) const;
    // Sends a WM_APP_SEARCH_* message to the parent window.
    void Notify(UINT message);
    void SetHovered(Button button);
    // Inserts one typed character; the first keystroke after opening replaces
    // the prefilled selection.
    void AppendCharacter(wchar_t character);
    void DeleteLastCharacter();

    HWND m_hwnd = nullptr;
    UINT m_dpi = 96;
    std::wstring m_query;
    std::wstring m_status;
    bool m_statusIsError = false;

    RECT m_fieldRect{};
    RECT m_statusRect{};
    RECT m_previousRect{};
    RECT m_nextRect{};
    RECT m_closeRect{};

    Button m_hovered = Button::None;
    bool m_trackingMouse = false;
    bool m_focused = false;
    bool m_caretVisible = true;
    bool m_replaceOnNextCharacter = false;

    HFONT m_queryFont = nullptr;
    HFONT m_statusFont = nullptr;
};

}  // namespace pengpdf
