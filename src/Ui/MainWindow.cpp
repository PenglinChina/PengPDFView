// =============================================================================
//  MainWindow.cpp
//  Frame window and application wiring.
// =============================================================================
#include "Ui/MainWindow.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <utility>

#include "App/Commands.h"
#include "Core/Config.h"
#include "Core/FileUtil.h"
#include "Core/Log.h"
#include "Core/Utf.h"
#include "Text/TextSearch.h"
#include "Ui/Dialogs.h"
#include "Ui/Theme.h"
#include "resource.h"

namespace pengpdf {
namespace {

// Delays the actual search until the user pauses typing.
constexpr UINT_PTR kSearchDebounceTimerId = 2;

// Small formatting helper for the status bar's page count.
std::wstring FormatSizeText(int pageCount) {
    return Format(L"%d", pageCount);
}

}  // namespace

// Registers the frame class.  The background is painted manually so the
// class does not request a system brush.
bool MainWindow::RegisterWindowClass(HINSTANCE instance, const wchar_t* className) {
    WNDCLASSEXW description = {};
    description.cbSize = sizeof(description);
    description.style = CS_HREDRAW | CS_VREDRAW;
    description.lpfnWndProc = &MainWindow::WindowProc;
    description.hInstance = instance;
    description.hIcon = ::LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    description.hIconSm = ::LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    description.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    description.lpszClassName = className;
    return ::RegisterClassExW(&description) != 0;
}

bool MainWindow::Create(HINSTANCE instance, const wchar_t* className, int showCommand,
                        const std::wstring& initialDocument) {
    m_instance = instance;
    m_settings.Load();
    m_bookmarks.Load();

    // The language must be known before the first window is created so that
    // menus and the empty page are already localised on the first paint.
    Lang language = DetectSystemLanguage();
    const std::wstring& tag = m_settings.Values().languageTag;
    if (tag == L"zh") {
        language = Lang::Chinese;
    } else if (tag == L"en") {
        language = Lang::English;
    }
    SetLanguage(language);

    const WindowPlacement& placement = m_settings.Values().window;
    const int width = placement.width > 0 ? placement.width : 1240;
    const int height = placement.height > 0 ? placement.height : 900;

    m_hwnd = ::CreateWindowExW(
        0, className, kAppDisplayName, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        placement.x >= 0 ? placement.x : CW_USEDEFAULT,
        placement.y >= 0 ? placement.y : CW_USEDEFAULT, width, height, nullptr, nullptr,
        instance, this);
    if (m_hwnd == nullptr) {
        LogError(L"Failed to create the main window");
        return false;
    }

    ::ShowWindow(m_hwnd, placement.maximized ? SW_SHOWMAXIMIZED : showCommand);
    ::UpdateWindow(m_hwnd);

    if (!initialDocument.empty()) {
        OpenDocument(initialDocument);
    }
    return true;
}

// Creates the child controls and the two worker services, restores the saved
// settings and builds the first menu.  The document itself opens later.
bool MainWindow::OnCreate() {
    m_dpi = theme::DpiOf(m_hwnd);
    ::DragAcceptFiles(m_hwnd, TRUE);

    if (!m_toolbar.Create(m_hwnd, m_instance, kToolbarClass, m_dpi)) {
        return false;
    }
    if (!m_sidebar.Create(m_hwnd, m_instance, kSidebarClass, m_dpi)) {
        return false;
    }
    if (!m_canvas.Create(m_hwnd, m_instance, kPageCanvasClass, m_dpi)) {
        return false;
    }

    const AppSettings& settings = m_settings.Values();
    m_sidebarVisible = settings.sidebarVisible;
    m_sidebarWidthPx = theme::Scale(
        std::clamp(settings.sidebarWidth, kSidebarMinWidthDip, kSidebarMaxWidthDip), m_dpi);

    m_canvas.SetZoomMode(settings.zoomMode);
    if (settings.zoomMode == ZoomMode::Custom) {
        m_canvas.SetZoom(settings.zoom);
    }

    m_renderer = std::make_unique<RenderService>(m_hwnd);
    m_canvas.SetRenderService(m_renderer.get(), m_documentToken);

    m_textService = std::make_unique<TextService>(m_hwnd);
    m_canvas.SetTextService(m_textService.get(), m_documentToken);

    if (!m_searchBar.Create(m_hwnd, m_instance, kSearchBarClass, m_dpi)) {
        return false;
    }
    ::ShowWindow(m_searchBar.Handle(), SW_HIDE);

    RecreateFonts();
    BuildMenu();
    CreateAccelerators();
    LayoutChildren();
    UpdateChrome();
    UpdateStatus();
    return true;
}

// Status fonts follow the interface language; the Chinese UI font has
// different metrics, so the height calculations use the created font.
void MainWindow::RecreateFonts() {
    theme::ReleaseFont(m_statusFont);
    theme::ReleaseFont(m_statusBoldFont);
    const bool chinese = IsChineseInterface();
    m_statusFont = theme::CreateUiFont(m_dpi, 8.5, FW_NORMAL, chinese);
    m_statusBoldFont = theme::CreateUiFont(m_dpi, 8.5, FW_SEMIBOLD, chinese);
}

// Static accelerator table.  Accelerators are translated by the message loop
// in main.cpp; while a modal dialog is open it owns the message loop and its
// own key handling, so these shortcuts cannot fire behind it.
void MainWindow::CreateAccelerators() {
    ACCEL table[] = {
        {FVIRTKEY | FCONTROL, 'O', command::FileOpen},
        {FVIRTKEY | FCONTROL, 'W', command::FileClose},
        {FVIRTKEY | FCONTROL, '=', command::ViewZoomIn},
        {FVIRTKEY | FCONTROL, VK_OEM_PLUS, command::ViewZoomIn},
        {FVIRTKEY | FCONTROL, VK_ADD, command::ViewZoomIn},
        {FVIRTKEY | FCONTROL, '-', command::ViewZoomOut},
        {FVIRTKEY | FCONTROL, VK_OEM_MINUS, command::ViewZoomOut},
        {FVIRTKEY | FCONTROL, VK_SUBTRACT, command::ViewZoomOut},
        {FVIRTKEY | FCONTROL, '1', command::ViewFitWidth},
        {FVIRTKEY | FCONTROL, '2', command::ViewFitPage},
        {FVIRTKEY | FCONTROL, '0', command::ViewActualSize},
        {FVIRTKEY | FCONTROL, 'G', command::GoToPage},
        {FVIRTKEY | FCONTROL, 'D', command::BookmarksAdd},
        {FVIRTKEY | FCONTROL | FSHIFT, 'D', command::BookmarksRemoveCurrent},
        {FVIRTKEY | FCONTROL, VK_OEM_COMMA, command::SettingsPreferences},
        {FVIRTKEY | FCONTROL, 'F', command::EditFind},
        {FVIRTKEY | FCONTROL, 'C', command::EditCopy},
        {FVIRTKEY, VK_ESCAPE, command::EditClearSelection},
        {FVIRTKEY, VK_F4, command::ViewToggleSidebar},
        {FVIRTKEY, VK_NEXT, command::GoNextPage},
        {FVIRTKEY, VK_PRIOR, command::GoPreviousPage},
        {FVIRTKEY, VK_HOME, command::GoFirstPage},
        {FVIRTKEY, VK_END, command::GoLastPage},
    };
    m_accelerators = ::CreateAcceleratorTableW(table, static_cast<int>(std::size(table)));
}

// Rebuilds the menu from scratch.  Enable/check states depend on the document,
// the current page, the bookmark store, the selection and the language, so a
// rebuild is simpler than tracking each item separately.
void MainWindow::BuildMenu() {
    if (m_hwnd == nullptr) {
        return;
    }

    const HMENU menu = ::CreateMenu();

    // File ------------------------------------------------------------------
    const HMENU fileMenu = ::CreatePopupMenu();
    ::AppendMenuW(fileMenu, MF_STRING, command::FileOpen, Tr(Str::MenuOpen));

    const HMENU recentMenu = ::CreatePopupMenu();
    const auto& recentFiles = m_settings.Values().recentFiles;
    bool hasRecent = false;
    for (size_t index = 0; index < recentFiles.size(); ++index) {
        if (!FileExists(recentFiles[index])) {
            continue;
        }
        const std::wstring label = GetFileName(recentFiles[index]);
        ::AppendMenuW(recentMenu, MF_STRING,
                      command::kRecentFileFirst + static_cast<int>(index), label.c_str());
        hasRecent = true;
    }
    if (!hasRecent) {
        ::AppendMenuW(recentMenu, MF_STRING | MF_GRAYED, 0, Tr(Str::MenuNoRecent));
    }
    ::AppendMenuW(fileMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(recentMenu),
                  Tr(Str::MenuRecent));
    ::AppendMenuW(fileMenu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(fileMenu, MF_STRING, command::FileClose, Tr(Str::MenuClose));
    ::AppendMenuW(fileMenu, MF_STRING, command::FileExit, Tr(Str::MenuExit));
    ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(fileMenu), Tr(Str::MenuFile));

    // Edit ------------------------------------------------------------------
    const HMENU editMenu = ::CreatePopupMenu();
    ::AppendMenuW(editMenu, MF_STRING, command::EditFind, Tr(Str::MenuFind));
    ::AppendMenuW(editMenu, MF_STRING, command::EditCopy, Tr(Str::MenuCopy));
    ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(editMenu), Tr(Str::MenuEdit));

