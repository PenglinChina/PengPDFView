// =============================================================================
//  Commands.h
//  Command identifiers shared by the menu, the toolbar and the main window.
// =============================================================================
#pragma once

namespace pengpdf {
namespace command {

// Values are explicit because they form the contract between the WM_COMMAND
// handlers, the menu and the accelerator table: they must never be renumbered
// or reused.  The blocks of 100 leave room for new commands inside a group
// without shifting the ones behind it.
enum : int {
    // File -------------------------------------------------------------------
    FileOpen = 1001,
    FileClose,
    FileExit,
    FileClearRecent,

    // View -------------------------------------------------------------------
    ViewZoomIn = 1101,
    ViewZoomOut,
    ViewFitWidth,
    ViewFitPage,
    ViewActualSize,
    ViewToggleSidebar,

    // Navigation -------------------------------------------------------------
    GoFirstPage = 1201,
    GoPreviousPage,
    GoNextPage,
    GoLastPage,
    GoToPage,

    // Bookmarks --------------------------------------------------------------
    BookmarksAdd = 1301,
    BookmarksRemoveCurrent,

    // Edit -------------------------------------------------------------------
    EditFind = 1601,
    EditCopy,
    EditClearSelection,

    // Settings ---------------------------------------------------------------
    SettingsPreferences = 1401,
    SettingsLanguageEnglish,
    SettingsLanguageChinese,

    // Help -------------------------------------------------------------------
    HelpAbout = 1501,
};

// Recent file menu entries occupy this contiguous range: the i-th entry is
// kRecentFileFirst + i.  The UI recognises the whole block with a single range
// check and currently reserves room for 100 entries (MainWindow::OnCommand).
inline constexpr int kRecentFileFirst = 2000;

// Commands that the toolbar can emit, used for enabling/disabling buttons.
inline constexpr int kToolbarCommandFirst = 1000;
inline constexpr int kToolbarCommandLast = 1500;
}  // namespace command
}  // namespace pengpdf
