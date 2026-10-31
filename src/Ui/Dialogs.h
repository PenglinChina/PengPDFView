// =============================================================================
//  Dialogs.h
//  Small owner-drawn modal dialogs.
//
//  The settings dialog is where the interface language is switched between
//  English and Simplified Chinese; the change is applied immediately so the
//  user can preview it before confirming.
//
//  Both dialogs are frameless WS_POPUP windows.  ModalDialog paints the custom
//  title bar and makes it draggable through WM_NCHITTEST; the derived class
//  only paints and hit-tests the content below the title bar.  Sizes are given
//  in device independent pixels and scaled once against the owner's DPI when
//  ShowModal starts, so the layout follows the monitor the window opens on.
// =============================================================================
#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "App/Localization.h"
#include "App/Settings.h"

namespace pengpdf {

// Base class for the application's modal dialogs.  It draws a compact custom
// title bar, runs a modal message loop and forwards painting and input to the
// derived dialog.
class ModalDialog {
public:
    virtual ~ModalDialog();

    // Creates the dialog centred over `owner`, disables the owner, runs a
    // message loop until CloseModal() is called (or the window closes), then
    // destroys the window and re-enables the owner.  Blocks the caller, so it
    // must not be called while another modal dialog is open.  Returns the
    // Accepted() result.
    bool ShowModal(HWND owner, HINSTANCE instance);

    // True when the dialog was closed through OnAccept()/the OK button rather
    // than through Cancel, Escape, the close button or WM_CLOSE.
    bool Accepted() const { return m_result; }

protected:
    // Size of the dialog in device independent pixels.
    virtual int DialogWidthDip() const = 0;
    virtual int DialogHeightDip() const = 0;
    virtual std::wstring DialogTitle() const = 0;

    // Painting and input ------------------------------------------------
    // `content` excludes the custom title bar and the one pixel border; use the
    // rectangle helpers below instead of recomputing it.  All points are in
    // client pixels, so hit tests must be DPI aware as well.
    virtual void PaintContent(HDC dc, const RECT& content) = 0;
    virtual void OnContentMouseDown(POINT) {}
    virtual void OnContentMouseMove(POINT) {}
    virtual void OnContentMouseUp(POINT) {}
    virtual void OnKeyDown(WPARAM) {}
    virtual void OnChar(wchar_t) {}
    // Default accept action: close the dialog with a positive result.  Derived
    // classes override it to validate input before closing.
    virtual void OnAccept() { CloseModal(true); }

    // Language switches immediately inside the settings dialog.
    virtual void OnLanguageChanged() {}

    // Ends the modal loop and records the result; safe to call from an input
    // handler.  The window itself is destroyed by ShowModal after the loop.
    void CloseModal(bool accepted);
    void Invalidate();
    // Enables the hand cursor over the content area.  The flag is sticky: the
    // derived dialog must clear it when the pointer leaves interactive content.
    void SetContentHandCursor(bool hand) { m_contentHandCursor = hand; }
    // Re-reads the title from DialogTitle(), used after a language switch.
    void RefreshTitleText();

    HWND Handle() const { return m_hwnd; }
    UINT Dpi() const { return m_dpi; }
    // Converts a DIP value with the DPI captured in ShowModal.
    int Scale(int dip) const;
    // Client rectangle of the dialog without its custom title bar.
    RECT ContentArea() const;
    HFONT BodyFont() const { return m_bodyFont; }
    HFONT BoldFont() const { return m_boldFont; }
    HFONT SmallFont() const { return m_smallFont; }
    HFONT TitleFont() const { return m_titleFont; }

    // Rebuilds the cached fonts after the interface language changed.
    void RefreshFonts();

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam,
                                       LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    // Paints the background, one pixel border, title text, close button and its
    // hover highlight.  Called before PaintContent() into the same back buffer.
    void PaintFrame(HDC dc, const RECT& client);
    int TitleBarHeight() const;
    RECT CloseButtonRect(const RECT& client) const;
    RECT ContentRect(const RECT& client) const;

    HWND m_hwnd = nullptr;   // null outside ShowModal, including after WM_DESTROY
    HWND m_owner = nullptr;  // parent used for centring and input disabling
    UINT m_dpi = 96;         // DPI of the owner's monitor, captured in ShowModal
    int m_widthPx = 0;       // total window size in physical pixels
    int m_heightPx = 0;
    std::wstring m_title;    // cached localised title, refreshed on language change
    bool m_running = false;  // keeps the modal message loop alive
    bool m_result = false;   // whether the dialog was accepted
    bool m_closeHovered = false;
    // Sticky hand-cursor flag driven by the derived dialog's mouse move handler.
    bool m_contentHandCursor = false;
    bool m_trackingMouse = false;  // TME_LEAVE is armed

