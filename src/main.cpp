// =============================================================================
//  main.cpp
//  Application entry point.
//
//  Responsibilities kept deliberately small: initialise COM and the common
//  controls, register the custom window classes, create the main window and
//  run the message loop with the keyboard accelerator table.
// =============================================================================
#include <windows.h>

#include <commctrl.h>
#include <shellapi.h>

#include <string>

#include "Core/Config.h"
#include "Core/Log.h"
#include "Ui/MainWindow.h"
#include "Ui/PageCanvas.h"
#include "Ui/SearchBar.h"
#include "Ui/Sidebar.h"
#include "Ui/Toolbar.h"

namespace {

// Reads the optional document path from the command line.  Only the first
// argument is used; the shell passes a single file when the application is
// associated with .pdf files.
std::wstring DocumentFromCommandLine() {
    int argumentCount = 0;
    LPWSTR* arguments = ::CommandLineToArgvW(::GetCommandLineW(), &argumentCount);
    std::wstring document;
    if (arguments != nullptr) {
        if (argumentCount > 1 && arguments[1] != nullptr) {
            document = arguments[1];
        }
        ::LocalFree(arguments);
    }
    return document;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    // Per-monitor DPI awareness is declared in the manifest; the runtime call
    // is a harmless fallback for hosts that ignore the manifest.
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // The UI thread uses a single threaded apartment because the shell file
    // dialogs and drag and drop require one.  The result is ignored on
    // purpose: RPC_E_CHANGED_MODE means COM was already initialised with a
    // different model (possible when embedded), and the window can still run.
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    INITCOMMONCONTROLSEX commonControls = {};
    commonControls.dwSize = sizeof(commonControls);
    commonControls.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
    ::InitCommonControlsEx(&commonControls);

    pengpdf::LogInfo(L"PengPDFView %s starting", pengpdf::kAppVersion);

    if (!pengpdf::MainWindow::RegisterWindowClass(instance, pengpdf::kMainWindowClass) ||
        !pengpdf::Toolbar::RegisterWindowClass(instance, pengpdf::kToolbarClass) ||
        !pengpdf::Sidebar::RegisterWindowClass(instance, pengpdf::kSidebarClass) ||
        !pengpdf::PageCanvas::RegisterWindowClass(instance, pengpdf::kPageCanvasClass) ||
        !pengpdf::SearchBar::RegisterWindowClass(instance, pengpdf::kSearchBarClass)) {
        // All classes must exist before the main window is created, because
        // its WM_CREATE handler instantiates the child controls.  Failing here
        // is fatal, so report it before the first window exists.
        ::MessageBoxW(nullptr, L"The application window classes could not be registered.",
                      pengpdf::kAppDisplayName, MB_OK | MB_ICONERROR);
        ::CoUninitialize();
        return 1;
    }

    pengpdf::MainWindow mainWindow;
    if (!mainWindow.Create(instance, pengpdf::kMainWindowClass, showCommand,
                           DocumentFromCommandLine())) {
        ::MessageBoxW(nullptr, L"The main window could not be created.",
                      pengpdf::kAppDisplayName, MB_OK | MB_ICONERROR);
        ::CoUninitialize();
        return 1;
    }

    MSG message = {};
    // The loop ends on WM_QUIT (GetMessageW returns 0) or on a queue error
    // (-1).  A queue error is treated like a quit: without its message queue
    // the UI cannot continue, and there is no window left to report to.
    while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
        // Accelerators are dispatched through the main window's table; a
        // non-zero result means the key was consumed, so it must not reach
        // TranslateMessage/DispatchMessage as well.
        if (!::TranslateAcceleratorW(mainWindow.Handle(), mainWindow.Accelerators(),
                                     &message)) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
    }

    pengpdf::LogInfo(L"PengPDFView exiting with code %d", static_cast<int>(message.wParam));
    ::CoUninitialize();
    return static_cast<int>(message.wParam);
}
