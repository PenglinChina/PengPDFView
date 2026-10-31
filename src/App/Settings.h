// =============================================================================
//  Settings.h
//  Persistent user preferences stored in %APPDATA%\PengPDFView\settings.ini.
// =============================================================================
#pragma once

#include <string>
#include <vector>

namespace pengpdf {

// How the page is scaled.  FitWidth and FitPage are computed from the window
// on every layout, so the stored `zoom` factor is only used by Custom.
enum class ZoomMode {
    FitWidth,
    FitPage,
    Custom,
};

// Remembered window geometry.  A negative coordinate means "use the default".
// Values are pixels in virtual-screen coordinates taken from the normal
// (restored) placement, not from the maximised rectangle.
struct WindowPlacement {
    int x = -1;
    int y = -1;
    int width = 0;
    int height = 0;
    bool maximized = false;
};

// The complete persisted state.  It is loaded once during startup and written
// back with SettingsStore::Save(); fields are mutated through Values().
struct AppSettings {
    // Empty language tag means "follow the operating system".
    std::wstring languageTag;
    ZoomMode zoomMode = ZoomMode::FitWidth;
    double zoom = 1.0;              // 1.0 = 100%, only meaningful for Custom
    bool sidebarVisible = true;
    int sidebarWidth = 268;         // DIP, so it survives DPI changes
    WindowPlacement window;
    // Most recently opened file first; at most kMaxRecentFiles entries.
    std::vector<std::wstring> recentFiles;
};

class SettingsStore {
public:
    // Reads the file if it exists; missing or malformed entries fall back to
    // the defaults above.
    void Load();

    // Writes the current values back to disk.  Failures are logged only.
    void Save();

    // Mutable access for the UI: change fields and then call Save().  There is
    // no dirty tracking, so forgetting the Save() silently loses the change.
    AppSettings& Values() { return m_values; }
    const AppSettings& Values() const { return m_values; }

    // Moves `path` to the front of the recent file list.  Duplicates are
    // removed by exact (case sensitive) string comparison, so callers pass the
    // already normalised path; the list is capped at kMaxRecentFiles.
    void AddRecentFile(const std::wstring& path);

    void ClearRecentFiles();

private:
    AppSettings m_values;
};

// Conversion between ZoomMode and the stable tag written to settings.ini.
// Unknown tags fall back to FitWidth so that a file from a newer build (or a
// hand-edited one) still opens with a usable zoom.
ZoomMode ZoomModeFromTag(const std::wstring& tag);
const wchar_t* ZoomModeTag(ZoomMode mode);

}  // namespace pengpdf
