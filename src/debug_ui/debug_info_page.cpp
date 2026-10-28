// Commander X16 Emulator — ImGui debugger: the Settings window's Debug info
// page. See debug_info_page.h.

#include "debug_info_page.h"

#include "imgui.h"
#include "debug_ui_widgets.h" // dbgui_text_width
#include "file_dialog.h"

#include <algorithm>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <time.h>
#include <vector>

extern "C" {
#include "../dbg_info.h"
#include "../dbg_load.h"
extern uint8_t *fsroot_path; // host FS root (main.c)
}

namespace {

struct Candidate {
    std::string path;
    std::string folder;
    std::string name;
    long long   size  = -1;
    time_t      mtime = 0;
};

std::vector<Candidate> s_found;
double                 s_scanned_at = -1.0;
std::string            s_msg;
char                   s_path[1024] = "";

bool
file_info(const char *path, long long *size, time_t *mtime)
{
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(path, &st) != 0)
        return false;
#else
    struct stat st;
    if (stat(path, &st) != 0)
        return false;
#endif
    *size  = (long long)st.st_size;
    *mtime = (time_t)st.st_mtime;
    return true;
}

std::string
folder_of(const std::string &p)
{
    const size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? std::string() : p.substr(0, s);
}

std::string
name_of(const std::string &p)
{
    const size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

// Paths compare equal whatever their slashes or (on Windows) case.
bool
same_path(const std::string &a, const std::string &b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); i++) {
        char x = a[i] == '\\' ? '/' : a[i], y = b[i] == '\\' ? '/' : b[i];
#ifdef _WIN32
        x = (char)tolower((unsigned char)x);
        y = (char)tolower((unsigned char)y);
#endif
        if (x != y)
            return false;
    }
    return true;
}

bool
is_loaded(const std::string &p)
{
    for (int i = 0; i < dbg_info_loaded_count(); i++)
        if (dbg_info_loaded_at(i) && same_path(p, dbg_info_loaded_at(i)))
            return true;
    return false;
}

// The .dbg files worth offering: beside the loaded one, beside the -prg file,
// and in the host filesystem root. Re-read every few seconds, not every frame.
void
rescan(bool force)
{
    const double now = ImGui::GetTime();
    if (!force && s_scanned_at >= 0 && now - s_scanned_at < 3.0)
        return;
    s_scanned_at = now;
    s_found.clear();
    std::vector<std::string> dirs;
    if (dbg_info_get_dbg_dir() && dbg_info_get_dbg_dir()[0])
        dirs.push_back(dbg_info_get_dbg_dir());
    if (dbg_load_prg_path()[0])
        dirs.push_back(folder_of(dbg_load_prg_path()));
    if (fsroot_path && fsroot_path[0])
        dirs.push_back((const char *)fsroot_path);
    static char paths[64][DBG_INFO_PATH_MAX];
    for (const std::string &d : dirs) {
        const int n = dbg_info_scan_dbg_files(d.c_str(), paths, 64);
        for (int i = 0; i < n; i++) {
            bool dup = false;
            for (const Candidate &c : s_found)
                dup = dup || same_path(c.path, paths[i]);
            if (dup)
                continue;
            Candidate c;
            c.path   = paths[i];
            c.folder = folder_of(c.path);
            c.name   = name_of(c.path);
            file_info(c.path.c_str(), &c.size, &c.mtime);
            s_found.push_back(c);
        }
    }
    // Newest first: after a rebuild, the build just made is at the top.
    std::stable_sort(s_found.begin(), s_found.end(),
                     [](const Candidate &a, const Candidate &b) { return a.mtime > b.mtime; });
}

void
when(time_t t, char *buf, size_t n)
{
    if (!t) {
        snprintf(buf, n, "?");
        return;
    }
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    strftime(buf, n, "%Y-%m-%d %H:%M:%S", &tmv);
}

void
replace_with(const char *path)
{
    char msg[1200];
    dbg_load_replace(path, msg, sizeof msg);
    s_msg = msg;
    rescan(true);
}

// Where a file dialog starts: beside the loaded .dbg, else the program, else
// the host folder.
std::string
browse_from(void)
{
    if (dbg_info_loaded_count() > 0 && dbg_info_loaded_at(0))
        return folder_of(dbg_info_loaded_at(0));
    if (dbg_load_prg_path()[0])
        return folder_of(dbg_load_prg_path());
    if (fsroot_path && fsroot_path[0])
        return (const char *)fsroot_path;
    return std::string();
}

} // namespace

