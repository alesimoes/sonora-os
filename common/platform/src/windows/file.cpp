// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Windows bodies of file.h (ADR-0008 §3, seam 1). Built only when the
// neutral platform is compiled for Windows; every other file.h function
// still comes from src/unsupported/file.cpp until it gets a Windows body.
#include "sonora/platform/file.h"

#include <cerrno>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace sonora::platform {

namespace {

// file.h speaks UTF-8; the narrow Win32 calls would read the ANSI code page.
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

int errno_of(DWORD win_error) {
    switch (win_error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
    case ERROR_BAD_NETPATH:
        return ENOENT;
    case ERROR_ACCESS_DENIED:
        return EACCES;
    default:
        return EIO;
    }
}

}  // namespace

bool sync_directory(const std::string& path) {
    const std::wstring wide = widen(path);
    if (wide.empty()) {
        errno = ENOENT;
        return false;
    }
    // Checked first so that the access-denied pass below can only ever
    // apply to a directory that exists, as POSIX's O_DIRECTORY would.
    const DWORD attributes = ::GetFileAttributesW(wide.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        errno = errno_of(::GetLastError());
        return false;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        errno = ENOTDIR;
        return false;
    }
    // FlushFileBuffers needs GENERIC_WRITE, and a directory opens at all
    // only with FILE_FLAG_BACKUP_SEMANTICS.
    const HANDLE h = ::CreateFileW(wide.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        // Windows grants a directory for writing only to a process with
        // the backup privilege, so an ordinary user is refused here. NTFS
        // journals its metadata: a created or renamed entry is committed
        // with the log, which is the durability fsync on a POSIX directory
        // buys. Nothing more is there to ask for (ADR-0008, 2026-11-13).
        if (err == ERROR_ACCESS_DENIED) return true;
        errno = errno_of(err);
        return false;
    }
    bool ok = ::FlushFileBuffers(h) != 0;
    DWORD err = ok ? ERROR_SUCCESS : ::GetLastError();
    // As POSIX's EINVAL: a filesystem that cannot flush a directory handle
    // (FAT, some redirectors) has its entries as durable as they will be.
    if (!ok && (err == ERROR_INVALID_FUNCTION || err == ERROR_ACCESS_DENIED)) {
        ok = true;
        err = ERROR_SUCCESS;
    }
    ::CloseHandle(h);
    if (!ok) errno = errno_of(err);
    return ok;
}

}  // namespace sonora::platform