    // View ------------------------------------------------------------------
    const HMENU viewMenu = ::CreatePopupMenu();
    ::AppendMenuW(viewMenu, MF_STRING, command::ViewZoomIn, Tr(Str::MenuZoomIn));
    ::AppendMenuW(viewMenu, MF_STRING, command::ViewZoomOut, Tr(Str::MenuZoomOut));
    ::AppendMenuW(viewMenu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(viewMenu, MF_STRING, command::ViewFitWidth, Tr(Str::MenuFitWidth));
    ::AppendMenuW(viewMenu, MF_STRING, command::ViewFitPage, Tr(Str::MenuFitPage));
    ::AppendMenuW(viewMenu, MF_STRING, command::ViewActualSize, Tr(Str::MenuActualSize));
    ::AppendMenuW(viewMenu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(viewMenu, MF_STRING, command::ViewToggleSidebar,
                  Tr(Str::MenuShowSidebar));
    ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(viewMenu), Tr(Str::MenuView));

    const int zoomCommand = m_canvas.GetZoomMode() == ZoomMode::FitWidth
                                ? command::ViewFitWidth
                                : (m_canvas.GetZoomMode() == ZoomMode::FitPage
                                       ? command::ViewFitPage
                                       : command::ViewActualSize);
    ::CheckMenuRadioItem(viewMenu, command::ViewFitWidth, command::ViewActualSize,
                         zoomCommand, MF_BYCOMMAND);
    ::CheckMenuItem(viewMenu, command::ViewToggleSidebar,
                    MF_BYCOMMAND | (m_sidebarVisible ? MF_CHECKED : MF_UNCHECKED));

    // Go --------------------------------------------------------------------
    const HMENU goMenu = ::CreatePopupMenu();
    ::AppendMenuW(goMenu, MF_STRING, command::GoFirstPage, Tr(Str::MenuFirstPage));
    ::AppendMenuW(goMenu, MF_STRING, command::GoPreviousPage, Tr(Str::MenuPreviousPage));
    ::AppendMenuW(goMenu, MF_STRING, command::GoNextPage, Tr(Str::MenuNextPage));
    ::AppendMenuW(goMenu, MF_STRING, command::GoLastPage, Tr(Str::MenuLastPage));
    ::AppendMenuW(goMenu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(goMenu, MF_STRING, command::GoToPage, Tr(Str::MenuGoToPage));
    ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(goMenu), Tr(Str::MenuGo));

    // Bookmarks -------------------------------------------------------------
    const HMENU bookmarkMenu = ::CreatePopupMenu();
    ::AppendMenuW(bookmarkMenu, MF_STRING, command::BookmarksAdd,
                  Tr(Str::MenuAddBookmark));
    ::AppendMenuW(bookmarkMenu, MF_STRING, command::BookmarksRemoveCurrent,
                  Tr(Str::MenuRemoveBookmark));
    ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(bookmarkMenu),
                  Tr(Str::MenuBookmarks));

    // Settings --------------------------------------------------------------
    const HMENU settingsMenu = ::CreatePopupMenu();
    ::AppendMenuW(settingsMenu, MF_STRING, command::SettingsPreferences,
                  Tr(Str::MenuPreferences));
    ::AppendMenuW(settingsMenu, MF_SEPARATOR, 0, nullptr);
    const HMENU languageMenu = ::CreatePopupMenu();
    ::AppendMenuW(languageMenu, MF_STRING, command::SettingsLanguageEnglish,
                  Tr(Str::MenuLanguageEnglish));
    ::AppendMenuW(languageMenu, MF_STRING, command::SettingsLanguageChinese,
                  Tr(Str::MenuLanguageChinese));
    ::AppendMenuW(settingsMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(languageMenu),
                  Tr(Str::MenuLanguage));
    ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(settingsMenu),
                  Tr(Str::MenuSettings));

    ::CheckMenuRadioItem(languageMenu, command::SettingsLanguageEnglish,
                         command::SettingsLanguageChinese,
                         GetLanguage() == Lang::Chinese ? command::SettingsLanguageChinese
                                                        : command::SettingsLanguageEnglish,
                         MF_BYCOMMAND);

    // Help ------------------------------------------------------------------
    const HMENU helpMenu = ::CreatePopupMenu();
    ::AppendMenuW(helpMenu, MF_STRING, command::HelpAbout, Tr(Str::MenuAbout));
    ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(helpMenu), Tr(Str::MenuHelp));

    // Enable states ---------------------------------------------------------
    const UINT documentState = m_documentOpen ? MF_ENABLED : MF_GRAYED;
    ::EnableMenuItem(fileMenu, command::FileClose, MF_BYCOMMAND | documentState);
    for (int id : {command::ViewZoomIn, command::ViewZoomOut, command::ViewFitWidth,
                   command::ViewFitPage, command::ViewActualSize, command::GoFirstPage,
                   command::GoPreviousPage, command::GoNextPage, command::GoLastPage,
                   command::GoToPage, command::BookmarksAdd}) {
        ::EnableMenuItem(menu, id, MF_BYCOMMAND | documentState);
    }
    ::EnableMenuItem(bookmarkMenu, command::BookmarksRemoveCurrent,
                     MF_BYCOMMAND | (HasBookmarkForCurrentPage() ? MF_ENABLED : MF_GRAYED));
    ::EnableMenuItem(editMenu, command::EditFind, MF_BYCOMMAND | documentState);
    ::EnableMenuItem(editMenu, command::EditCopy,
                     MF_BYCOMMAND |
                         (m_canvas.HasSelection() ? MF_ENABLED : MF_GRAYED));

    const HMENU oldMenu = ::GetMenu(m_hwnd);
    ::SetMenu(m_hwnd, menu);
    ::DrawMenuBar(m_hwnd);
    if (oldMenu != nullptr) {
        ::DestroyMenu(oldMenu);
    }
}

