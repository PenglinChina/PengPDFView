// =============================================================================
//  Settings.cpp
//  Very small INI reader/writer.  The format is deliberately plain UTF-8 text
//  ("key=value" per line) so users can inspect or fix the file with Notepad.
// =============================================================================
#include "App/Settings.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string_view>
#include <vector>

#include "Core/Config.h"
#include "Core/FileUtil.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Core/Utf.h"

namespace pengpdf {
namespace {

// Overwrites the settings file in place.  Doing it with plain Win32 calls
// (instead of WritePrivateProfileString) keeps the format portable and lets
// the same code write UTF-8 paths on any system locale.
bool WriteTextFile(const std::wstring& path, const std::string& utf8) {
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const BOOL ok = ::WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()),
                                &written, nullptr);
    ::CloseHandle(file);
    return ok != FALSE;
}

// Splits on LF and drops a preceding CR, so the file may use either line
// ending.  Empty lines are preserved (the parser skips them) and a trailing
// newline yields a final empty line, which is harmless.
std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
        if (end == text.size()) {
            break;
        }
        start = end + 1;
    }
    return lines;
}

// Owning wrapper around TrimWide(): the parser needs stable strings, while
// TrimWide() deliberately returns a view into its input.
std::wstring Trim(const std::wstring& text) {
    return std::wstring(TrimWide(text));
}

}  // namespace

ZoomMode ZoomModeFromTag(const std::wstring& tag) {
    // Unknown tags (a file from a newer build, or manual editing) fall back to
    // the default mode instead of failing to load the rest of the settings.
    if (tag == L"fitPage") {
        return ZoomMode::FitPage;
    }
    if (tag == L"custom") {
        return ZoomMode::Custom;
    }
    return ZoomMode::FitWidth;
}

const wchar_t* ZoomModeTag(ZoomMode mode) {
    switch (mode) {
        case ZoomMode::FitPage:
            return L"fitPage";
        case ZoomMode::Custom:
            return L"custom";
        case ZoomMode::FitWidth:
        default:
            return L"fitWidth";
    }
}

void SettingsStore::Load() {
    std::vector<uint8_t> bytes;
    std::wstring error;
    // A missing file is the normal first-run case: keep the defaults above.
    // Intentionally not clearing m_values here, so Load() is called only once
    // (from MainWindow::Create) before the UI reads any value.
    if (!ReadFileBytes(SettingsFilePath(), bytes, &error)) {
        return;
    }

    const std::string text(bytes.begin(), bytes.end());
    // Forgiving parser: unknown keys, stray lines and '#'/';' comments are
    // ignored, which keeps old setting files usable after an upgrade.
    for (const std::string& rawLine : SplitLines(text)) {
        const std::wstring line = Trim(Utf8ToWide(rawLine));
        if (line.empty() || line[0] == L'#' || line[0] == L';') {
            continue;
        }
        const size_t separator = line.find(L'=');
        if (separator == std::wstring::npos) {
            continue;
        }
        const std::wstring key = Trim(line.substr(0, separator));
        const std::wstring value = Trim(line.substr(separator + 1));

        // Zoom and sidebar width are clamped to the ranges from Config.h so
        // that a corrupt or hand-edited file cannot produce an unusable view.
        // Window sizes are only forced non-negative, and the position is kept
        // exactly as written because virtual-screen coordinates are legitimately
        // negative on monitors left of or above the primary one.
        if (key == L"language") {
            m_values.languageTag = value;
        } else if (key == L"zoomMode") {
            m_values.zoomMode = ZoomModeFromTag(value);
        } else if (key == L"zoom") {
            m_values.zoom = std::clamp(_wtof(value.c_str()), kMinZoom, kMaxZoom);
        } else if (key == L"sidebar") {
            m_values.sidebarVisible = value != L"0";
        } else if (key == L"sidebarWidth") {
            m_values.sidebarWidth = std::clamp(_wtoi(value.c_str()), kSidebarMinWidthDip,
                                               kSidebarMaxWidthDip);
        } else if (key == L"windowX") {
            m_values.window.x = _wtoi(value.c_str());
        } else if (key == L"windowY") {
            m_values.window.y = _wtoi(value.c_str());
        } else if (key == L"windowWidth") {
            m_values.window.width = std::max(0, _wtoi(value.c_str()));
        } else if (key == L"windowHeight") {
            m_values.window.height = std::max(0, _wtoi(value.c_str()));
        } else if (key == L"maximized") {
            m_values.window.maximized = value == L"1";
        } else if (key.rfind(L"recent", 0) == 0) {
            // The index in "recentN" is not used: entries keep the order in
            // which they appear in the file.  Duplicates (possible after an
            // interrupted save) are dropped.
            if (!value.empty() &&
                std::find(m_values.recentFiles.begin(), m_values.recentFiles.end(), value) ==
                    m_values.recentFiles.end()) {
                m_values.recentFiles.push_back(value);
            }
        }
    }

    if (m_values.recentFiles.size() > static_cast<size_t>(kMaxRecentFiles)) {
        m_values.recentFiles.resize(static_cast<size_t>(kMaxRecentFiles));
    }
}

void SettingsStore::Save() {
    // The whole file is regenerated on every save, so comments and keys added
    // by hand (or by a newer build) are discarded.  That is fine for a private
    // preferences file and keeps the writer trivial.
    std::string output;
    output += "# PengPDFView settings\r\n";
    output += "language=" + WideToUtf8(m_values.languageTag) + "\r\n";
    output += "zoomMode=" + WideToUtf8(ZoomModeTag(m_values.zoomMode)) + "\r\n";
    output += "zoom=" + WideToUtf8(Format(L"%.3f", m_values.zoom)) + "\r\n";
    output += std::string("sidebar=") + (m_values.sidebarVisible ? "1" : "0") + "\r\n";
    output += "sidebarWidth=" + std::to_string(m_values.sidebarWidth) + "\r\n";
    output += "windowX=" + std::to_string(m_values.window.x) + "\r\n";
    output += "windowY=" + std::to_string(m_values.window.y) + "\r\n";
    output += "windowWidth=" + std::to_string(m_values.window.width) + "\r\n";
    output += "windowHeight=" + std::to_string(m_values.window.height) + "\r\n";
    output += std::string("maximized=") + (m_values.window.maximized ? "1" : "0") + "\r\n";
    for (size_t index = 0; index < m_values.recentFiles.size(); ++index) {
        output += "recent" + std::to_string(index) + "=" +
                  WideToUtf8(m_values.recentFiles[index]) + "\r\n";
    }

    if (!WriteTextFile(SettingsFilePath(), output)) {
        LogError(L"Cannot write the settings file %s", SettingsFilePath().c_str());
    }
}

void SettingsStore::AddRecentFile(const std::wstring& path) {
    if (path.empty()) {
        return;
    }
    // Remove the previous occurrence first so re-opening a document promotes
    // it instead of duplicating it, then insert at the front.
    m_values.recentFiles.erase(
        std::remove(m_values.recentFiles.begin(), m_values.recentFiles.end(), path),
        m_values.recentFiles.end());
    m_values.recentFiles.insert(m_values.recentFiles.begin(), path);
    if (m_values.recentFiles.size() > static_cast<size_t>(kMaxRecentFiles)) {
        m_values.recentFiles.resize(static_cast<size_t>(kMaxRecentFiles));
    }
}

void SettingsStore::ClearRecentFiles() {
    m_values.recentFiles.clear();
}

}  // namespace pengpdf