void
debug_info_draw_page(void)
{
    rescan(false);

    // What is loaded.
    const int nl = dbg_info_loaded_count();
    if (!dbg_info_is_loaded() || nl == 0) {
        ImGui::TextDisabled("No debug info is loaded.");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Load a .dbg below. Its folder becomes a source folder, so source files beside "
                            "it are found with nothing else to set up.");
        ImGui::PopTextWrapPos();
    } else {
        ImGui::Text("Loaded: %d source files, %d symbols, from", dbg_info_file_count(), dbg_info_symbol_count());
        for (int i = 0; i < nl; i++) {
            const char *p = dbg_info_loaded_at(i);
            long long   size;
            time_t      mt;
            char        w[32] = "?";
            if (p && file_info(p, &size, &mt))
                when(mt, w, sizeof w);
            ImGui::BulletText("%s", p ? p : "?");
            ImGui::SameLine();
            ImGui::TextDisabled("(modified %s)", w);
        }
        if (nl > 1) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.3f, 1.0f),
                               "%d files are merged. That is right for a program and the overlays it loads, but "
                               "if two are builds of the same program they describe the same addresses "
                               "differently, and names and source lines come from whichever wins. The emulator "
                               "also reads the .dbg beside each program the machine LOADs (-no-dbgauto turns "
                               "that off), which can merge a second build's file on top of -dbgfile's. Use "
                               "the one that matches the running build below.",
                               nl);
            ImGui::PopTextWrapPos();
        }
        if (ImGui::Button("Reload")) {
            // Read the same file again, for after a rebuild.
            std::string first = dbg_info_loaded_at(0) ? dbg_info_loaded_at(0) : "";
            replace_with(first.c_str());
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Read the loaded file again from disk, replacing what is loaded:\n"
                              "for after the program was rebuilt.");
    }

    // Others found on disk.
    const bool any = dbg_info_is_loaded() && nl > 0;
    ImGui::SeparatorText(any ? "Replace with" : "Load");
    if (any)
        ImGui::TextWrapped("Replacing throws away all the debug info loaded now and loads only the file "
                           "chosen. Use it when the wrong build's .dbg was loaded: names, source lines and "
                           "source breakpoints all come from it.");
    if (s_found.empty()) {
        ImGui::TextDisabled("No other .dbg files beside the loaded one, the program, or the host folder.");
    } else if (ImGui::BeginTable("dbgfiles", 4,
                                 ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                     ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Modified");
        ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        int i = 0;
        for (const Candidate &c : s_found) {
            ImGui::PushID(i++);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(c.name.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", c.path.c_str());
            ImGui::TableSetColumnIndex(1);
            char w[32];
            when(c.mtime, w, sizeof w);
            ImGui::TextUnformatted(w);
            ImGui::TableSetColumnIndex(2);
            if (c.size >= 0)
                ImGui::Text("%lld KB", (c.size + 1023) / 1024);
            ImGui::TableSetColumnIndex(3);
            if (is_loaded(c.path)) {
                ImGui::TextDisabled("loaded");
            } else if (ImGui::SmallButton("Use this")) {
                replace_with(c.path.c_str());
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::SmallButton("Look again"))
        rescan(true);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Search the folders again for .dbg files.");

    ImGui::Spacing();
    if (file_dialog_available()) {
        if (ImGui::Button("Browse for a .dbg file...")) {
            char              picked[1024];
            const std::string from = browse_from();
            if (file_dialog_open("Load debug info", "cc65 debug info (*.dbg)", "*.dbg", from.c_str(), picked,
                                 sizeof picked)) {
                snprintf(s_path, sizeof s_path, "%s", picked);
                replace_with(picked);
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Pick a file and load it in place of what is loaded now.");
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Other file");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-dbgui_text_width("Use this file") - ImGui::GetStyle().FramePadding.x * 2 -
                            ImGui::GetStyle().ItemSpacing.x);
    const bool enter = ImGui::InputTextWithHint("##dbgpath", "C:\\path\\to\\program.dbg", s_path, sizeof s_path,
                                                ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::BeginDisabled(!s_path[0]);
    if (ImGui::Button("Use this file") || (enter && s_path[0]))
        replace_with(s_path);
    ImGui::EndDisabled();

    if (!s_msg.empty()) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", s_msg.c_str());
        ImGui::PopTextWrapPos();
    }

    ImGui::Spacing();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("After a replace, breakpoints set by clicking a source line stay at their addresses; "
                        "a debug adapter's source breakpoints are resolved again. The last profile recording "
                        "is named again from the new file, waterfall included; a saved run that was opened "
                        "keeps the names it was saved with.");
    ImGui::PopTextWrapPos();
}