// Positions the toolbar, sidebar, canvas and floating search bar.  The search
// bar is a sibling of the canvas and must be raised in z-order when shown.
void MainWindow::LayoutChildren() {
    if (m_hwnd == nullptr) {
        return;
    }
    RECT client = {};
    ::GetClientRect(m_hwnd, &client);

    const int toolbarHeight = m_toolbar.Height();
    const int statusHeight = StatusBarHeight();
    const int splitterWidth = theme::Scale(kSplitterWidthDip, m_dpi);
    const int contentTop = toolbarHeight;
    const int contentBottom = std::max(contentTop, static_cast<int>(client.bottom) - statusHeight);
    const int sidebarWidth = m_sidebarVisible ? m_sidebarWidthPx : 0;

    ::MoveWindow(m_toolbar.Handle(), 0, 0, client.right, toolbarHeight, TRUE);
    ::ShowWindow(m_sidebar.Handle(), m_sidebarVisible ? SW_SHOW : SW_HIDE);
    if (m_sidebarVisible) {
        ::MoveWindow(m_sidebar.Handle(), 0, contentTop, sidebarWidth,
                     contentBottom - contentTop, TRUE);
    }
    const int canvasLeft = m_sidebarVisible ? sidebarWidth + splitterWidth : 0;
    const int canvasWidth = std::max(0, static_cast<int>(client.right) - canvasLeft);
    ::MoveWindow(m_canvas.Handle(), canvasLeft, contentTop, canvasWidth,
                 contentBottom - contentTop, TRUE);

    // The search bar floats over the top right corner of the canvas.
    if (m_searchBar.Handle() != nullptr) {
        const int barWidth = std::min(m_searchBar.PreferredWidth(),
                                      std::max(200, canvasWidth - theme::Scale(24, m_dpi)));
        const int barHeight = m_searchBar.Height();
        const int barLeft = canvasLeft + canvasWidth - barWidth - theme::Scale(12, m_dpi);
        const int barTop = contentTop + theme::Scale(10, m_dpi);
        ::MoveWindow(m_searchBar.Handle(), barLeft, barTop, barWidth, barHeight, TRUE);
    }
}

// Pushes the document/zoomed/bookmark state into the toolbar and title.
void MainWindow::UpdateChrome() {
    if (m_hwnd == nullptr) {
        return;
    }
    m_toolbar.SetDocumentState(m_documentOpen);
    m_toolbar.SetPageState(m_currentPage, m_pageCount);
    m_toolbar.SetZoomState(m_canvas.GetZoomMode(),
                           Format(Tr(Str::ZoomPercent), m_canvas.ZoomPercent()));
    m_toolbar.SetBookmarked(HasBookmarkForCurrentPage());
    m_toolbar.SetSidebarVisible(m_sidebarVisible);
    UpdateTitle();
}

// Keeps the caption in sync with the document title; cheap enough to call from
// every chrome update.
void MainWindow::UpdateTitle() {
    if (m_hwnd == nullptr) {
        return;
    }
    const std::wstring title = WindowTitleText();
    ::SetWindowTextW(m_hwnd, title.c_str());
}

// "Document title - Peng PDF View", or just the app name when nothing is open.
std::wstring MainWindow::WindowTitleText() const {
    if (m_documentOpen && !m_documentPath.empty()) {
        const std::wstring displayName = m_documentTitle.empty()
                                             ? GetFileName(m_documentPath)
                                             : m_documentTitle;
        return Format(L"%s - %s", displayName.c_str(), Tr(Str::AppTitle));
    }
    return Tr(Str::AppTitle);
}

// The status bar shows the toolbar hint while hovering a button, otherwise the
// page position, outline size and zoom.
void MainWindow::UpdateStatus() {
    if (m_hwnd == nullptr) {
        return;
    }
    RECT status = StatusBarRect();
    ::InvalidateRect(m_hwnd, &status, FALSE);
}

// Status bar height is derived from the font so it scales with DPI and language.
int MainWindow::StatusBarHeight() const {
    return theme::Scale(kStatusBarHeightDip, m_dpi);
}

// The status bar occupies the bottom strip of the client area.
RECT MainWindow::StatusBarRect() const {
    RECT client = {};
    ::GetClientRect(m_hwnd, &client);
    const int height = StatusBarHeight();
    RECT rect{client.left, client.bottom - height, client.right, client.bottom};
    return rect;
}

// The splitter is only interactive while the sidebar is visible.
RECT MainWindow::SplitterRect() const {
    RECT client = {};
    ::GetClientRect(m_hwnd, &client);
    const int top = m_toolbar.Height();
    const int bottom = static_cast<int>(client.bottom) - StatusBarHeight();
    const int left = m_sidebarWidthPx;
    RECT rect{left, top, left + theme::Scale(kSplitterWidthDip, m_dpi), bottom};
    return rect;
}

// Paints the frame background and the status bar; child windows paint
// themselves.  WS_CLIPCHILDREN keeps this from overwriting their pixels.
void MainWindow::PaintFrame(HDC dc, const RECT& client) {
    const theme::Palette& colors = theme::Colors();
    theme::FillSolid(dc, client, colors.windowBackground);

    // The splitter lives between the two child windows; highlight it while the
    // pointer is over it so the resize target is discoverable.
    if (m_sidebarVisible) {
        const RECT splitter = SplitterRect();
        if (m_splitterHover || m_splitterDragging) {
            theme::FillSolid(dc, splitter, colors.accentSoft);
            const int centerX = (splitter.left + splitter.right) / 2;
            theme::FillSolid(dc,
                             RECT{centerX, splitter.top, centerX + 1, splitter.bottom},
                             colors.accent);
        }
    }

    // Status bar ------------------------------------------------------------
    const RECT status = StatusBarRect();
    theme::FillSolid(dc, status, colors.windowBackground);
    theme::FillSolid(dc, RECT{status.left, status.top, status.right, status.top + 1},
                     colors.separator);

    std::wstring leftText = m_hintText;
    if (leftText.empty()) {
        if (m_documentOpen) {
            leftText = m_documentTitle.empty() ? GetFileName(m_documentPath)
                                               : m_documentTitle;
            // Section count is a useful hint that the outline was parsed.
            const int outlineCount =
                (m_document != nullptr && m_document->IsLoaded())
                    ? static_cast<int>(m_document->Outline().size())
                    : 0;
            if (outlineCount > 0) {
                leftText += L"   \u00B7   " +
                            Format(Tr(Str::StatusOutlineCount), outlineCount);
            }
        } else {
            leftText = Tr(Str::StatusNoDocument);
        }
    }

    RECT leftRect{status.left + theme::Scale(14, m_dpi), status.top,
                  status.left + static_cast<int>(status.right - status.left) / 2,
                  status.bottom};
    theme::DrawStringEllipsis(dc, leftText, leftRect, colors.textMuted, m_statusFont,
                              DT_LEFT);

    if (m_documentOpen) {
        std::wstring rightText;
        if (m_currentPage >= 0 && m_pageCount > 0) {
            rightText = Format(Tr(Str::StatusPageOf), m_currentPage + 1, m_pageCount);
        } else {
            rightText = FormatSizeText(m_pageCount);
        }
        rightText += L"      ";
        rightText += Format(Tr(Str::ZoomPercent), m_canvas.ZoomPercent());

        RECT rightRect{status.left + static_cast<int>(status.right - status.left) / 2,
                       status.top, status.right - theme::Scale(14, m_dpi), status.bottom};
        theme::DrawString(dc, rightText, rightRect, colors.textMuted, m_statusFont,
                          DT_RIGHT);
    }
}

