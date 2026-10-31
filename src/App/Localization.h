// =============================================================================
//  Localization.h
//  Bilingual user interface (English / Simplified Chinese).
//
//  Every visible string goes through Tr().  Switching languages only requires
//  calling SetLanguage() and re-applying the strings to the already created
//  windows, because both languages are compiled into the executable.
// =============================================================================
#pragma once

namespace pengpdf {

// Only two interface languages are compiled in.  Chinese is always presented
// as Simplified Chinese, even on Traditional Chinese installations.
enum class Lang {
    English,
    Chinese,
};

// Identifiers for every localised string of the application.
//
// The numeric order is irrelevant, but the identifier alone is the lookup key:
// every value here must have exactly one row in kStringTable (Localization.cpp)
// or Tr() will return an empty string.
enum class Str {
    // Application shell ------------------------------------------------------
    AppTitle,
    MenuFile,
    MenuOpen,
    MenuRecent,
    MenuNoRecent,
    MenuClose,
    MenuExit,
    MenuEdit,
    MenuFind,
    MenuCopy,
    MenuView,
    MenuZoomIn,
    MenuZoomOut,
    MenuFitWidth,
    MenuFitPage,
    MenuActualSize,
    MenuShowSidebar,
    MenuGo,
    MenuFirstPage,
    MenuPreviousPage,
    MenuNextPage,
    MenuLastPage,
    MenuGoToPage,
    MenuBookmarks,
    MenuAddBookmark,
    MenuRemoveBookmark,
    MenuSettings,
    MenuPreferences,
    MenuLanguage,
    MenuLanguageEnglish,
    MenuLanguageChinese,
    MenuHelp,
    MenuAbout,
    OpenDialogTitle,
    FilterPdfFiles,
    FilterAllFiles,

    // Toolbar hints ----------------------------------------------------------
    HintOpen,
    HintSidebar,
    HintPreviousPage,
    HintNextPage,
    HintZoomOut,
    HintZoomIn,
    HintFitWidth,
    HintFitPage,
    HintBookmark,
    HintSettings,
    HintGoToPage,

    // Sidebar ----------------------------------------------------------------
    TabContents,
    TabBookmarks,
    NoOutline,
    NoBookmarks,
    BookmarkHint,
    BookmarkPageLabel,
    DeleteBookmark,

    // Empty document state ---------------------------------------------------
    EmptyTitle,
    EmptyHint,

    // Status bar -------------------------------------------------------------
    StatusReady,
    StatusNoDocument,
    StatusLoading,
    StatusPageOf,
    StatusOutlineCount,

    // Zoom -------------------------------------------------------------------
    ZoomPercent,
    ZoomFitWidthShort,
    ZoomFitPageShort,
    ZoomActualSizeShort,

    // Settings dialog --------------------------------------------------------
    SettingsTitle,
    SettingsLanguage,
    SettingsLanguageHint,
    SettingsDefaultZoom,
    SettingsShowSidebar,
    SettingsOk,
    SettingsCancel,

    // Search -----------------------------------------------------------------
    SearchPlaceholder,
    SearchSearching,
    SearchNoResults,
    SearchResultCount,

    // Go to page dialog ------------------------------------------------------
    GoToPageTitle,
    GoToPagePrompt,
    GoToPageInvalid,

    // Errors -----------------------------------------------------------------
    ErrorOpenTitle,
    ErrorOpenMessage,
    ErrorNoPages,
    ErrorFileMissing,
    ErrorPassword,
    ErrorInvalidPdf,
    ErrorAccessDenied,
    ErrorEngineFailure,

    // About ------------------------------------------------------------------
    AboutTitle,
    AboutVersion,
    AboutDescription,
    AboutEngine,
    AboutAuthor,
    AboutClose,
};

// Current language ------------------------------------------------------------
// Picks Chinese for every Chinese UI language (Simplified, Traditional, Hong
// Kong) and English for everything else.
Lang DetectSystemLanguage();

// The language currently in effect.  English until SetLanguage() changes it or
// MainWindow applies the saved language tag during startup.
Lang GetLanguage();

// Switches the active table.  Existing windows keep their old captions until
// the caller re-applies the strings; see MainWindow::ApplyLanguage().
void SetLanguage(Lang language);

bool IsChineseInterface();

// Returns the localised text for `identifier`; never null.
//
// The pointer refers to a string literal owned by the table, so it stays valid
// for the lifetime of the process - including after SetLanguage().  A pointer
// obtained before the switch simply keeps the old language's text.
const wchar_t* Tr(Str identifier);

}  // namespace pengpdf
