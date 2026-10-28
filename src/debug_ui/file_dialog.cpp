// Commander X16 Emulator — ImGui debugger: the host's "open a file" dialog.
// See file_dialog.h.

#include "file_dialog.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>

#include <string>

namespace {

std::wstring
widen(const char *s)
{
    if (!s || !s[0])
        return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1)
        return std::wstring();
    std::wstring w((size_t)(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}

} // namespace

bool
file_dialog_available(void)
{
    return true;
}

bool
file_dialog_open(const char *title, const char *what, const char *pattern, const char *start_dir, char *out,
                 size_t out_size)
{
    if (!out || out_size == 0)
        return false;
    // "what\0pattern\0All files\0*.*\0\0": the filter list is one string of
    // pairs, each part ended by a NUL, the list by another.
    std::wstring filter = widen(what);
    filter.push_back(L'\0');
    filter += widen(pattern);
    filter.push_back(L'\0');
    filter += L"All files";
    filter.push_back(L'\0');
    filter += L"*.*";
    filter.push_back(L'\0');
    filter.push_back(L'\0');

    std::wstring dir = widen(start_dir);
    for (wchar_t &c : dir)
        if (c == L'/')
            c = L'\\';
    const std::wstring caption = widen(title);

    wchar_t      file[4096] = L"";
    OPENFILENAMEW ofn       = {};
    ofn.lStructSize         = sizeof ofn;
    ofn.hwndOwner           = GetActiveWindow();
    ofn.lpstrFilter         = filter.c_str();
    ofn.lpstrFile           = file;
    ofn.nMaxFile            = (DWORD)(sizeof file / sizeof file[0]);
    ofn.lpstrInitialDir     = dir.empty() ? nullptr : dir.c_str();
    ofn.lpstrTitle          = caption.empty() ? nullptr : caption.c_str();
    // NOCHANGEDIR: the dialog otherwise moves the process's working directory,
    // which every relative path the emulator holds (ROM, -fsroot, exports)
    // is read against.
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn))
        return false;
    return WideCharToMultiByte(CP_UTF8, 0, file, -1, out, (int)out_size, nullptr, nullptr) > 0;
}

#else

bool
file_dialog_available(void)
{
    return false;
}

bool
file_dialog_open(const char *, const char *, const char *, const char *, char *out, size_t out_size)
{
    if (out && out_size)
        out[0] = 0;
    return false;
}

#endif