LRESULT CALLBACK MainWindow::WindowProc(HWND window, UINT message, WPARAM wParam,
                                        LPARAM lParam) {
    MainWindow* self =
        reinterpret_cast<MainWindow*>(::GetWindowLongPtrW(window, GWLP_USERDATA));

    if (message == WM_CREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<MainWindow*>(create->lpCreateParams);
        ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self != nullptr) {
            self->m_hwnd = window;
            self->m_instance = create->hInstance;
            if (!self->OnCreate()) {
                return -1;
            }
        }
        return 0;
    }
    if (self == nullptr) {
        return ::DefWindowProcW(window, message, wParam, lParam);
    }
    return self->HandleMessage(message, wParam, lParam);
}

// Central message handler.  Worker results are translated into method calls
// that take ownership of the heap allocated payloads.
LRESULT MainWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT paint = {};
            HDC dc = ::BeginPaint(m_hwnd, &paint);
            RECT client = {};
            ::GetClientRect(m_hwnd, &client);
            if (client.right > 0 && client.bottom > 0) {
                HDC memoryDc = ::CreateCompatibleDC(dc);
                HBITMAP bitmap = ::CreateCompatibleBitmap(dc, client.right, client.bottom);
                HGDIOBJ oldBitmap = ::SelectObject(memoryDc, bitmap);
                PaintFrame(memoryDc, client);
                ::BitBlt(dc, 0, 0, client.right, client.bottom, memoryDc, 0, 0, SRCCOPY);
                ::SelectObject(memoryDc, oldBitmap);
                ::DeleteObject(bitmap);
                ::DeleteDC(memoryDc);
            }
            ::EndPaint(m_hwnd, &paint);
            return 0;
        }

        case WM_SIZE:
            LayoutChildren();
            return 0;

        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = theme::Scale(920, m_dpi);
            info->ptMinTrackSize.y = theme::Scale(620, m_dpi);
            return 0;
        }

        case WM_COMMAND:
            OnCommand(LOWORD(wParam));
            return 0;

        case WM_APP_HINT: {
            const std::wstring previous = m_hintText;
            m_hintText = (wParam == 1) ? Tr(static_cast<Str>(lParam)) : std::wstring();
            if (previous != m_hintText) {
                UpdateStatus();
            }
            return 0;
        }

        case WM_APP_NAVIGATE_TO_PAGE:
            m_canvas.GoToPage(static_cast<int>(wParam));
            return 0;

        case WM_APP_DELETE_BOOKMARK:
            RemoveBookmarkAt(static_cast<int>(wParam));
            return 0;

        case WM_APP_CANVAS_PAGE_CHANGED: {
            const int page = static_cast<int>(wParam);
            if (page != m_currentPage) {
                m_currentPage = page;
                m_sidebar.SetCurrentPage(page);
                UpdateChrome();
                UpdateStatus();
            }
            return 0;
        }

        case WM_APP_CANVAS_ZOOM_CHANGED:
            UpdateChrome();
            BuildMenu();
            UpdateStatus();
            return 0;

        case WM_RENDER_DOCUMENT_READY:
            OnDocumentReady(reinterpret_cast<DocumentReadyMessage*>(lParam));
            return 0;

        case WM_RENDER_PAGE_READY:
            OnPageReady(reinterpret_cast<PageReadyMessage*>(lParam));
            return 0;

        case WM_APP_TEXT_READY:
            OnTextReady(reinterpret_cast<TextReadyMessage*>(lParam));
            return 0;

        case WM_APP_SEARCH_PAGE:
            OnSearchPage(reinterpret_cast<SearchPageMessage*>(lParam));
            return 0;

        case WM_APP_SEARCH_DONE:
            OnSearchDone(reinterpret_cast<SearchDoneMessage*>(lParam));
            return 0;

        case WM_APP_SEARCH_TEXT_CHANGED:
            // Restart the debounce timer on every edit so the search only runs
            // when the user pauses typing.
            if (m_searchTimer != 0) {
                ::KillTimer(m_hwnd, kSearchDebounceTimerId);
                m_searchTimer = 0;
            }
            m_searchTimer = 1;
            ::SetTimer(m_hwnd, kSearchDebounceTimerId, 250, nullptr);
            return 0;

        case WM_APP_SEARCH_NEXT:
            StepSearchMatch(1);
            return 0;

        case WM_APP_SEARCH_PREVIOUS:
            StepSearchMatch(-1);
            return 0;

        case WM_APP_SEARCH_CLOSE:
            HideSearchBar();
            return 0;

        case WM_APP_SELECTION_CHANGED:
            // Refreshes the enabled state of the Copy menu command.
            BuildMenu();
            return 0;

        case WM_TIMER:
            if (wParam == kSearchDebounceTimerId) {
                ::KillTimer(m_hwnd, kSearchDebounceTimerId);
                m_searchTimer = 0;
                RestartSearch();
            }
            return 0;

        case WM_MOUSEMOVE: {
            if (!m_trackingMouse) {
                TRACKMOUSEEVENT track = {};
                track.cbSize = sizeof(track);
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = m_hwnd;
                ::TrackMouseEvent(&track);
                m_trackingMouse = true;
            }

            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (m_splitterDragging) {
                const int minimum = theme::Scale(kSidebarMinWidthDip, m_dpi);
                RECT client = {};
                ::GetClientRect(m_hwnd, &client);
                const int maximum = std::min(theme::Scale(kSidebarMaxWidthDip, m_dpi),
                                             static_cast<int>(client.right) -
                                                 theme::Scale(360, m_dpi));
                m_sidebarWidthPx = std::clamp(static_cast<int>(point.x), minimum,
                                              std::max(minimum, maximum));
                LayoutChildren();
                ::InvalidateRect(m_hwnd, nullptr, FALSE);
                return 0;
            }

            RECT splitter = SplitterRect();
            const bool hover = m_sidebarVisible && point.x >= splitter.left &&
                               point.x < splitter.right;
            if (hover != m_splitterHover) {
                m_splitterHover = hover;
                ::InvalidateRect(m_hwnd, &splitter, FALSE);
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            m_trackingMouse = false;
            if (m_splitterHover) {
                m_splitterHover = false;
                RECT splitter = SplitterRect();
                ::InvalidateRect(m_hwnd, &splitter, FALSE);
            }
            return 0;

        case WM_LBUTTONDOWN: {
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const RECT splitter = SplitterRect();
            if (m_sidebarVisible && point.x >= splitter.left && point.x < splitter.right &&
                point.y >= splitter.top && point.y < splitter.bottom) {
                m_splitterDragging = true;
                ::SetCapture(m_hwnd);
            }
            return 0;
        }

        case WM_LBUTTONUP:
            if (m_splitterDragging) {
                m_splitterDragging = false;
                ::ReleaseCapture();
                m_settings.Values().sidebarWidth =
                    m_dpi > 0 ? ::MulDiv(m_sidebarWidthPx, 96, static_cast<int>(m_dpi))
                              : m_sidebarWidthPx;
                m_settings.Save();
                ::InvalidateRect(m_hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_CAPTURECHANGED:
            m_splitterDragging = false;
            return 0;

        case WM_SETCURSOR: {
            POINT point = {};
            ::GetCursorPos(&point);
            ::ScreenToClient(m_hwnd, &point);
            const RECT splitter = SplitterRect();
            if (m_sidebarVisible && point.x >= splitter.left && point.x < splitter.right &&
                point.y >= splitter.top && point.y < splitter.bottom) {
                ::SetCursor(::LoadCursorW(nullptr, IDC_SIZEWE));
                return TRUE;
            }
            break;
        }

        case WM_DPICHANGED: {
            const UINT dpi = HIWORD(wParam);
            OnDpiChanged(dpi, reinterpret_cast<const RECT*>(lParam));
            return 0;
        }

        case WM_DROPFILES:
            OnDropFiles(reinterpret_cast<HDROP>(wParam));
            return 0;

        case WM_SETFOCUS:
            if (m_canvas.Handle() != nullptr) {
                ::SetFocus(m_canvas.Handle());
            }
            return 0;

        case WM_CLOSE:
            SaveSettings(true);
            m_bookmarks.Save();
            ::DestroyWindow(m_hwnd);
            return 0;

        case WM_DESTROY:
            if (m_accelerators != nullptr) {
                ::DestroyAcceleratorTable(m_accelerators);
                m_accelerators = nullptr;
            }
            m_renderer.reset();
            m_textService.reset();
            // Both worker threads have stopped, so the queue can no longer grow.
            // Messages that arrived after the last WM_PAINT still own heap
            // payloads that the window will never receive.
            DrainWorkerMessages();
            theme::ReleaseFont(m_statusFont);
            theme::ReleaseFont(m_statusBoldFont);
            ::PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return ::DefWindowProcW(m_hwnd, message, wParam, lParam);
}

// Single command dispatcher for menu items, toolbar buttons and accelerators.
// Recent-file commands occupy a contiguous id range handled in the default arm.
void MainWindow::OnCommand(int command) {
    switch (command) {
        case command::FileOpen:
            ShowOpenDialog();
            break;
        case command::FileClose:
            CloseDocument();
            break;
        case command::FileExit:
            ::PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
            break;
        case command::ViewZoomIn:
            m_canvas.ZoomIn();
            break;
        case command::ViewZoomOut:
            m_canvas.ZoomOut();
            break;
        case command::ViewFitWidth:
            m_canvas.SetZoomMode(ZoomMode::FitWidth);
            break;
        case command::ViewFitPage:
            m_canvas.SetZoomMode(ZoomMode::FitPage);
            break;
        case command::ViewActualSize:
            m_canvas.SetZoom(1.0);
            break;
        case command::ViewToggleSidebar:
            ToggleSidebar();
            break;
        case command::GoFirstPage:
            GoToPage(0);
            break;
        case command::GoPreviousPage:
            StepPage(-1);
            break;
        case command::GoNextPage:
            StepPage(1);
            break;
        case command::GoLastPage:
            GoToPage(m_pageCount - 1);
            break;
        case command::GoToPage:
            ShowGoToPageDialog();
            break;
        case command::BookmarksAdd:
            ToggleBookmarkForCurrentPage();
            break;
        case command::BookmarksRemoveCurrent:
            RemoveBookmarkForCurrentPage();
            break;
        case command::EditFind:
            ShowSearchBar();
            break;
        case command::EditCopy:
            CopySelection();
            break;
        case command::EditClearSelection:
            if (m_searchBar.IsBarVisible()) {
                HideSearchBar();
            } else {
                m_canvas.ClearSelection();
                BuildMenu();
            }
            break;
        case command::SettingsPreferences:
            ShowSettingsDialog();
            break;
        case command::SettingsLanguageEnglish:
            ApplyLanguage(Lang::English);
            break;
        case command::SettingsLanguageChinese:
            ApplyLanguage(Lang::Chinese);
            break;
        case command::HelpAbout:
            ShowAboutDialog();
            break;
        default:
            if (command >= command::kRecentFileFirst &&
                command < command::kRecentFileFirst + 100) {
                const size_t index = static_cast<size_t>(command - command::kRecentFileFirst);
                const auto& recent = m_settings.Values().recentFiles;
                if (index < recent.size()) {
                    OpenDocument(recent[index]);
                }
            }
            break;
    }
}

// Uses the modern IFileOpenDialog; the PDF filter is localised.  The dialog is
// modal to the frame and returns only a path, never a handle.
void MainWindow::ShowOpenDialog() {
    winrt::com_ptr<IFileOpenDialog> dialog;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(dialog.put())))) {
        return;
    }

    const COMDLG_FILTERSPEC filters[] = {
        {Tr(Str::FilterPdfFiles), L"*.pdf"},
        {Tr(Str::FilterAllFiles), L"*.*"},
    };
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    dialog->SetTitle(Tr(Str::OpenDialogTitle));

    if (FAILED(dialog->Show(m_hwnd))) {
        return;
    }
    winrt::com_ptr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.put()))) {
        return;
    }
    PWSTR filePath = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &filePath)) && filePath != nullptr) {
        OpenDocument(filePath);
        ::CoTaskMemFree(filePath);
    }
}

