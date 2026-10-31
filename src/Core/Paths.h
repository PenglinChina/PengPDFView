// =============================================================================
//  Paths.h
//  Locations of the per-user data files.  Everything lives below
//  %APPDATA%\PengPDFView so the application never needs administrator rights.
// =============================================================================
#pragma once

#include <string>

namespace pengpdf {

// Returns (and creates on first use) the application data directory.  The
// result is cached for the process lifetime, so deleting the directory at
// runtime will not make the next call recreate it.  If Windows cannot report
// any roaming profile directory the result is empty, and the file path helpers
// below then degrade to bare file names in the current directory.
std::wstring AppDataDirectory();

// Individual configuration files inside the data directory.
std::wstring SettingsFilePath();    // settings.ini
std::wstring BookmarksFilePath();   // bookmarks.tsv
std::wstring LogFilePath();         // pengpdfview.log

// Creates `directory` when it does not exist yet.  Only a single level is
// created - the parent is assumed to exist (true for %APPDATA%) - so this is
// not a recursive "mkdir -p".  Returns true when the directory exists
// afterwards, including the "already existed" case.
bool EnsureDirectory(const std::wstring& directory);

}  // namespace pengpdf
