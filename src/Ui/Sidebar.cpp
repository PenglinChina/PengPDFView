// =============================================================================
//  Sidebar.cpp
//  Owner-drawn navigation pane.
// =============================================================================
#include "Ui/Sidebar.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "App/Localization.h"
#include "Core/Utf.h"
#include "Ui/Theme.h"

namespace pengpdf {
namespace {

// All layout constants are DIPs; every use goes through theme::Scale so the
// pane keeps the same proportions at any monitor DPI.
constexpr int kHeaderHeightDip = 44;
constexpr int kRowHeightDip = 28;
// A row starts at kBaseIndentDip and each nesting level adds kIndentStepDip;
// the toggler (for nodes with children) occupies the first kTogglerWidthDip.
constexpr int kBaseIndentDip = 12;
constexpr int kIndentStepDip = 14;
constexpr int kTogglerWidthDip = 18;
// Each header tab has a fixed width, which also keeps the underline centred
// regardless of the translated label length.
constexpr int kTabWidthDip = 92;
constexpr int kHeaderPaddingDip = 12;
// Extra scrollable space below the last row so it does not hug the bottom edge.
constexpr int kContentBottomPaddingDip = 12;

// Header tab rectangle in client coordinates.  `index` is the Tab ordinal, so
// the layout order and the click order always agree.
RECT TabRectFor(int index, UINT dpi) {
    const int width = theme::Scale(kTabWidthDip, dpi);
    const int left = theme::Scale(kHeaderPaddingDip, dpi) + index * width;
    RECT rect{left, 0, left + width, theme::Scale(kHeaderHeightDip, dpi)};
    return rect;
}

// Draws a small triangle that indicates the expansion state of an outline row.
// The triangle points down when expanded and right when collapsed, the common
// tree-view convention.  `half` is half of its base, and the /2 terms keep the
// shape compact around the vertical text centre.
void DrawToggler(HDC dc, int centerX, int centerY, bool expanded, COLORREF color,
                 UINT dpi) {
    const int half = theme::Scale(3, dpi);
    POINT points[3] = {};
    if (expanded) {
        points[0].x = centerX - half;
        points[0].y = centerY - half / 2;
        points[1].x = centerX + half;
        points[1].y = centerY - half / 2;
        points[2].x = centerX;
        points[2].y = centerY + half;
    } else {
        points[0].x = centerX - half / 2;
        points[0].y = centerY - half;
        points[1].x = centerX - half / 2;
        points[1].y = centerY + half;
        points[2].x = centerX + half;
        points[2].y = centerY;
    }

    // Polygon fills and closes the triangle; NULL_PEN avoids a dark outline.
    HBRUSH brush = ::CreateSolidBrush(color);
    HGDIOBJ oldBrush = ::SelectObject(dc, brush);
    HGDIOBJ oldPen = ::SelectObject(dc, ::GetStockObject(NULL_PEN));
    ::Polygon(dc, points, 3);
    ::SelectObject(dc, oldPen);
    ::SelectObject(dc, oldBrush);
    ::DeleteObject(brush);
}

// A tiny bookmark star used at the left edge of the bookmark list.
// Ten vertices alternate between `radius` and 45% of it, starting straight up.
void DrawStar(HDC dc, int centerX, int centerY, int radius, COLORREF color) {
    POINT points[10] = {};
    for (int index = 0; index < 10; ++index) {
        const double angle =
            -3.14159265358979323846 / 2.0 + index * 3.14159265358979323846 / 5.0;
        const double r = (index % 2 == 0) ? radius : radius * 0.45;
        points[index].x = centerX + static_cast<int>(r * std::cos(angle));
        points[index].y = centerY + static_cast<int>(r * std::sin(angle));
    }

    HBRUSH brush = ::CreateSolidBrush(color);
    HGDIOBJ oldBrush = ::SelectObject(dc, brush);
    HGDIOBJ oldPen = ::SelectObject(dc, ::GetStockObject(NULL_PEN));
    ::Polygon(dc, points, 10);
    ::SelectObject(dc, oldPen);
    ::SelectObject(dc, oldBrush);
    ::DeleteObject(brush);
}

}  // namespace

bool Sidebar::RegisterWindowClass(HINSTANCE instance, const wchar_t* className) {
    WNDCLASSEXW description = {};
    description.cbSize = sizeof(description);
    // The pane repaints itself completely into a back buffer, so class-level
    // invalidation on resize is enough; no background brush is installed and
    // WM_ERASEBKGND is suppressed below.
    description.style = CS_HREDRAW | CS_VREDRAW;
    description.lpfnWndProc = &Sidebar::WindowProc;
    description.hInstance = instance;
    description.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    description.lpszClassName = className;
    return ::RegisterClassExW(&description) != 0;
}

bool Sidebar::Create(HWND parent, HINSTANCE instance, const wchar_t* className,
                     UINT dpi) {
    // Store the DPI first because WM_SIZE during creation can already reach
    // ClampScroll(); `this` is delivered through lpCreateParams.
    m_dpi = dpi;
    m_hwnd = ::CreateWindowExW(0, className, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                               0, 0, 0, 0, parent, nullptr, instance, this);
    if (m_hwnd == nullptr) {
        return false;
    }
    SetDpi(dpi);
    return true;
}

void Sidebar::SetDpi(UINT dpi) {
    m_dpi = dpi;
    m_scrollBar.SetDpi(dpi);

    // Fonts are owned by the sidebar: always release the previous set before
    // creating replacements, otherwise every DPI/language change leaks four
    // GDI fonts.
    theme::ReleaseFont(m_rowFont);
    theme::ReleaseFont(m_headerFont);
    theme::ReleaseFont(m_smallFont);

    const bool chinese = IsChineseInterface();
    m_rowFont = theme::CreateUiFont(dpi, 9.0, FW_NORMAL, chinese);
    m_headerFont = theme::CreateUiFont(dpi, 9.0, FW_SEMIBOLD, chinese);
    m_smallFont = theme::CreateUiFont(dpi, 8.0, FW_NORMAL, chinese);

    // Row height and the viewport depend on the DPI, so the old offset may now
    // be past the end of the content.
    ClampScroll();
    if (m_hwnd != nullptr) {
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void Sidebar::ApplyLanguage() {
    // The language only changes the font family (and later translations read
    // through Tr() during paint), so re-running SetDpi is sufficient.
    SetDpi(m_dpi);
}

void Sidebar::SetOutline(const std::vector<pdf::OutlineEntry>& outline) {
    m_outline = outline;
    // m_expanded is parallel to m_outline and is the only state the flattening
    // step consults.  Start from the parser hint for every node.
    m_expanded.assign(outline.size(), true);
    for (size_t index = 0; index < outline.size(); ++index) {
        m_expanded[index] = outline[index].initiallyOpen;
    }
    // A new document always starts on the contents tab at the top.
    m_tab = Tab::Contents;
    m_scrollOffset = 0;
    m_hoverRow = -1;
    RebuildRows();
    if (m_hwnd != nullptr) {
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void Sidebar::SetBookmarks(const std::vector<Bookmark>& bookmarks) {
    // The bookmark list can be refreshed while the Bookmarks tab is visible, so
    // keep the tab and scroll position; RebuildRows clamps the offset.
    m_bookmarks = bookmarks;
    RebuildRows();
    if (m_hwnd != nullptr) {
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void Sidebar::SetCurrentPage(int page) {
    if (m_currentPage == page) {
        return;
    }
    m_currentPage = page;
    UpdateActiveRow();

    // Keep the outline entry that contains the current page inside the visible
    // area, but only scroll when it is actually off screen.
    if (m_tab == Tab::Contents && !m_rows.empty() && m_hwnd != nullptr) {
        // UpdateActiveRow applied the same rule as the highlight: the last row
        // whose page is <= the current page, skipping rows without a target.
        const int activeRow = m_activeRow;
        if (activeRow >= 0) {
            RECT client = {};
            ::GetClientRect(m_hwnd, &client);
            // The header is not part of the scrollable viewport.
            const int viewport = std::max(0, static_cast<int>(client.bottom) - ContentTop());
            // Row geometry in scroll coordinates: row 0 sits at offset 0.
            const int rowTop = activeRow * RowHeight();
            const int rowBottom = rowTop + RowHeight();
            if (rowTop < m_scrollOffset) {
                m_scrollOffset = rowTop;
            } else if (rowBottom > m_scrollOffset + viewport) {
                m_scrollOffset = rowBottom - viewport;
            }
            ClampScroll();
        }
    }

    if (m_hwnd != nullptr) {
        // The accent highlight and scroll position both changed.
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void Sidebar::Reset() {
    // Clear all document-dependent data, including the derived row list and the
    // hit-test indices, so stale rows can never be activated after a close.
    m_outline.clear();
    m_expanded.clear();
    m_bookmarks.clear();
    m_rows.clear();
    m_scrollOffset = 0;
    m_contentHeight = 0;
    m_currentPage = -1;
    m_hoverRow = -1;
    m_activeRow = -1;
    m_wheelRemainder = 0;
    m_tab = Tab::Contents;
    if (m_hwnd != nullptr) {
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

int Sidebar::HeaderHeight() const {
    return theme::Scale(kHeaderHeightDip, m_dpi);
}

int Sidebar::RowHeight() const {
    return theme::Scale(kRowHeightDip, m_dpi);
}

int Sidebar::ContentTop() const {
    // One extra pixel below the header band leaves a clean seam under the
    // header separator; every content calculation uses this same origin.
    return HeaderHeight() + 1;
}

RECT Sidebar::ContentRect(const RECT& client) const {
    RECT content = client;
    content.top = ContentTop();
    return content;
}

int Sidebar::MaxScroll() const {
    if (m_hwnd == nullptr) {
        return 0;
    }
    RECT client = {};
    ::GetClientRect(m_hwnd, &client);
    // Only the area below the header scrolls; m_contentHeight already includes
    // the bottom padding added by RebuildRows.
    const int viewport = std::max(0, static_cast<int>(client.bottom) - ContentTop());
    return std::max(0, m_contentHeight - viewport);
}

void Sidebar::ClampScroll() {
    // Keeps the offset inside [0, MaxScroll] after a resize, DPI change or data
    // refresh; callers then repaint.
    m_scrollOffset = std::clamp(m_scrollOffset, 0, MaxScroll());
}

void Sidebar::RebuildRows() {
    m_rows.clear();

    if (m_tab == Tab::Contents) {
        // The parser returns outline entries in depth-first document order.
        // When a collapsed node is met, `skipDeeperThan` remembers its level and
        // all following entries with a greater level are hidden.  The sentinel
        // is cleared as soon as an entry at the same or a shallower level is
        // reached, which ends the hidden subtree.
        int skipDeeperThan = -1;
        for (size_t index = 0; index < m_outline.size(); ++index) {
            const pdf::OutlineEntry& entry = m_outline[index];
            if (skipDeeperThan >= 0 && entry.level > skipDeeperThan) {
                continue;
            }
            skipDeeperThan = -1;

            VisibleRow row;
            row.index = static_cast<int>(index);
            row.depth = entry.level;
            row.hasChildren = entry.hasChildren;
            // m_expanded is parallel to m_outline; guard against a size mismatch
            // and treat missing entries as collapsed.
            row.expanded = index < m_expanded.size() && m_expanded[index];
            row.page = entry.pageIndex;
            m_rows.push_back(row);

            if (entry.hasChildren && !row.expanded) {
                skipDeeperThan = entry.level;
            }
        }
    } else {
        // Bookmarks are a flat list: every entry becomes one top level row.
        for (size_t index = 0; index < m_bookmarks.size(); ++index) {
            VisibleRow row;
            row.index = static_cast<int>(index);
            row.page = m_bookmarks[index].page;
            m_rows.push_back(row);
        }
    }

    // Add the padding as content, not as a separate margin, so the scrollbar
    // range matches what the user can actually scroll through.
    m_contentHeight = static_cast<int>(m_rows.size()) * RowHeight() +
                      theme::Scale(kContentBottomPaddingDip, m_dpi);
    // Collapsing a node can leave the previous offset past the end of the list.
    ClampScroll();
    // The active section depends on which rows are currently visible.
    UpdateActiveRow();
}

void Sidebar::UpdateActiveRow() {
    m_activeRow = -1;
    if (m_tab != Tab::Contents) {
        return;
    }
    // Outline destinations are in document order, so the last row at or before
    // the current page is the section that contains it.  Rows without a
    // destination (page < 0) are skipped.
    for (size_t index = 0; index < m_rows.size(); ++index) {
        if (m_rows[index].page >= 0 && m_rows[index].page <= m_currentPage) {
            m_activeRow = static_cast<int>(index);
        }
    }
}

void Sidebar::Paint(HDC dc, const RECT& client) {
    const theme::Palette& colors = theme::Colors();
    // Painting order: opaque background, header, scrolling rows, and finally the
    // separator against the canvas so row highlights cannot cover it.
    theme::FillSolid(dc, client, colors.sidebarBackground);

    PaintHeader(dc, client);
    PaintRows(dc, client);

    // Right edge separator against the page canvas.
    theme::FillSolid(dc, RECT{client.right - 1, client.top, client.right, client.bottom},
                     colors.separator);
}

void Sidebar::PaintHeader(HDC dc, const RECT& client) {
    const theme::Palette& colors = theme::Colors();
    const int headerHeight = HeaderHeight();
    // Slightly lighter than the row area, like a title bar.
    theme::FillSolid(dc, RECT{client.left, client.top, client.right, headerHeight},
                     colors.windowBackground);

    for (int index = 0; index < 2; ++index) {
        const RECT tab = TabRectFor(index, m_dpi);
        const bool active = static_cast<int>(m_tab) == index;
        const wchar_t* label = Tr(index == 0 ? Str::TabContents : Str::TabBookmarks);

        // Inactive tabs use the muted colour so the selected one stands out even
        // without relying on the underline alone.
        theme::DrawString(dc, label, tab, active ? colors.text : colors.textMuted,
                          m_headerFont, DT_CENTER);
        if (active) {
            // Underline is as wide as the measured label, not the whole tab, so
            // it reads as a text accent in both languages.
            const int textWidth = theme::TextWidth(dc, label, m_headerFont);
            const int centerX = (tab.left + tab.right) / 2;
            RECT underline{centerX - textWidth / 2, headerHeight - theme::Scale(4, m_dpi),
                           centerX + textWidth / 2,
                           headerHeight - theme::Scale(1, m_dpi)};
            theme::FillRound(dc, underline, theme::Scale(1, m_dpi), colors.accent);
        }
    }

    // One pixel divider between the tabs and the scrolling content.
    theme::FillSolid(dc, RECT{client.left, headerHeight - 1, client.right, headerHeight},
                     colors.separator);
}

void Sidebar::PaintRows(HDC dc, const RECT& client) {
    const theme::Palette& colors = theme::Colors();
    const RECT content = ContentRect(client);
    if (content.bottom <= content.top) {
        return;
    }

    if (m_rows.empty()) {
        PaintEmptyState(dc, content);
    } else {
        const int rowHeight = RowHeight();
        // Index of the first row that is at least partially visible; integer
        // division deliberately rounds down so the clipped top row is drawn.
        const int firstRow = std::max(0, m_scrollOffset / rowHeight);
        // Keep text clear of the overlay scrollbar's hit strip on the right.
        const int rightEdge = content.right - m_scrollBar.HitThickness();

        // Highlight the section that contains the current page.  The cached
        // value is maintained by SetCurrentPage/RebuildRows; painting only
        // reads it, which keeps huge outlines cheap to scroll.
        const int activeRow = m_activeRow;

        for (int rowIndex = firstRow; rowIndex < static_cast<int>(m_rows.size());
             ++rowIndex) {
            // Row coordinates are in content space; subtracting the offset maps
            // them to the client.  Stop as soon as the list leaves the viewport.
            const int top = content.top + rowIndex * rowHeight - m_scrollOffset;
            if (top >= content.bottom) {
                break;
            }
            if (top + rowHeight <= content.top) {
                continue;
            }

            RECT rowRect{content.left + 1, top, rightEdge, top + rowHeight};
            const bool active = rowIndex == activeRow;
            const bool hovered = rowIndex == m_hoverRow;

            // Selected rows win over hover; the rounded highlight is inset so
            // adjacent rows keep a gap and the bar never touches the edges.
            if (active) {
                RECT background = rowRect;
                ::InflateRect(&background, -theme::Scale(4, m_dpi), -theme::Scale(1, m_dpi));
                theme::FillRound(dc, background, theme::Scale(5, m_dpi), colors.selection);
            } else if (hovered) {
                RECT background = rowRect;
                ::InflateRect(&background, -theme::Scale(4, m_dpi), -theme::Scale(1, m_dpi));
                theme::FillRound(dc, background, theme::Scale(5, m_dpi), colors.hover);
            }

            const VisibleRow& row = m_rows[rowIndex];
            const int textTop = top;
            const int textBottom = top + rowHeight;

            if (m_tab == Tab::Contents) {
                // Indentation grows with the tree depth; the toggler column is
                // reserved even for leaf rows so all titles stay aligned.
                const int indent = theme::Scale(kBaseIndentDip, m_dpi) +
                                   row.depth * theme::Scale(kIndentStepDip, m_dpi);
                if (row.hasChildren) {
                    // Centre the triangle inside the toggler column.
                    DrawToggler(dc, indent + theme::Scale(kTogglerWidthDip, m_dpi) / 2,
                                (textTop + textBottom) / 2, row.expanded,
                                active ? colors.accent : colors.textMuted, m_dpi);
                }
                RECT textRect{indent + theme::Scale(kTogglerWidthDip, m_dpi), textTop,
                              rightEdge - theme::Scale(10, m_dpi), textBottom};
                const std::wstring& title = m_outline[static_cast<size_t>(row.index)].title;
                // Ellipsis keeps deep/long titles readable without wrapping, and
                // the active row repeats the accent colour of its highlight.
                theme::DrawStringEllipsis(dc, title, textRect,
                                          active ? colors.accent : colors.text, m_rowFont,
                                          DT_LEFT);
            } else {
                // Bookmark rows: star marker, then the title; the page number is
                // right aligned in a reserved column when the title is custom.
                const int starX = theme::Scale(kBaseIndentDip + 4, m_dpi);
                const int starRadius = theme::Scale(5, m_dpi);
                DrawStar(dc, starX, (textTop + textBottom) / 2, starRadius,
                         active ? colors.accent : colors.textMuted);

                const Bookmark& bookmark = m_bookmarks[static_cast<size_t>(row.index)];
                const std::wstring pageLabel =
                    Format(Tr(Str::BookmarkPageLabel), bookmark.page + 1);
                // Bookmarks created from the outline carry a title; bookmarks
                // without one show the localised page label as their title.
                const bool hasCustomTitle = !bookmark.title.empty();
                const std::wstring title = hasCustomTitle ? bookmark.title : pageLabel;

                // A custom title needs room for the separate page number on the
                // right; the default "Page N" title does not.
                RECT textRect{starX + theme::Scale(12, m_dpi), textTop,
                              rightEdge - theme::Scale(hasCustomTitle ? 56 : 10, m_dpi),
                              textBottom};
                theme::DrawStringEllipsis(dc, title, textRect,
                                          active ? colors.accent : colors.text, m_rowFont,
                                          DT_LEFT);

                // Avoid repeating "Page N" when the bookmark has no custom title.
                if (hasCustomTitle) {
                    RECT pageRect{rightEdge - theme::Scale(54, m_dpi), textTop,
                                  rightEdge - theme::Scale(10, m_dpi), textBottom};
                    theme::DrawString(dc, pageLabel, pageRect, colors.textMuted,
                                      m_smallFont, DT_RIGHT);
                }
            }
        }
    }

    // The bar is windowless, so the sidebar feeds it the current viewport and
    // offset every paint.  It never tracks per-bar hover, but an active drag is
    // still rendered with the darker thumb by ScrollBar::Paint itself.
    m_scrollBar.Update(ScrollOrientation::Vertical, content.bottom - content.top,
                       m_contentHeight, m_scrollOffset);
    m_scrollBar.Paint(dc, content, false);
}

void Sidebar::PaintEmptyState(HDC dc, const RECT& content) {
    const theme::Palette& colors = theme::Colors();
    const bool contentsTab = m_tab == Tab::Contents;

    // Start well below the header so the message does not look like a row, and
    // keep a generous side margin for wrapped languages.
    RECT textRect = content;
    textRect.left += theme::Scale(20, m_dpi);
    textRect.right -= theme::Scale(20, m_dpi);
    textRect.top = content.top + theme::Scale(56, m_dpi);

    theme::DrawStringBlock(dc, Tr(contentsTab ? Str::NoOutline : Str::NoBookmarks),
                           textRect, colors.textMuted, m_rowFont,
                           DT_CENTER | DT_WORDBREAK | DT_TOP);
    if (!contentsTab) {
        // Bookmarks get a second, lighter line explaining how to create one.
        RECT hintRect = textRect;
        hintRect.top += theme::Scale(34, m_dpi);
        theme::DrawStringBlock(dc, Tr(Str::BookmarkHint), hintRect, colors.textDisabled,
                               m_smallFont, DT_CENTER | DT_WORDBREAK | DT_TOP);
    }
}

int Sidebar::RowAtPoint(POINT point) const {
    if (m_hwnd == nullptr) {
        return -1;
    }
    RECT client = {};
    ::GetClientRect(m_hwnd, &client);
    const RECT content = ContentRect(client);
    // The header and the right hand scrollbar strip are not row areas; the
    // scrollbar handles its own hit testing before rows are consulted.
    if (point.y < content.top || point.y >= content.bottom || point.x < content.left ||
        point.x >= content.right) {
        return -1;
    }
    // Convert the client y into list coordinates by adding the scroll offset,
    // then divide by the row height.  The division rounds towards zero, which is
    // correct here because contentY is guaranteed non-negative.
    const int contentY = point.y - content.top + m_scrollOffset;
    const int row = contentY / RowHeight();
    if (row < 0 || row >= static_cast<int>(m_rows.size())) {
        return -1;
    }
    return row;
}

bool Sidebar::IsTogglerHit(const VisibleRow& row, POINT point) const {
    if (!row.hasChildren) {
        return false;
    }
    // Only the horizontal position matters: the caller already knows the point
    // is inside this row.  The hit column is wider than the drawn triangle so
    // the small shape is still easy to hit.
    const int indent = theme::Scale(kBaseIndentDip, m_dpi) +
                       row.depth * theme::Scale(kIndentStepDip, m_dpi);
    const int left = indent;
    const int right = indent + theme::Scale(kTogglerWidthDip, m_dpi);
    return point.x >= left && point.x < right;
}

void Sidebar::ToggleRow(int rowIndex) {
    // Toggling only makes sense on the tree tab; bookmark rows have no children.
    if (m_tab != Tab::Contents || rowIndex < 0 ||
        rowIndex >= static_cast<int>(m_rows.size())) {
        return;
    }
    const int outlineIndex = m_rows[static_cast<size_t>(rowIndex)].index;
    if (outlineIndex < 0 || outlineIndex >= static_cast<int>(m_expanded.size())) {
        return;
    }
    // m_expanded is indexed by outline entry, unlike rowIndex which addresses
    // the flattened list, so the two are mapped through VisibleRow::index.
    m_expanded[static_cast<size_t>(outlineIndex)] =
        !m_expanded[static_cast<size_t>(outlineIndex)];
    // The flattened list changes shape, so the row index that triggered this
    // call is no longer meaningful after the rebuild.
    RebuildRows();
    if (m_hwnd != nullptr) {
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void Sidebar::ActivateRow(int rowIndex) {
    if (rowIndex < 0 || rowIndex >= static_cast<int>(m_rows.size())) {
        return;
    }
    const int page = m_rows[static_cast<size_t>(rowIndex)].page;
    if (page >= 0) {
        // Posted instead of sent: the frame may re-enter the sidebar (for
        // example to update the active row) while handling the navigation.
        ::PostMessageW(::GetParent(m_hwnd), WM_APP_NAVIGATE_TO_PAGE,
                       static_cast<WPARAM>(page), 0);
    }
}

void Sidebar::SetHoveredRow(int row) {
    // Mouse moves are frequent; repaint only when the hovered row changes.
    if (m_hoverRow == row) {
        return;
    }
    m_hoverRow = row;
    if (m_hwnd != nullptr) {
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void Sidebar::ScrollBy(int deltaPixels) {
    if (deltaPixels == 0) {
        return;
    }
    // Positive deltas scroll towards the end of the list (content moves up).
    // Clamping here keeps every caller (wheel, keyboard, scrollbar paging) safe.
    m_scrollOffset = std::clamp(m_scrollOffset + deltaPixels, 0, MaxScroll());
    if (m_hwnd != nullptr) {
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void Sidebar::ShowBookmarkContextMenu(int bookmarkIndex) {
    if (bookmarkIndex < 0 || bookmarkIndex >= static_cast<int>(m_bookmarks.size())) {
        return;
    }
    HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }
    // A single command; the menu is created on demand because its label depends
    // on the current interface language.
    ::AppendMenuW(menu, MF_STRING, 1, Tr(Str::DeleteBookmark));

    POINT cursor = {};
    ::GetCursorPos(&cursor);
    // TPM_RETURNCMD makes TrackPopupMenu return the chosen id instead of
    // posting WM_COMMAND, so the temporary menu can be destroyed immediately.
    const int result = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, cursor.x,
                                        cursor.y, 0, m_hwnd, nullptr);
    ::DestroyMenu(menu);

    if (result == 1) {
        // The frame removes the bookmark and refreshes this sidebar; the index
        // refers to m_bookmarks, not to the flattened row list.
        ::PostMessageW(::GetParent(m_hwnd), WM_APP_DELETE_BOOKMARK,
                       static_cast<WPARAM>(bookmarkIndex), 0);
    }
}

LRESULT CALLBACK Sidebar::WindowProc(HWND window, UINT message, WPARAM wParam,
                                     LPARAM lParam) {
    Sidebar* self =
        reinterpret_cast<Sidebar*>(::GetWindowLongPtrW(window, GWLP_USERDATA));

    if (message == WM_CREATE) {
        // Bind the C++ object to the HWND; WM_CREATE is the only message that
        // delivers CREATESTRUCT::lpCreateParams.
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<Sidebar*>(create->lpCreateParams);
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
            // The double-buffered paint covers the whole client area.
            return 1;

        case WM_SIZE:
            // A shorter window may leave the existing offset past the end.
            self->ClampScroll();
            ::InvalidateRect(window, nullptr, FALSE);
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT paint = {};
            HDC dc = ::BeginPaint(window, &paint);
            RECT client = {};
            ::GetClientRect(window, &client);
            if (client.right > 0 && client.bottom > 0) {
                // Paint the whole pane off screen and blit once to avoid the
                // flicker of many individual rounded rectangles and text runs.
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
            // One-shot leave tracking, re-armed after every WM_MOUSELEAVE.
            if (!self->m_trackingMouse) {
                TRACKMOUSEEVENT track = {};
                track.cbSize = sizeof(track);
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = window;
                ::TrackMouseEvent(&track);
                self->m_trackingMouse = true;
            }

            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            // While the thumb is being dragged the pointer may leave the strip;
            // DragTo clamps the position, and only the delta is applied so the
            // sidebar does not jump when the drag started mid-thumb.
            if (self->m_scrollBar.IsDragging()) {
                RECT client = {};
                ::GetClientRect(window, &client);
                int newOffset = 0;
                if (self->m_scrollBar.DragTo(self->ContentRect(client), point, newOffset)) {
                    self->ScrollBy(newOffset - self->m_scrollOffset);
                }
                return 0;
            }

            self->SetHoveredRow(self->RowAtPoint(point));
            return 0;
        }

        case WM_MOUSELEAVE:
            self->m_trackingMouse = false;
            self->SetHoveredRow(-1);
            return 0;

        case WM_LBUTTONDOWN: {
            // Clicking anywhere in the pane gives it the keyboard focus so the
            // arrow/page keys below can scroll it.
            ::SetFocus(window);
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

            // Header tabs.  The y check uses the tab height; the x range is the
            // tab column computed by the same helper used for painting.
            for (int index = 0; index < 2; ++index) {
                const RECT tab = TabRectFor(index, self->m_dpi);
                if (point.x >= tab.left && point.x < tab.right && point.y < tab.bottom) {
                    if (static_cast<int>(self->m_tab) != index) {
                        // Switching tabs rebuilds the flattened rows; reset the
                        // position because the two lists have no relation.
                        self->m_tab = static_cast<Tab>(index);
                        self->m_scrollOffset = 0;
                        self->m_hoverRow = -1;
                        self->RebuildRows();
                        ::InvalidateRect(window, nullptr, FALSE);
                    }
                    return 0;
                }
            }

            RECT client = {};
            ::GetClientRect(window, &client);
            const RECT content = self->ContentRect(client);

            // Scrollbar interaction.  A press on the thumb starts a drag; a
            // press elsewhere on the track pages by 80% of the viewport in the
            // direction of the click.
            if (self->m_scrollBar.IsVisible() &&
                self->m_scrollBar.HitTestTrack(content, point)) {
                if (!self->m_scrollBar.BeginDrag(content, point)) {
                    const int pageHeight =
                        std::max(0, static_cast<int>(content.bottom - content.top));
                    // Compare with the middle of the thumb: above it pages up,
                    // below it pages down.
                    const bool belowThumb = point.y >
                        (self->m_scrollBar.ThumbRect(content).top +
                         self->m_scrollBar.ThumbRect(content).bottom) / 2;
                    self->ScrollBy(belowThumb ? pageHeight * 8 / 10 : -pageHeight * 8 / 10);
                }
                // Capture even for a track click so that leaving the window
                // while the button is down still delivers the matching up.
                ::SetCapture(window);
                return 0;
            }

            const int row = self->RowAtPoint(point);
            if (row >= 0) {
                const VisibleRow& visibleRow = self->m_rows[static_cast<size_t>(row)];
                // The expander column toggles the node; the rest of the row
                // navigates to its destination page.
                if (self->IsTogglerHit(visibleRow, point)) {
                    self->ToggleRow(row);
                } else {
                    self->ActivateRow(row);
                }
            }
            return 0;
        }

        case WM_LBUTTONUP:
            if (::GetCapture() == window) {
                ::ReleaseCapture();
            }
            self->m_scrollBar.EndDrag();
            return 0;

        case WM_CAPTURECHANGED:
            // Another window stole the capture; end the drag state so a later
            // WM_MOUSEMOVE does not keep scrolling.
            self->m_scrollBar.EndDrag();
            return 0;

        case WM_RBUTTONUP: {
            // Only bookmark rows have a context menu.
            if (self->m_tab != Tab::Bookmarks) {
                return 0;
            }
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const int row = self->RowAtPoint(point);
            if (row >= 0) {
                self->ShowBookmarkContextMenu(self->m_rows[static_cast<size_t>(row)].index);
            }
            return 0;
        }

        case WM_MOUSEWHEEL: {
            // GET_WHEEL_DELTA_WPARAM is normally a multiple of WHEEL_DELTA per
            // notch, but precision touchpads send smaller deltas.  Accumulate
            // them so a slow swipe eventually scrolls instead of being lost.
            self->m_wheelRemainder += GET_WHEEL_DELTA_WPARAM(wParam);
            const int notches = self->m_wheelRemainder / WHEEL_DELTA;
            self->m_wheelRemainder -= notches * WHEEL_DELTA;
            if (notches != 0) {
                self->ScrollBy(-notches * self->RowHeight() * 3);
            }
            return 0;
        }

        case WM_KEYDOWN: {
            // With an empty list there is nothing to scroll; consume the keys.
            if (self->m_rows.empty()) {
                return 0;
            }
            switch (wParam) {
                case VK_UP:
                    // Up only ever reduces the offset; no clamp to MaxScroll is
                    // needed, but the lower bound must stay at zero.
                    self->m_scrollOffset = std::max(0, self->m_scrollOffset - self->RowHeight());
                    ::InvalidateRect(window, nullptr, FALSE);
                    return 0;
                case VK_DOWN:
                    self->ScrollBy(self->RowHeight());
                    return 0;
                case VK_PRIOR:
                    self->ScrollBy(-self->RowHeight() * 8);
                    return 0;
                case VK_NEXT:
                    self->ScrollBy(self->RowHeight() * 8);
                    return 0;
                default:
                    break;
            }
            return 0;
        }

        case WM_SETCURSOR: {
            // Only the tabs, rows and scrollbar track show a hand; the rest of
            // the pane keeps the ordinary arrow via DefWindowProc.
            POINT point = {};
            ::GetCursorPos(&point);
            ::ScreenToClient(window, &point);
            bool hand = false;
            for (int index = 0; index < 2; ++index) {
                const RECT tab = TabRectFor(index, self->m_dpi);
                if (point.x >= tab.left && point.x < tab.right && point.y < tab.bottom) {
                    hand = true;
                }
            }
            if (self->RowAtPoint(point) >= 0) {
                hand = true;
            }
            RECT client = {};
            ::GetClientRect(window, &client);
            if (self->m_scrollBar.HitTestTrack(self->ContentRect(client), point)) {
                hand = true;
            }
            if (hand) {
                ::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        }

        case WM_DESTROY:
            // The Sidebar object outlives its HWND, so release the GDI fonts
            // here; no further paint can use them.
            theme::ReleaseFont(self->m_rowFont);
            theme::ReleaseFont(self->m_headerFont);
            theme::ReleaseFont(self->m_smallFont);
            self->m_hwnd = nullptr;
            return 0;

        default:
            break;
    }
    return ::DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace pengpdf
