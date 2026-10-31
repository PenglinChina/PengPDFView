// =============================================================================
//  UiMessages.h
//  Custom window messages exchanged between the custom controls and the frame.
//
//  The identifiers are allocated in the private WM_APP range so they can never
//  collide with standard window or control notifications.  They are delivered
//  to the top-level frame window, which owns the application state; the payload
//  convention of each message is documented next to its declaration.
// =============================================================================
#pragma once

#include <windows.h>

namespace pengpdf {

// Status bar hint: wParam = 1 when a hint is shown, 0 to restore the default;
// lParam = static_cast<LPARAM>(Str) identifying the localised hint text.
inline constexpr UINT WM_APP_HINT = WM_APP + 120;

// Request to scroll to a page: wParam = zero based page index.
inline constexpr UINT WM_APP_NAVIGATE_TO_PAGE = WM_APP + 121;

// Delete a bookmark: wParam = index into the bookmark list of the document.
inline constexpr UINT WM_APP_DELETE_BOOKMARK = WM_APP + 122;

// The sidebar width changed because the user dragged the splitter:
// wParam = new width in physical pixels.
inline constexpr UINT WM_APP_SIDEBAR_RESIZED = WM_APP + 123;

// The page canvas scrolled to another page: wParam = zero based page index.
inline constexpr UINT WM_APP_CANVAS_PAGE_CHANGED = WM_APP + 124;

// The page canvas zoom changed (mode or factor).  No payload: the frame queries
// the canvas for the new mode and percentage before refreshing the toolbar.
inline constexpr UINT WM_APP_CANVAS_ZOOM_CHANGED = WM_APP + 125;

// Search bar notifications sent to its parent window.  The parent reads the
// query through SearchBar::Query(), so no payload is needed.
inline constexpr UINT WM_APP_SEARCH_TEXT_CHANGED = WM_APP + 150;
inline constexpr UINT WM_APP_SEARCH_NEXT = WM_APP + 151;
inline constexpr UINT WM_APP_SEARCH_PREVIOUS = WM_APP + 152;
inline constexpr UINT WM_APP_SEARCH_CLOSE = WM_APP + 153;

// The canvas text selection changed (used to refresh the Copy menu state).
inline constexpr UINT WM_APP_SELECTION_CHANGED = WM_APP + 154;

}  // namespace pengpdf
