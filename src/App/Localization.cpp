// =============================================================================
//  Localization.cpp
//  String tables for the English and Simplified Chinese user interface.
// =============================================================================
#include "App/Localization.h"

#include <windows.h>

#include <unordered_map>

namespace pengpdf {
namespace {

// One row of the table.  Both translations must be complete, and they must
// carry the same printf placeholders (%d / %s / %%) because the callers pass a
// fixed argument list to Format().  A mnemonic is marked with '&' and the
// keyboard shortcut is appended after a TAB, which is the layout Win32 menus
// expect when the string is used as a menu item.
struct StringEntry {
    Str identifier;
    const wchar_t* english;
    const wchar_t* chinese;
};

// The table is intentionally a flat array: adding a language later only means
// adding one field and one lookup branch.
constexpr StringEntry kStringTable[] = {
    // Application shell ------------------------------------------------------
    {Str::AppTitle, L"Peng PDF View", L"Peng PDF 阅读器"},
    {Str::MenuFile, L"&File", L"文件(&F)"},
    {Str::MenuOpen, L"&Open...\tCtrl+O", L"打开(&O)...\tCtrl+O"},
    {Str::MenuRecent, L"Recent &Files", L"最近打开(&R)"},
    {Str::MenuNoRecent, L"(empty)", L"（空）"},
    {Str::MenuClose, L"&Close Document\tCtrl+W", L"关闭文档(&C)\tCtrl+W"},
    {Str::MenuExit, L"E&xit\tAlt+F4", L"退出(&X)\tAlt+F4"},
    {Str::MenuEdit, L"&Edit", L"编辑(&E)"},
    {Str::MenuFind, L"&Find...\tCtrl+F", L"查找(&F)...\tCtrl+F"},
    {Str::MenuCopy, L"&Copy\tCtrl+C", L"复制(&C)\tCtrl+C"},
    {Str::MenuView, L"&View", L"视图(&V)"},
    {Str::MenuZoomIn, L"Zoom &In\tCtrl++", L"放大(&I)\tCtrl++"},
    {Str::MenuZoomOut, L"Zoom &Out\tCtrl+-", L"缩小(&O)\tCtrl+-"},
    {Str::MenuFitWidth, L"Fit &Width\tCtrl+1", L"适合宽度(&W)\tCtrl+1"},
    {Str::MenuFitPage, L"Fit &Page\tCtrl+2", L"适合页面(&P)\tCtrl+2"},
    {Str::MenuActualSize, L"&Actual Size\tCtrl+0", L"实际大小(&A)\tCtrl+0"},
    {Str::MenuShowSidebar, L"Show &Sidebar\tF4", L"显示侧边栏(&S)\tF4"},
    {Str::MenuGo, L"&Go", L"转到(&G)"},
    {Str::MenuFirstPage, L"&First Page\tHome", L"第一页(&F)\tHome"},
    {Str::MenuPreviousPage, L"&Previous Page\tPgUp", L"上一页(&P)\tPgUp"},
    {Str::MenuNextPage, L"&Next Page\tPgDn", L"下一页(&N)\tPgDn"},
    {Str::MenuLastPage, L"&Last Page\tEnd", L"最后一页(&L)\tEnd"},
    {Str::MenuGoToPage, L"&Go to Page...\tCtrl+G", L"跳转到页面(&G)...\tCtrl+G"},
    {Str::MenuBookmarks, L"&Bookmarks", L"书签(&B)"},
    {Str::MenuAddBookmark, L"&Add Bookmark\tCtrl+D", L"添加书签(&A)\tCtrl+D"},
    {Str::MenuRemoveBookmark, L"&Remove Bookmark\tCtrl+Shift+D",
     L"删除书签(&R)\tCtrl+Shift+D"},
    {Str::MenuSettings, L"&Settings", L"设置(&S)"},
    {Str::MenuPreferences, L"&Preferences...\tCtrl+,", L"首选项(&P)...\tCtrl+,"},
    {Str::MenuLanguage, L"&Language", L"语言(&L)"},
    {Str::MenuLanguageEnglish, L"English", L"English"},
    {Str::MenuLanguageChinese, L"简体中文", L"简体中文"},
    {Str::MenuHelp, L"&Help", L"帮助(&H)"},
    {Str::MenuAbout, L"&About Peng PDF View", L"关于 Peng PDF 阅读器(&A)"},
    {Str::OpenDialogTitle, L"Open PDF document", L"打开 PDF 文档"},
    {Str::FilterPdfFiles, L"PDF documents (*.pdf)", L"PDF 文档 (*.pdf)"},
    {Str::FilterAllFiles, L"All files (*.*)", L"所有文件 (*.*)"},

    // Toolbar hints ----------------------------------------------------------
    {Str::HintOpen, L"Open a PDF document", L"打开 PDF 文档"},
    {Str::HintSidebar, L"Show or hide the sidebar", L"显示或隐藏侧边栏"},
    {Str::HintPreviousPage, L"Previous page", L"上一页"},
    {Str::HintNextPage, L"Next page", L"下一页"},
    {Str::HintZoomOut, L"Zoom out", L"缩小"},
    {Str::HintZoomIn, L"Zoom in", L"放大"},
    {Str::HintFitWidth, L"Fit the page width to the window", L"使页面宽度适合窗口"},
    {Str::HintFitPage, L"Fit the whole page in the window", L"使整页适合窗口"},
    {Str::HintBookmark, L"Bookmark the current page", L"为当前页添加书签"},
    {Str::HintSettings, L"Settings", L"设置"},
    {Str::HintGoToPage, L"Jump to another page", L"跳转到指定页面"},

    // Sidebar ----------------------------------------------------------------
    {Str::TabContents, L"Contents", L"目录"},
    {Str::TabBookmarks, L"Bookmarks", L"书签"},
    {Str::NoOutline, L"This document has no table of contents.",
     L"此文档没有目录。"},
    {Str::NoBookmarks, L"No bookmarks yet.", L"还没有书签。"},
    {Str::BookmarkHint, L"Press Ctrl+D to bookmark the current page.",
     L"按 Ctrl+D 为当前页添加书签。"},
    {Str::BookmarkPageLabel, L"Page %d", L"第 %d 页"},
    {Str::DeleteBookmark, L"Delete Bookmark", L"删除书签"},

    // Empty document state ---------------------------------------------------
    {Str::EmptyTitle, L"Open a PDF document to start reading",
     L"打开一个 PDF 文档开始阅读"},
    {Str::EmptyHint, L"Press Ctrl+O or drop a file onto this window",
     L"按 Ctrl+O，或将文件拖放到此窗口"},

    // Status bar -------------------------------------------------------------
    {Str::StatusReady, L"Ready", L"就绪"},
    {Str::StatusNoDocument, L"No document", L"未打开文档"},
    {Str::StatusLoading, L"Rendering page...", L"正在渲染页面..."},
    {Str::StatusPageOf, L"Page %d of %d", L"第 %d 页，共 %d 页"},
    {Str::StatusOutlineCount, L"%d sections", L"%d 个章节"},

    // Zoom -------------------------------------------------------------------
    {Str::ZoomPercent, L"%d%%", L"%d%%"},
    {Str::ZoomFitWidthShort, L"Fit width", L"适合宽度"},
    {Str::ZoomFitPageShort, L"Fit page", L"适合页面"},
    {Str::ZoomActualSizeShort, L"Actual size", L"实际大小"},

    // Settings dialog --------------------------------------------------------
    {Str::SettingsTitle, L"Settings", L"设置"},
    {Str::SettingsLanguage, L"Language", L"语言"},
    {Str::SettingsLanguageHint, L"Choose the language of the interface.",
     L"选择界面显示语言。"},
    {Str::SettingsDefaultZoom, L"Default zoom", L"默认缩放"},
    {Str::SettingsShowSidebar, L"Show the sidebar when opening a document",
     L"打开文档时显示侧边栏"},
    {Str::SettingsOk, L"OK", L"确定"},
    {Str::SettingsCancel, L"Cancel", L"取消"},

    // Search -----------------------------------------------------------------
    {Str::SearchPlaceholder, L"Search in document", L"在文档中搜索"},
    {Str::SearchSearching, L"Searching...", L"正在搜索..."},
    {Str::SearchNoResults, L"No results", L"没有找到结果"},
    {Str::SearchResultCount, L"%d of %d", L"第 %d 个，共 %d 个"},

    // Go to page dialog ------------------------------------------------------
    {Str::GoToPageTitle, L"Go to Page", L"跳转到页面"},
    {Str::GoToPagePrompt, L"Page number (1 - %d):", L"页码（1 - %d）："},
    {Str::GoToPageInvalid, L"Please enter a valid page number.", L"请输入有效的页码。"},

    // Errors -----------------------------------------------------------------
    {Str::ErrorOpenTitle, L"Cannot open document", L"无法打开文档"},
    {Str::ErrorOpenMessage, L"Could not open \"%s\".\n\n%s", L"无法打开“%s”。\n\n%s"},
    {Str::ErrorNoPages, L"The document does not contain any pages.",
     L"该文档不包含任何页面。"},
    {Str::ErrorFileMissing, L"The file does not exist.", L"文件不存在。"},
    {Str::ErrorPassword, L"The document is protected by a password.",
     L"该文档受密码保护，无法打开。"},
    {Str::ErrorInvalidPdf, L"The file is not a valid PDF document.",
     L"该文件不是有效的 PDF 文档。"},
    {Str::ErrorAccessDenied, L"Access to the file was denied.", L"无法访问该文件。"},
    {Str::ErrorEngineFailure, L"The PDF engine could not open the document.",
     L"PDF 引擎无法打开该文档。"},

    // About ------------------------------------------------------------------
    {Str::AboutTitle, L"About Peng PDF View", L"关于 Peng PDF 阅读器"},
    {Str::AboutVersion, L"Version %s", L"版本 %s"},
    {Str::AboutDescription,
     L"A lightweight PDF reader written in C++ with a native Win32 interface, "
     L"a custom PDF structure parser and bilingual English / Chinese UI.",
     L"使用 C++ 与原生 Win32 界面编写的轻量级 PDF 阅读器，"
     L"内置自行实现的 PDF 结构解析器，支持中英文双语界面。"},
    {Str::AboutEngine, L"Page rendering: Windows PDF engine (Windows.Data.Pdf)",
     L"页面渲染：Windows PDF 引擎（Windows.Data.Pdf）"},
    {Str::AboutAuthor, L"Author: Peng", L"作者：Peng"},
    {Str::AboutClose, L"Close", L"关闭"},
};

// The active language is a plain global: it is only written from the UI thread
// by SetLanguage(), which happens before the windows re-apply their captions.
Lang g_language = Lang::English;

using StringMap = std::unordered_map<int, const wchar_t*>;

// Flattens the table into a hash map.  This is cheap for a hundred entries and
// only runs on startup and on a language switch; emplace() keeps the first row
// if an identifier is accidentally listed twice.
StringMap BuildMap(Lang language) {
    StringMap map;
    map.reserve(std::size(kStringTable));
    for (const StringEntry& entry : kStringTable) {
        map.emplace(static_cast<int>(entry.identifier),
                    language == Lang::Chinese ? entry.chinese : entry.english);
    }
    return map;
}

// Caches the map for the active language.  Tr() is called for every label that
// is painted or laid out, so rebuilding the map on each call would show up in
// the paint path.  The static locals are initialised once, which is thread
// safe since C++11, and refreshed lazily when SetLanguage() changes the global.
//
// Only the UI thread touches this cache.
const StringMap& CurrentMap() {
    static Lang cachedLanguage = Lang::English;
    static StringMap cachedMap = BuildMap(Lang::English);
    if (cachedLanguage != g_language) {
        cachedMap = BuildMap(g_language);
        cachedLanguage = g_language;
    }
    return cachedMap;
}

}  // namespace

Lang DetectSystemLanguage() {
    // GetUserDefaultUILanguage() honours the user's Windows display language
    // (including the MUI override), which is what the menus should match.
    // Only the primary language is inspected: the sublanguage distinguishes
    // Simplified from Traditional Chinese, but the UI ships Simplified only.
    const LANGID languageId = ::GetUserDefaultUILanguage();
    const WORD primary = PRIMARYLANGID(languageId);
    return primary == LANG_CHINESE ? Lang::Chinese : Lang::English;
}

Lang GetLanguage() {
    return g_language;
}

void SetLanguage(Lang language) {
    g_language = language;
}

bool IsChineseInterface() {
    return g_language == Lang::Chinese;
}

const wchar_t* Tr(Str identifier) {
    // A missing identifier yields L"" instead of null: every caller feeds the
    // result to a Win32 API or std::wstring, and a blank label is easier to
    // spot during testing than a crash in the paint code.
    const StringMap& map = CurrentMap();
    const auto it = map.find(static_cast<int>(identifier));
    return it == map.end() ? L"" : it->second;
}

}  // namespace pengpdf
