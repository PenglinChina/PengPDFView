// =============================================================================
//  FileUtil.h
//  File-system helpers shared by the parser, the settings store and the UI.
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pengpdf {

// Reads a whole file into memory.  PDF parsing works on a memory image because
// random access into the file is needed for every cross-reference entry.
//
// On failure `out` is left empty and `errorMessage` (when not null) receives a
// short, user-visible sentence; the function never throws on I/O errors.
bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& out,
                   std::wstring* errorMessage);

// File name without the directory part: "C:\docs\a.pdf" -> "a.pdf"
std::wstring GetFileName(const std::wstring& path);

// File name without directory and extension: "C:\docs\a.pdf" -> "a"
std::wstring GetFileStem(const std::wstring& path);

// Directory part without the trailing separator.
std::wstring GetDirectoryName(const std::wstring& path);

// Absolute, canonical form of a path (resolves "." and "..").  A path that the
// Win32 API cannot resolve inside the fixed buffer is returned unchanged, so a
// very long path keeps exactly the spelling the caller passed in.
std::wstring GetFullPath(const std::wstring& path);

// Stable key used as a map index for per-document data (bookmarks).
// The canonical path is lower-cased because Windows compares file names case
// insensitively; only ASCII letters are folded, matching ToLower().
std::wstring MakePathKey(const std::wstring& path);

// True only for existing files: directories and missing files both yield false.
bool FileExists(const std::wstring& path);

}  // namespace pengpdf
