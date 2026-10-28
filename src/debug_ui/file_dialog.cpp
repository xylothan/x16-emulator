// Commander X16 Emulator — ImGui debugger: the host's "open a file" and
// "save a file" dialogs. See file_dialog.h.

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
#include <wchar.h>

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

// "what\0pattern\0All files\0*.*\0\0": the filter list is one string of
// pairs, each part ended by a NUL, the list by another.
std::wstring
filter_of(const char *what, const char *pattern)
{
    std::wstring filter = widen(what);
    filter.push_back(L'\0');
    filter += widen(pattern);
    filter.push_back(L'\0');
    filter += L"All files";
    filter.push_back(L'\0');
    filter += L"*.*";
    filter.push_back(L'\0');
    filter.push_back(L'\0');
    return filter;
}

std::wstring
backslashed(const char *path)
{
    std::wstring w = widen(path);
    for (wchar_t &c : w)
        if (c == L'/')
            c = L'\\';
    return w;
}

// One dialog, open or save.
bool
run_dialog(bool save, const char *title, const char *what, const char *pattern, const char *ext,
           const char *start_dir, const char *name, char *out, size_t out_size)
{
    if (!out || out_size == 0)
        return false;
    const std::wstring filter  = filter_of(what, pattern);
    const std::wstring dir     = backslashed(start_dir);
    const std::wstring caption = widen(title);
    const std::wstring defext  = widen(ext);

    wchar_t file[4096] = L"";
    if (name && name[0]) {
        const std::wstring n = widen(name);
        wcsncpy(file, n.c_str(), sizeof file / sizeof file[0] - 1);
    }
    OPENFILENAMEW ofn   = {};
    ofn.lStructSize     = sizeof ofn;
    ofn.hwndOwner       = GetActiveWindow();
    ofn.lpstrFilter     = filter.c_str();
    ofn.lpstrFile       = file;
    ofn.nMaxFile        = (DWORD)(sizeof file / sizeof file[0]);
    ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
    ofn.lpstrTitle      = caption.empty() ? nullptr : caption.c_str();
    ofn.lpstrDefExt     = defext.empty() ? nullptr : defext.c_str();
    // NOCHANGEDIR: the dialog otherwise moves the process's working directory,
    // which every relative path the emulator holds (ROM, -fsroot, exports)
    // is read against.
    ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY |
                (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn)))
        return false;
    return WideCharToMultiByte(CP_UTF8, 0, file, -1, out, (int)out_size, nullptr, nullptr) > 0;
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
    return run_dialog(false, title, what, pattern, nullptr, start_dir, nullptr, out, out_size);
}

bool
file_dialog_save(const char *title, const char *what, const char *pattern, const char *ext, const char *start_dir,
                 const char *name, char *out, size_t out_size)
{
    return run_dialog(true, title, what, pattern, ext, start_dir, name, out, out_size);
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

bool
file_dialog_save(const char *, const char *, const char *, const char *, const char *, const char *, char *out,
                 size_t out_size)
{
    if (out && out_size)
        out[0] = 0;
    return false;
}

#endif