// Opens a document: closes the previous one, parses the structure with our own
// parser (outline/metadata) and starts both worker services with a fresh token.
void MainWindow::OpenDocument(const std::wstring& path) {
    if (path.empty()) {
        return;
    }
    const std::wstring fullPath = GetFullPath(path);
    if (!FileExists(fullPath)) {
        const std::wstring text = Format(Tr(Str::ErrorOpenMessage), GetFileName(path).c_str(),
                                         Tr(Str::ErrorFileMissing));
        ::MessageBoxW(m_hwnd, text.c_str(), Tr(Str::ErrorOpenTitle),
                      MB_OK | MB_ICONERROR);
        return;
    }

    CloseDocument();

    m_documentPath = fullPath;
    m_documentTitle = GetFileStem(fullPath);
    m_documentToken++;
    m_currentPage = -1;
    m_pageCount = 0;

    // Parse the document structure first: it is fast and provides the outline
    // and metadata long before the renderer has produced the first page.
    m_document = std::make_shared<pdf::Document>();
    std::wstring parseError;
    if (!m_document->LoadFromFile(fullPath, &parseError)) {
        LogError(L"Structure parsing failed for %s: %s", fullPath.c_str(),
                 parseError.c_str());
    } else if (!m_document->Metadata().title.empty()) {
        m_documentTitle = m_document->Metadata().title;
    }

    m_canvas.SetRenderService(m_renderer.get(), m_documentToken);
    m_canvas.SetTextService(m_textService.get(), m_documentToken);
    if (m_textService) {
        m_textService->OpenDocument(m_document, m_documentToken);
    }
    m_renderer->OpenDocument(fullPath, m_documentToken);

    UpdateChrome();
    UpdateStatus();
}

