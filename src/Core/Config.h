// =============================================================================
//  Config.h
//  Compile-time constants shared by every module of PengPDFView.
//
//  Keeping these values in one place makes it easy to rebrand the application
//  or to bump the version number without hunting through the source tree.
// =============================================================================
#pragma once

#include <cstdint>

namespace pengpdf {

// Product identity ------------------------------------------------------------
// The internal (ASCII) name is used for registry-free paths and window classes,
// while the display name is what the user actually sees.
inline constexpr wchar_t kAppInternalName[] = L"PengPDFView";
inline constexpr wchar_t kAppDisplayName[]  = L"Peng PDF View";
inline constexpr wchar_t kAppVersion[]      = L"1.1.1";
inline constexpr wchar_t kAppVendor[]       = L"Peng";

// Window class names ----------------------------------------------------------
// Each custom control registers its own class so that Windows can route mouse
// and paint messages to the right window procedure.
inline constexpr wchar_t kMainWindowClass[]     = L"PengPDFView.MainWindow";
inline constexpr wchar_t kToolbarClass[]        = L"PengPDFView.Toolbar";
inline constexpr wchar_t kSidebarClass[]        = L"PengPDFView.Sidebar";
inline constexpr wchar_t kPageCanvasClass[]     = L"PengPDFView.PageCanvas";
inline constexpr wchar_t kSearchBarClass[]       = L"PengPDFView.SearchBar";
inline constexpr wchar_t kDialogWindowClass[]   = L"PengPDFView.Dialog";

// Layout metrics expressed in device independent pixels (DIP).  Every value is
// multiplied by (dpi / 96) before it reaches GDI, which keeps the UI crisp on
// high-DPI monitors.
inline constexpr int kToolbarHeightDip = 46;
inline constexpr int kStatusBarHeightDip = 26;
inline constexpr int kSidebarMinWidthDip = 180;
inline constexpr int kSidebarMaxWidthDip = 520;
inline constexpr int kSidebarDefaultWidthDip = 268;
inline constexpr int kSplitterWidthDip = 5;
inline constexpr int kPageGapDip = 14;
inline constexpr int kCanvasMarginDip = 16;

// Document handling -----------------------------------------------------------
inline constexpr int  kMaxRecentFiles = 8;
inline constexpr int  kMaxOutlineDepth = 32;     // protects against cyclic /Kids
inline constexpr int  kMaxOutlineItems = 20000;  // sanity limit for huge TOCs
inline constexpr int  kMaxRenderCachePages = 12; // memory budget for page bitmaps
inline constexpr int  kMaxPageRenderWidth = 6000; // hard cap for one bitmap
// A byte budget complements the page count: one 6000 px wide A4 bitmap is about
// 200 MB, so twelve of them could exceed 2 GB.  At least the current page is
// always kept, even when a single bitmap is larger than this budget.
inline constexpr uint64_t kMaxRenderCacheBytes = 192ull * 1024ull * 1024ull;

// Zoom limits.  A zoom of 1.0 renders one PDF point as one Windows pixel
// (adjusted for the monitor DPI), i.e. 100% looks like "actual size".
inline constexpr double kMinZoom = 0.15;
inline constexpr double kMaxZoom = 8.0;

}  // namespace pengpdf