    // Owned by the dialog: created by RefreshFonts() and released when
    // ShowModal returns.  Null only if font creation failed.
    HFONT m_titleFont = nullptr;
    HFONT m_bodyFont = nullptr;
    HFONT m_boldFont = nullptr;
    HFONT m_smallFont = nullptr;
};

// -----------------------------------------------------------------------------
//  Settings dialog
// -----------------------------------------------------------------------------
// Edits a copy of AppSettings.  Selecting a language applies it to the whole
// application immediately (so labels can be previewed); if the dialog is
// rejected, the frame calls ApplyLanguage() with the language it passed in.
class SettingsDialog : public ModalDialog {
public:
    // `currentLanguage` is the language active when the dialog opens; the
    // dialog starts from `settings` and leaves them untouched until accepted.
    SettingsDialog(const AppSettings& settings, Lang currentLanguage);

    // Valid after ShowModal returned true.
    Lang SelectedLanguage() const { return m_language; }
    const AppSettings& UpdatedSettings() const { return m_settings; }

protected:
    int DialogWidthDip() const override { return 430; }
    int DialogHeightDip() const override { return 470; }
    std::wstring DialogTitle() const override;
    void PaintContent(HDC dc, const RECT& content) override;
    void OnContentMouseDown(POINT point) override;
    void OnContentMouseMove(POINT point) override;
    void OnAccept() override;
    void OnLanguageChanged() override;

private:
    // Identifiers of the clickable rows inside the dialog.
    enum class Target {
        None,
        LanguageEnglish,
        LanguageChinese,
        ZoomFitWidth,
        ZoomFitPage,
        ZoomActualSize,
        ShowSidebar,
        ButtonCancel,
        ButtonOk,
    };

    // Hit testing is checked in the same order as the visual stacking; all
    // rectangles are absolute client coordinates derived from ContentArea().
    Target HitTest(POINT point) const;
    // Option rows are 34 DIPs tall and use the full content width minus a
    // 14 DIP margin; `index` selects the language / zoom option.
    RECT LanguageRowRect(int index) const;
    RECT ZoomRowRect(int index) const;
    // Full width row containing the sidebar checkbox; the checkbox itself is
    // drawn smaller and vertically centred inside it.
    RECT CheckboxRect() const;
    RECT OkButtonRect() const;
    RECT CancelButtonRect() const;
    // Fixed offsets from the top of the content area; the vertical rhythm is
    // hand tuned for the 470 DIP dialog rather than computed from the rows.
    int LanguageRowTop() const;
    int ZoomRowTop() const;

    AppSettings m_settings;    // edited copy, committed by OnAccept()
    Lang m_language;           // currently previewed language
    // Language active when the dialog was created; the frame restores it when
    // the dialog is rejected.
    Lang m_originalLanguage;
    Target m_hovered = Target::None;
};

// -----------------------------------------------------------------------------
//  "Go to page" dialog
// -----------------------------------------------------------------------------
// A single line numeric input.  The current page number is pre-filled and
// selected, so typing a digit replaces it while Backspace edits it normally.
// Enter (WM_KEYDOWN VK_RETURN) accepts through OnAccept().
class GoToPageDialog : public ModalDialog {
public:
    // `pageCount` is clamped to at least one so the range check in Submit() is
    // always meaningful; `currentPage` is a zero based index.
    GoToPageDialog(int pageCount, int currentPage);

    // Zero based page index selected by the user.
    int SelectedPage() const { return m_selectedPage; }

protected:
    int DialogWidthDip() const override { return 340; }
    int DialogHeightDip() const override { return 240; }
    std::wstring DialogTitle() const override;
    void PaintContent(HDC dc, const RECT& content) override;
    void OnContentMouseDown(POINT point) override;
    void OnKeyDown(WPARAM key) override;
    void OnChar(wchar_t character) override;
    void OnAccept() override;

private:
    // Geometry of the edit field and the two buttons, in client coordinates.
    RECT InputRect() const;
    RECT OkButtonRect() const;
    RECT CancelButtonRect() const;
    // Validates the digits and closes the dialog when the page is in range.
    void Submit();

    int m_pageCount = 0;    // at least 1
    int m_currentPage = 0;  // zero based, only used to seed the text
    int m_selectedPage = 0; // zero based, valid after acceptance
    std::wstring m_text;    // digits entered so far
    bool m_invalid = false; // show the red frame/error message after a bad submit
    // True while the initial page number is still selected; the first typed
    // digit then replaces it instead of appending.
    bool m_replaceOnNextChar = true;
};

}  // namespace pengpdf