// Releases the document and every derived state (search, selection, outline,
// bookmarks view).  The token bump makes late worker results harmless.
void MainWindow::CloseDocument() {
    m_documentToken++;
    m_documentOpen = false;
    m_pageCount = 0;
    m_currentPage = -1;
    m_documentPath.clear();
    m_documentTitle.clear();
    m_hintText.clear();
    m_document.reset();

    if (m_textService) {
        m_textService->CloseDocument();
    }
    m_searchMatches.clear();
    m_currentSearchMatch = -1;
    m_searchPending = false;
    m_searchId = 0;
    if (m_searchTimer != 0) {
        ::KillTimer(m_hwnd, kSearchDebounceTimerId);
        m_searchTimer = 0;
    }
    m_searchBar.HideBar();

    if (m_renderer) {
        m_renderer->CloseDocument();
    }
    m_canvas.SetRenderService(m_renderer.get(), m_documentToken);
    m_canvas.SetTextService(m_textService.get(), m_documentToken);
    m_canvas.ClearDocument();
    m_sidebar.Reset();

    BuildMenu();
    UpdateChrome();
    UpdateStatus();
}

// The renderer reports the authoritative page count and geometry.  Error
// classifications are mapped onto localised messages before closing.
void MainWindow::OnDocumentReady(DocumentReadyMessage* message) {
    std::unique_ptr<DocumentReadyMessage> holder(message);
    if (message == nullptr) {
        return;
    }
    LogInfo(L"Document ready: token=%llu ok=%d kind=%d",
            static_cast<unsigned long long>(message->token), message->ok ? 1 : 0,
            static_cast<int>(message->errorKind));
    if (message->token != m_documentToken) {
        LogInfo(L"Discarding a stale render result");
        return;
    }

    if (!message->ok) {
        // Map the engine's error classification onto a localised explanation.
        const wchar_t* reason = Tr(Str::ErrorInvalidPdf);
        switch (message->errorKind) {
            case RenderErrorKind::FileNotFound:
                reason = Tr(Str::ErrorFileMissing);
                break;
            case RenderErrorKind::AccessDenied:
                reason = Tr(Str::ErrorAccessDenied);
                break;
            case RenderErrorKind::PasswordProtected:
                reason = Tr(Str::ErrorPassword);
                break;
            case RenderErrorKind::InvalidDocument:
                reason = Tr(Str::ErrorInvalidPdf);
                break;
            case RenderErrorKind::EngineFailure:
                reason = Tr(Str::ErrorEngineFailure);
                break;
            case RenderErrorKind::None:
            default:
                break;
        }
        const std::wstring text =
            Format(Tr(Str::ErrorOpenMessage), GetFileName(m_documentPath).c_str(), reason);
        ::MessageBoxW(m_hwnd, text.c_str(), Tr(Str::ErrorOpenTitle),
                      MB_OK | MB_ICONERROR);
        CloseDocument();
        return;
    }
    if (message->pages.empty()) {
        ::MessageBoxW(m_hwnd, Tr(Str::ErrorNoPages), Tr(Str::ErrorOpenTitle),
                      MB_OK | MB_ICONERROR);
        CloseDocument();
        return;
    }

    m_pageCount = static_cast<int>(message->pages.size());
    m_currentPage = 0;
    m_documentOpen = true;

    // A new document starts with no search results or selection.
    m_searchMatches.clear();
    m_currentSearchMatch = -1;
    m_searchPending = false;
    m_searchId = 0;
    m_canvas.ClearSearch();
    m_searchBar.HideBar();

    // The outline comes from our own parser; the page geometry comes from the
    // Windows PDF engine, which is authoritative for rendering.
    std::vector<pdf::OutlineEntry> outline;
    if (m_document && m_document->IsLoaded()) {
        outline = m_document->Outline();
    }
    m_sidebar.SetOutline(outline);
    m_sidebar.SetCurrentPage(0);

    m_canvas.SetRenderService(m_renderer.get(), m_documentToken);
    m_canvas.SetDocument(message->pages);

    RefreshBookmarkViews();

    m_settings.AddRecentFile(m_documentPath);
    m_settings.Save();

    BuildMenu();
    UpdateChrome();
    UpdateStatus();
    ::SetFocus(m_canvas.Handle());
}

// Worker results are forwarded immediately; the canvas checks the tokens.
void MainWindow::OnPageReady(PageReadyMessage* message) {
    std::unique_ptr<PageReadyMessage> holder(message);
    if (message == nullptr) {
        return;
    }
    m_canvas.OnPageReady(*message);
}

// Text results are forwarded to the canvas, which owns selection state.
void MainWindow::OnTextReady(TextReadyMessage* message) {
    std::unique_ptr<TextReadyMessage> holder(message);
    if (message == nullptr) {
        return;
    }
    m_canvas.OnTextReady(*message);
}

