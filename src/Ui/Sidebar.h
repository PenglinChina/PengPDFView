// =============================================================================
//  Sidebar.h
//  Left hand navigation pane with two tabs:
//
//    * Contents  - the outline (bookmark tree) stored inside the PDF,
//    * Bookmarks - bookmarks created by the user, remembered per document.
//
//  The pane is owner drawn so that it matches the rest of the application and
//  so that both languages share exactly the same layout code.
//
//  The outline tree is copied into the sidebar and flattened into VisibleRow
//  entries according to the expanded state; that flat vector is what painting,
//  hit testing and scrolling operate on.  The scroll offset is kept in pixels
//  in the coordinate space of the row list (the header is excluded).
// =============================================================================
#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "App/Bookmarks.h"
#include "Pdf/PdfParser.h"
#include "Ui/ScrollBar.h"
#include "Ui/UiMessages.h"

namespace pengpdf {

class Sidebar {
public:
    // Registers the window class once per process; must succeed before Create.
    static bool RegisterWindowClass(HINSTANCE instance, const wchar_t* className);

    // Creates the child window; the Sidebar object must outlive the window and
    // is attached to it through CREATESTRUCT::lpCreateParams.
    bool Create(HWND parent, HINSTANCE instance, const wchar_t* className, UINT dpi);
    HWND Handle() const { return m_hwnd; }

    void SetDpi(UINT dpi);
    // Recreates the fonts for the new interface language and repaints.
    void ApplyLanguage();

    // Replaces the document outline.  Every node starts expanded/collapsed
    // according to OutlineEntry::initiallyOpen, which the parser derives from
    // the PDF /Count value.
    void SetOutline(const std::vector<pdf::OutlineEntry>& outline);

    // Replaces the user bookmark list for the current document.
    void SetBookmarks(const std::vector<Bookmark>& bookmarks);

    // Highlights the outline entry that contains the current page.
    void SetCurrentPage(int page);

    // Clears everything when a document is closed.
    void Reset();

private:
    // Tab order is also the left-to-right order in the header: tab `index`
    // starts at padding + index * kTabWidthDip.
    enum class Tab {
        Contents = 0,
        Bookmarks = 1,
    };

    // One painted row after flattening the active tab's data.  Rows are indexed
    // by position in m_rows; `index` points back into m_outline or m_bookmarks.
    struct VisibleRow {
        int index = 0;       // index into m_outline or m_bookmarks
        int depth = 0;       // indentation level
        bool hasChildren = false;
        bool expanded = false;
        int page = -1;       // zero based destination, -1 when there is none
    };

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam,
                                       LPARAM lParam);

    // Re-flattens the active tab into m_rows and recomputes m_contentHeight.
    void RebuildRows();
    // Recomputes m_activeRow (the outline section containing m_currentPage).
    // Called when the rows or the current page change, not from painting.
    void UpdateActiveRow();
    // Paint entry points; all receive physical client coordinates.
    void Paint(HDC dc, const RECT& client);
    void PaintHeader(HDC dc, const RECT& client);
    void PaintRows(HDC dc, const RECT& client);
    void PaintEmptyState(HDC dc, const RECT& content);

    int HeaderHeight() const;
    int RowHeight() const;
    RECT ContentRect(const RECT& client) const;
    // First y coordinate below the header's one pixel separator.
    int ContentTop() const;
    // Largest valid scroll offset for the current client height.
    int MaxScroll() const;
    void ClampScroll();
    // Returns a row index into m_rows, or -1 when the point is outside the row
    // area or past the last row.
    int RowAtPoint(POINT point) const;
    // True when the point is horizontally inside the expand/collapse triangle.
    // The caller has already established that the point belongs to `row`.
    bool IsTogglerHit(const VisibleRow& row, POINT point) const;
    void ToggleRow(int rowIndex);
    // Posts WM_APP_NAVIGATE_TO_PAGE when the row has a destination.
    void ActivateRow(int rowIndex);
    void SetHoveredRow(int row);
    void ShowBookmarkContextMenu(int bookmarkIndex);
    void ScrollBy(int deltaPixels);

    HWND m_hwnd = nullptr;
    UINT m_dpi = 96;

    Tab m_tab = Tab::Contents;
    // Source data for the active view.  m_expanded is parallel to m_outline and
    // remembers which outline nodes are open; m_rows is the derived flat list.
    std::vector<pdf::OutlineEntry> m_outline;
    std::vector<bool> m_expanded;
    std::vector<Bookmark> m_bookmarks;
    std::vector<VisibleRow> m_rows;

    // Scroll state in pixels of the row list, not including the header.
    int m_scrollOffset = 0;
    int m_contentHeight = 0;
    int m_currentPage = -1;
    int m_hoverRow = -1;
    // Cached result of the outline lookup; -1 in the bookmarks tab or when no
    // row contains the current page.  Avoids an O(rows) scan on every paint.
    int m_activeRow = -1;
    // Fractional wheel delta accumulated across WM_MOUSEWHEEL messages, so a
    // precision touchpad (which sends less than WHEEL_DELTA at a time) scrolls.
    int m_wheelRemainder = 0;
    bool m_trackingMouse = false;

    // Windowless overlay bar; Sidebar forwards paint, hit test and drag to it.
    ScrollBar m_scrollBar;

    // Fonts are rebuilt by SetDpi()/ApplyLanguage() and released in WM_DESTROY.
    HFONT m_rowFont = nullptr;
    HFONT m_headerFont = nullptr;
    HFONT m_smallFont = nullptr;
};

}  // namespace pengpdf
