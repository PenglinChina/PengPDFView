// =============================================================================
//  MainWindow.h
//  Frame window: owns the menu bar, the toolbar, the sidebar, the page canvas,
//  the search bar, the render/text services and all persistent application
//  state.
//
//  The frame is the only place that knows about the document lifecycle; the
//  child controls are deliberately passive and communicate through messages.
// =============================================================================
#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

#include "App/Bookmarks.h"
#include "App/Localization.h"
#include "App/Settings.h"
#include "Pdf/PdfParser.h"
#include "Pdf/PdfTextTypes.h"
#include "Render/RenderService.h"
#include "Text/TextService.h"
#include "Ui/PageCanvas.h"
#include "Ui/SearchBar.h"
#include "Ui/Sidebar.h"
#include "Ui/Toolbar.h"

namespace pengpdf {

class MainWindow {
public:
    static bool RegisterWindowClass(HINSTANCE instance, const wchar_t* className);

    // Creates the hidden frame, restores the saved placement and optionally
    // opens `initialDocument` once the message loop is running.
    bool Create(HINSTANCE instance, const wchar_t* className, int showCommand,
                const std::wstring& initialDocument);
    HWND Handle() const { return m_hwnd; }
    // Accelerator table installed by the message loop in main.cpp.
    HACCEL Accelerators() const { return m_accelerators; }

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam,
                                       LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    // Initialisation --------------------------------------------------------
    bool OnCreate();
    void CreateAccelerators();
    void RecreateFonts();
    // Rebuilds the menu; called whenever document or selection state changes
    // because several commands are enabled conditionally.
    void BuildMenu();
    void LayoutChildren();
    void UpdateChrome();
    void UpdateStatus();
    void UpdateTitle();

    // Document lifecycle ----------------------------------------------------
    void OpenDocument(const std::wstring& path);
    void CloseDocument();
    // Message handlers for the worker services.  They take ownership of the
    // heap allocated payload.
    void OnDocumentReady(DocumentReadyMessage* message);
    void OnPageReady(PageReadyMessage* message);
    void OnTextReady(TextReadyMessage* message);
    // Frees payloads of worker messages that are still queued when the window
    // is destroyed.  Called after both services have joined their threads.
    void DrainWorkerMessages();

    // Search ----------------------------------------------------------------
    // Shows/focuses the floating bar and prefills it with the current single
    // line selection when there is one.
    void ShowSearchBar();
    void HideSearchBar();
    // Starts a new query through TextService and resets the canvas highlights.
    void RestartSearch();
    void UpdateSearchStatus();
    void SetCurrentSearchMatch(int index);
    void StepSearchMatch(int delta);
    void OnSearchPage(SearchPageMessage* message);
    void OnSearchDone(SearchDoneMessage* message);
    // Copies the current page selection to the clipboard as CF_UNICODETEXT.
    void CopySelection();

    // Commands and dialogs --------------------------------------------------
    void OnCommand(int command);
    void ShowOpenDialog();
    void ShowSettingsDialog();
    void ShowGoToPageDialog();
    void ShowAboutDialog();

    // Navigation and bookmarks ---------------------------------------------
    void GoToPage(int pageIndex);
    void StepPage(int delta);
    void ToggleBookmarkForCurrentPage();
    void RemoveBookmarkForCurrentPage();
    void RemoveBookmarkAt(int index);
    void RefreshBookmarkViews();
    bool HasBookmarkForCurrentPage() const;
    std::wstring BookmarkTitleForCurrentPage() const;

    // Shell integration -----------------------------------------------------
    void ApplyLanguage(Lang language);
    void ToggleSidebar();
    void OnDpiChanged(UINT dpi, const RECT* suggestedRect);
    void OnDropFiles(HDROP drop);
    void SaveSettings(bool includePlacement);
    void PaintFrame(HDC dc, const RECT& client);

    // Layout helpers --------------------------------------------------------
    int StatusBarHeight() const;
    RECT SplitterRect() const;
    RECT StatusBarRect() const;
    std::wstring WindowTitleText() const;

    HWND m_hwnd = nullptr;
    HINSTANCE m_instance = nullptr;
    UINT m_dpi = 96;
    HACCEL m_accelerators = nullptr;

    Toolbar m_toolbar;
    Sidebar m_sidebar;
    PageCanvas m_canvas;
    SearchBar m_searchBar;

    std::unique_ptr<RenderService> m_renderer;
    // Shared with the text worker; the parser guards its object cache.
    std::shared_ptr<pdf::Document> m_document;
    std::unique_ptr<TextService> m_textService;
    BookmarkStore m_bookmarks;
    SettingsStore m_settings;

    std::wstring m_documentPath;
    std::wstring m_documentTitle;
    std::wstring m_hintText;

    uint64_t m_documentToken = 0;
    bool m_documentOpen = false;
    int m_pageCount = 0;
    int m_currentPage = -1;

    // Search state owned by the frame; the canvas only paints the highlights.
    // The index in m_currentSearchMatch refers to the appended match list.
    uint64_t m_searchId = 0;
    std::vector<pdf::TextMatch> m_searchMatches;
    int m_currentSearchMatch = -1;
    bool m_searchPending = false;
    UINT_PTR m_searchTimer = 0;

    int m_sidebarWidthPx = 0;
    bool m_sidebarVisible = true;

    bool m_splitterDragging = false;
    bool m_splitterHover = false;
    bool m_trackingMouse = false;

    HFONT m_statusFont = nullptr;
    HFONT m_statusBoldFont = nullptr;
};

}  // namespace pengpdf