// A selected single line of text is a natural starting query, so Ctrl+F
// prefills the bar with it (trimmed and whitespace collapsed).
void MainWindow::ShowSearchBar() {
    if (!m_documentOpen || m_hwnd == nullptr) {
        return;
    }
    LayoutChildren();

    // A single line selection is a natural search query.
    std::wstring initial;
    if (m_canvas.HasSelection()) {
        const std::wstring selected = m_canvas.SelectedText();
        std::wstring singleLine;
        singleLine.reserve(selected.size());
        for (wchar_t character : selected) {
            if (character == L'\r' || character == L'\n') {
                if (!singleLine.empty() && singleLine.back() != L' ') {
                    singleLine.push_back(L' ');
                }
                continue;
            }
            singleLine.push_back(character);
        }
        const std::wstring trimmed(TrimWide(singleLine));
        if (!trimmed.empty() && trimmed.size() <= 120) {
            initial = trimmed;
        }
    }

    m_searchBar.ShowForSearch(initial);
    // The bar must sit above the canvas; children created during WM_CREATE are
    // appended at the bottom of the z-order, so raise it explicitly.
    ::SetWindowPos(m_searchBar.Handle(), HWND_TOP, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (!m_searchBar.Query().empty()) {
        RestartSearch();
    } else {
        UpdateSearchStatus();
    }
}

// Hiding the bar also cancels the worker and clears the canvas highlights; the
// canvas keeps the selection so the user can still copy it.
void MainWindow::HideSearchBar() {
    // Cancel the worker first so no result can arrive after the highlights are
    // cleared and make the status bar inconsistent.
    if (m_searchTimer != 0) {
        ::KillTimer(m_hwnd, kSearchDebounceTimerId);
        m_searchTimer = 0;
    }
    m_searchBar.HideBar();
    if (m_textService) {
        m_textService->CancelSearch();
    }
    m_canvas.ClearSearch();
    m_searchMatches.clear();
    m_currentSearchMatch = -1;
    m_searchPending = false;
    m_searchId = 0;

    if (::IsWindow(m_hwnd)) {
        ::SetFocus(m_canvas.Handle());
    }
    BuildMenu();
}

// Starts a new query.  The canvas is cleared first so stale highlights from the
// previous query disappear immediately, then results stream in page by page.
void MainWindow::RestartSearch() {
    if (!m_textService || m_hwnd == nullptr) {
        return;
    }
    const std::wstring query = NormalizeSearchQuery(m_searchBar.Query());
    m_searchMatches.clear();
    m_currentSearchMatch = -1;
    m_canvas.ClearSearch();

    if (query.empty()) {
        m_searchId = 0;
        m_searchPending = false;
        m_searchBar.SetStatus(L"", false);
        return;
    }

    m_searchId = m_textService->StartSearch(query);
    m_searchPending = true;
    m_searchBar.SetStatus(Tr(Str::SearchSearching), false);
}

// Shows "searching", "no results" or "N of M" depending on the current state.
void MainWindow::UpdateSearchStatus() {
    if (!m_searchBar.IsBarVisible()) {
        return;
    }
    if (m_searchBar.Query().empty()) {
        m_searchBar.SetStatus(L"", false);
        return;
    }
    if (m_searchMatches.empty()) {
        if (m_searchPending) {
            m_searchBar.SetStatus(Tr(Str::SearchSearching), false);
        } else {
            m_searchBar.SetStatus(Tr(Str::SearchNoResults), true);
        }
        return;
    }
    if (m_currentSearchMatch < 0) {
        m_currentSearchMatch = 0;
    }
    m_searchBar.SetStatus(Format(Tr(Str::SearchResultCount), m_currentSearchMatch + 1,
                                static_cast<int>(m_searchMatches.size())),
                          false);
}

// Validates the index, updates the canvas (which scrolls) and refreshes the
// "N of M" status.  Wrapping is handled by StepSearchMatch.
void MainWindow::SetCurrentSearchMatch(int index) {
    if (m_searchMatches.empty()) {
        return;
    }
    const int count = static_cast<int>(m_searchMatches.size());
    if (index < 0) {
        index = count - 1;
    } else if (index >= count) {
        index = 0;
    }
    m_currentSearchMatch = index;
    m_canvas.SetCurrentSearchMatch(index);
    UpdateSearchStatus();
}

// Moves to the previous/next hit and wraps around at both ends.
void MainWindow::StepSearchMatch(int delta) {
    if (m_searchMatches.empty()) {
        return;
    }
    const int count = static_cast<int>(m_searchMatches.size());
    int next = m_currentSearchMatch + delta;
    if (m_currentSearchMatch < 0) {
        next = delta >= 0 ? 0 : count - 1;
    }
    next = ((next % count) + count) % count;
    SetCurrentSearchMatch(next);
}

// A page of matches arrived: append it to the flat list and let the canvas
// highlight just that page.  The first hit is focused automatically.
void MainWindow::OnSearchPage(SearchPageMessage* message) {
    std::unique_ptr<SearchPageMessage> holder(message);
    if (message == nullptr || message->searchId != m_searchId) {
        return;
    }
    if (!message->matches.empty()) {
        m_searchMatches.insert(m_searchMatches.end(), message->matches.begin(),
                               message->matches.end());
        m_canvas.AppendSearchMatches(message->matches);
        if (m_currentSearchMatch < 0 && !m_searchMatches.empty()) {
            // Jump to the first hit as soon as it is known.
            SetCurrentSearchMatch(0);
        }
    }
    UpdateSearchStatus();
}

// The worker finished walking the document; update the status if the query is
// still the active one.
void MainWindow::OnSearchDone(SearchDoneMessage* message) {
    std::unique_ptr<SearchDoneMessage> holder(message);
    if (message == nullptr || message->searchId != m_searchId) {
        return;
    }
    m_searchPending = false;
    if (m_searchMatches.empty()) {
        m_searchBar.SetStatus(Tr(Str::SearchNoResults), true);
        return;
    }
    if (m_currentSearchMatch < 0) {
        SetCurrentSearchMatch(0);
    }
    UpdateSearchStatus();
}

// Copied text is CF_UNICODETEXT so Chinese and Latin text survive paste in any
// application.  Clipboard ownership transfers to the system on success.
void MainWindow::CopySelection() {
    const std::wstring text = m_canvas.SelectedText();
    if (text.empty() || !::OpenClipboard(m_hwnd)) {
        return;
    }
    ::EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory != nullptr) {
        void* target = ::GlobalLock(memory);
        if (target != nullptr) {
            std::memcpy(target, text.c_str(), bytes);
            ::GlobalUnlock(memory);
        }
        if (::SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
            ::GlobalFree(memory);
        }
    }
    ::CloseClipboard();
}

// Scrolls the canvas to the top of a page and refreshes the reading position.
void MainWindow::DrainWorkerMessages() {
    if (m_hwnd == nullptr) {
        return;
    }
    MSG message = {};
    // One pass per message type; lParam owns a heap object that the normal
    // handler would have wrapped in a unique_ptr.
    while (::PeekMessageW(&message, m_hwnd, WM_RENDER_DOCUMENT_READY,
                          WM_RENDER_DOCUMENT_READY, PM_REMOVE)) {
        delete reinterpret_cast<DocumentReadyMessage*>(message.lParam);
    }
    while (::PeekMessageW(&message, m_hwnd, WM_RENDER_PAGE_READY,
                          WM_RENDER_PAGE_READY, PM_REMOVE)) {
        delete reinterpret_cast<PageReadyMessage*>(message.lParam);
    }
    while (::PeekMessageW(&message, m_hwnd, WM_APP_TEXT_READY, WM_APP_TEXT_READY,
                          PM_REMOVE)) {
        delete reinterpret_cast<TextReadyMessage*>(message.lParam);
    }
    while (::PeekMessageW(&message, m_hwnd, WM_APP_SEARCH_PAGE, WM_APP_SEARCH_PAGE,
                          PM_REMOVE)) {
        delete reinterpret_cast<SearchPageMessage*>(message.lParam);
    }
    while (::PeekMessageW(&message, m_hwnd, WM_APP_SEARCH_DONE, WM_APP_SEARCH_DONE,
                          PM_REMOVE)) {
        delete reinterpret_cast<SearchDoneMessage*>(message.lParam);
    }
}

// Clamps to the document and lets the canvas scroll; the canvas reports the
// new reading position back through WM_APP_CANVAS_PAGE_CHANGED.
void MainWindow::GoToPage(int pageIndex) {
    if (!m_documentOpen || m_pageCount <= 0) {
        return;
    }
    m_canvas.GoToPage(std::clamp(pageIndex, 0, m_pageCount - 1));
}

// Moves by whole pages from the current reading position.
void MainWindow::StepPage(int delta) {
    if (!m_documentOpen) {
        return;
    }
    const int target = std::clamp(m_currentPage + delta, 0, m_pageCount - 1);
    m_canvas.GoToPage(target);
}

// Modal page-number prompt; invalid input keeps the dialog open.
void MainWindow::ShowGoToPageDialog() {
    if (!m_documentOpen) {
        return;
    }
    GoToPageDialog dialog(m_pageCount, m_currentPage);
    if (dialog.ShowModal(m_hwnd, m_instance)) {
        m_canvas.GoToPage(dialog.SelectedPage());
    }
}

// Modal settings dialog; language changes are applied immediately by the
// dialog itself, so the frame only has to persist the returned values.
void MainWindow::ShowSettingsDialog() {
    const Lang originalLanguage = GetLanguage();
    SettingsDialog dialog(m_settings.Values(), originalLanguage);

    if (!dialog.ShowModal(m_hwnd, m_instance)) {
        // The dialog previews language changes live; restore the old language.
        ApplyLanguage(originalLanguage);
        return;
    }

    const AppSettings updated = dialog.UpdatedSettings();
    ApplyLanguage(dialog.SelectedLanguage());

    m_settings.Values().zoomMode = updated.zoomMode;
    m_settings.Values().zoom = updated.zoom;
    m_settings.Values().sidebarVisible = updated.sidebarVisible;
    m_sidebarVisible = updated.sidebarVisible;

    m_canvas.SetZoomMode(updated.zoomMode);
    if (updated.zoomMode == ZoomMode::Custom) {
        m_canvas.SetZoom(updated.zoom);
    }

    m_settings.Save();
    LayoutChildren();
    BuildMenu();
    UpdateChrome();
    UpdateStatus();
    ::InvalidateRect(m_hwnd, nullptr, TRUE);
}

// About box with the version, vendor and a short description.
void MainWindow::ShowAboutDialog() {
    std::wstring text = Tr(Str::AppTitle);
    text += L"\n";
    text += Format(Tr(Str::AboutVersion), kAppVersion);
    text += L"\n\n";
    text += Tr(Str::AboutDescription);
    text += L"\n\n";
    text += Tr(Str::AboutEngine);
    text += L"\n";
    text += Tr(Str::AboutAuthor);

    ::MessageBoxW(m_hwnd, text.c_str(), Tr(Str::AboutTitle), MB_OK | MB_ICONINFORMATION);
}

// Bookmark lookups are linear over the current document's list, which is small
// in practice (tens of entries at most).
bool MainWindow::HasBookmarkForCurrentPage() const {
    if (!m_documentOpen || m_currentPage < 0 || m_documentPath.empty()) {
        return false;
    }
    return m_bookmarks.Has(m_documentPath, m_currentPage);
}

// The label saved with a bookmark: the current outline title when the page is
// inside a section, otherwise a localised "Page N" fallback.
std::wstring MainWindow::BookmarkTitleForCurrentPage() const {
    // Prefer the outline entry that starts on this page; otherwise leave the
    // title empty so the sidebar can show a localised "Page N" label instead.
    if (m_document && m_document->IsLoaded()) {
        for (const pdf::OutlineEntry& entry : m_document->Outline()) {
            if (entry.pageIndex == m_currentPage && !entry.title.empty()) {
                return entry.title;
            }
        }
    }
    return std::wstring();
}

// Star button / Ctrl+D: toggles a bookmark for the current page and persists
// the store immediately so a crash cannot lose it.
void MainWindow::ToggleBookmarkForCurrentPage() {
    if (!m_documentOpen || m_currentPage < 0) {
        return;
    }
    if (HasBookmarkForCurrentPage()) {
        RemoveBookmarkForCurrentPage();
        return;
    }

    m_bookmarks.Add(m_documentPath, m_currentPage, BookmarkTitleForCurrentPage());
    m_bookmarks.Save();
    RefreshBookmarkViews();
}

// Removes the first bookmark that points at the current page.
void MainWindow::RemoveBookmarkForCurrentPage() {
    if (!HasBookmarkForCurrentPage()) {
        return;
    }
    std::vector<Bookmark>& bookmarks = m_bookmarks.ForDocument(m_documentPath);
    for (size_t index = 0; index < bookmarks.size(); ++index) {
        if (bookmarks[index].page == m_currentPage) {
            RemoveBookmarkAt(static_cast<int>(index));
            return;
        }
    }
}

// Called from the sidebar context menu; the index is into the current
// document's bookmark list.
void MainWindow::RemoveBookmarkAt(int index) {
    if (!m_documentOpen || index < 0) {
        return;
    }
    m_bookmarks.RemoveAt(m_documentPath, static_cast<size_t>(index));
    m_bookmarks.Save();
    RefreshBookmarkViews();
}

// Pushes the bookmark list into the sidebar and refreshes the star and menu.
void MainWindow::RefreshBookmarkViews() {
    if (!m_documentOpen) {
        m_sidebar.SetBookmarks({});
        m_toolbar.SetBookmarked(false);
        return;
    }
    m_sidebar.SetBookmarks(m_bookmarks.ForDocument(m_documentPath));
    m_toolbar.SetBookmarked(HasBookmarkForCurrentPage());
    BuildMenu();
}

// Switches every visible string at once.  Page bitmaps and extracted text do
// not depend on the language and therefore stay cached.
void MainWindow::ApplyLanguage(Lang language) {
    SetLanguage(language);
    m_settings.Values().languageTag = (language == Lang::Chinese) ? L"zh" : L"en";
    m_settings.Save();

    m_toolbar.ApplyLanguage();
    m_sidebar.ApplyLanguage();
    m_canvas.ApplyLanguage();
    m_searchBar.ApplyLanguage();
    RecreateFonts();

    BuildMenu();
    LayoutChildren();
    UpdateChrome();
    UpdateStatus();
    ::InvalidateRect(m_hwnd, nullptr, TRUE);
}

// Hides/shows the sidebar and re-lays out the canvas; the width is remembered.
void MainWindow::ToggleSidebar() {
    m_sidebarVisible = !m_sidebarVisible;
    m_settings.Values().sidebarVisible = m_sidebarVisible;
    m_settings.Save();
    LayoutChildren();
    m_toolbar.SetSidebarVisible(m_sidebarVisible);
    BuildMenu();
    ::InvalidateRect(m_hwnd, nullptr, FALSE);
}

// The manifest declares Per-Monitor V2, so Windows sends the suggested new
// rectangle; the frame applies it and rebuilds all metrics.
void MainWindow::OnDpiChanged(UINT dpi, const RECT* suggestedRect) {
    m_dpi = dpi;
    if (suggestedRect != nullptr) {
        ::SetWindowPos(m_hwnd, nullptr, suggestedRect->left, suggestedRect->top,
                       suggestedRect->right - suggestedRect->left,
                       suggestedRect->bottom - suggestedRect->top,
                       SWP_NOZORDER | SWP_NOACTIVATE);
    }

    m_toolbar.SetDpi(m_dpi);
    m_sidebar.SetDpi(m_dpi);
    m_canvas.SetDpi(m_dpi);
    RecreateFonts();

    m_sidebarWidthPx = theme::Scale(
        std::clamp(m_settings.Values().sidebarWidth, kSidebarMinWidthDip,
                   kSidebarMaxWidthDip),
        m_dpi);

    LayoutChildren();
    UpdateChrome();
    UpdateStatus();
    ::InvalidateRect(m_hwnd, nullptr, TRUE);
}

// Dropping a PDF anywhere on the frame opens it; multiple files use the first.
void MainWindow::OnDropFiles(HDROP drop) {
    if (drop == nullptr) {
        return;
    }
    const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    if (count > 0) {
        wchar_t path[MAX_PATH * 2] = {};
        if (::DragQueryFileW(drop, 0, path, static_cast<UINT>(std::size(path))) > 0) {
            OpenDocument(path);
        }
    }
    ::DragFinish(drop);
}

// `includePlacement` is false while the window is minimised/maximised so the
// saved restore rectangle stays useful.
void MainWindow::SaveSettings(bool includePlacement) {
    if (m_hwnd == nullptr) {
        return;
    }
    if (includePlacement) {
        WINDOWPLACEMENT placement = {};
        placement.length = sizeof(placement);
        if (::GetWindowPlacement(m_hwnd, &placement)) {
            m_settings.Values().window.maximized =
                placement.showCmd == SW_SHOWMAXIMIZED;
            m_settings.Values().window.x = placement.rcNormalPosition.left;
            m_settings.Values().window.y = placement.rcNormalPosition.top;
            m_settings.Values().window.width =
                placement.rcNormalPosition.right - placement.rcNormalPosition.left;
            m_settings.Values().window.height =
                placement.rcNormalPosition.bottom - placement.rcNormalPosition.top;
        }
        if (m_dpi > 0) {
            m_settings.Values().sidebarWidth =
                ::MulDiv(m_sidebarWidthPx, 96, static_cast<int>(m_dpi));
        }
    }
    m_settings.Values().sidebarVisible = m_sidebarVisible;
    m_settings.Save();
}

}  // namespace pengpdf


