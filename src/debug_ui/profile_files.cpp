// Commander X16 Emulator — ImGui debugger: profile recordings as files. See
// profile_files.h.

#include "profile_files.h"

#include "imgui.h"
#include "debug_ui_panels.h"   // bring the Profile panel up after opening a run
#include "debug_ui_settings.h" // the export file name and date-stamp rule
#include "debug_ui_widgets.h"  // dbgui_text_width
#include "file_dialog.h"

#include "../export_paths.h"
#include "../profile_run.h"
#include "../profile_session.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

std::string s_status;
bool        s_open_window = false; // the Open run window (no file dialog)

struct Run {
    std::string path;
    time_t      mtime;
    long long   size;
};
std::vector<Run> s_recent;

// The runs in the export folder, newest first.
void
list_runs(std::vector<Run> &out)
{
    static char list[64][1024];
    const int   n = export_list(export_dir(EXPORT_PROFILE), PROF_RUN_EXT, &list[0][0], sizeof list[0], 64);
    out.clear();
    for (int i = 0; i < n; i++) {
        Run r{list[i], 0, -1};
#ifdef _WIN32
        struct _stat64 sb;
        if (_stat64(list[i], &sb) == 0) {
#else
        struct stat sb;
        if (stat(list[i], &sb) == 0) {
#endif
            r.mtime = (time_t)sb.st_mtime;
            r.size  = (long long)sb.st_size;
        }
        out.push_back(r);
    }
    std::sort(out.begin(), out.end(), [](const Run &a, const Run &b) { return a.mtime > b.mtime; });
}

const char *
base_of(const std::string &p)
{
    const size_t cut = p.find_last_of("/\\");
    return cut == std::string::npos ? p.c_str() : p.c_str() + cut + 1;
}

std::string
export_name(void)
{
    const std::string &n = debug_ui_settings().profile_export_name;
    return n.empty() ? std::string("profile") : n;
}

std::string
stamp(void)
{
    char         buf[32];
    const time_t now = time(nullptr);
    strftime(buf, sizeof buf, "-%Y%m%d-%H%M%S", localtime(&now));
    return buf;
}

// `name` in the export folder, made if missing. False (status set) if not.
bool
in_export_folder(const std::string &name, char *out, size_t n)
{
    if (export_path(EXPORT_PROFILE, name.c_str(), out, n))
        return true;
    s_status = std::string("Could not make the folder ") + export_dir(EXPORT_PROFILE);
    return false;
}

void
save_to(const char *path)
{
    char msg[1400];
    profile_session_save_run(path, msg, sizeof msg);
    s_status = msg;
}

void
show_profile_panel(void)
{
    for (DebugPanel &p : debug_ui_panels())
        if (strcmp(p.name, "Profile") == 0)
            p.open = true;
    ImGui::SetWindowFocus("Profile");
}

} // namespace

bool
profile_files_can_save(void)
{
    return profile_session_report() && !cap_running();
}

bool
profile_files_can_export(ProfileExport kind)
{
    const prof_report_t *r = profile_session_report();
    if (!r || cap_running())
        return false;
    switch (kind) {
        case PROFILE_EXPORT_FOLDED: return r->node_count > 2;
        case PROFILE_EXPORT_TRACE:  return profile_session_has_waterfall();
        default:                    return true;
    }
}

bool
profile_files_can_open(void)
{
    return !cap_running();
}

void
profile_files_save_run(void)
{
    char path[1100];
    // Always stamped: a run is a record of one recording, never replaced.
    if (in_export_folder(export_name() + stamp() + PROF_RUN_EXT, path, sizeof path))
        save_to(path);
}

void
profile_files_save_run_as(void)
{
    if (!file_dialog_available()) {
        profile_files_save_run();
        return;
    }
    // The export folder, made now so the dialog can start in it.
    export_make_dir(export_dir(EXPORT_PROFILE));
    const std::string name = export_name() + stamp() + PROF_RUN_EXT;
    char              path[1100];
    if (file_dialog_save("Save profile run", "Profile runs (*" PROF_RUN_EXT ")", "*" PROF_RUN_EXT,
                         PROF_RUN_EXT + 1, export_dir(EXPORT_PROFILE), name.c_str(), path, sizeof path))
        save_to(path);
}

void
profile_files_open_path(const char *path)
{
    char msg[1400];
    if (profile_session_load_run(path, msg, sizeof msg) == 0)
        show_profile_panel();
    s_status = msg;
}

void
profile_files_open_run(void)
{
    if (!file_dialog_available()) {
        s_open_window = true;
        return;
    }
    char path[1100];
    if (file_dialog_open("Open profile run", "Profile runs (*" PROF_RUN_EXT ")", "*" PROF_RUN_EXT,
                         export_dir(EXPORT_PROFILE), path, sizeof path))
        profile_files_open_path(path);
}

void
profile_files_export(ProfileExport kind)
{
    static const char *const ext[] = {".folded", ".json", ".trace.json"};
    const std::string        name  = export_name() + (debug_ui_settings().profile_export_stamp ? stamp() : "") + ext[kind];
    char                     path[1100];
    if (!in_export_folder(name, path, sizeof path))
        return;
    const int n = kind == PROFILE_EXPORT_FOLDED ? profile_session_export_folded(path)
                : kind == PROFILE_EXPORT_JSON   ? profile_session_export_json(path)
                                                : profile_session_export_trace(path);
    s_status = std::string(n < 0 ? "Could not write " : "Wrote ") + path;
}

int
profile_files_recent(int max)
{
    list_runs(s_recent);
    if ((int)s_recent.size() > max)
        s_recent.resize((size_t)max);
    return (int)s_recent.size();
}

const char *
profile_files_recent_at(int i)
{
    return (i >= 0 && i < (int)s_recent.size()) ? s_recent[(size_t)i].path.c_str() : "";
}

const char *
profile_files_status(void)
{
    return s_status.c_str();
}

void
profile_files_draw(void)
{
    if (!s_open_window)
        return;
    ImGui::SetNextWindowSize(ImVec2(dbgui_text_width("w") * 70.0f, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::Begin("Open profile run###X16OpenRun", &s_open_window,
                      ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    static std::vector<Run> found;
    if (ImGui::IsWindowAppearing())
        list_runs(found);
    ImGui::TextDisabled("Saved runs in %s:", export_dir(EXPORT_PROFILE)[0] ? export_dir(EXPORT_PROFILE)
                                                                           : "the working directory");
    ImGui::Separator();
    if (found.empty())
        ImGui::TextDisabled("  (none yet)");
    for (size_t i = 0; i < found.size(); i++) {
        char line[600];
        snprintf(line, sizeof line, "%s   (%.1f MB)", base_of(found[i].path),
                 found[i].size >= 0 ? found[i].size / 1048576.0 : 0.0);
        ImGui::PushID((int)i);
        if (ImGui::Selectable(line)) {
            profile_files_open_path(found[i].path.c_str());
            s_open_window = false;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", found[i].path.c_str());
        ImGui::PopID();
    }
    ImGui::Separator();
    static char path[1024] = "";
    ImGui::SetNextItemWidth(-dbgui_text_width("Open") - ImGui::GetStyle().FramePadding.x * 2 -
                            ImGui::GetStyle().ItemSpacing.x);
    const bool enter = ImGui::InputTextWithHint("##runpath", "or a path to a" PROF_RUN_EXT " file", path,
                                                sizeof path, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::BeginDisabled(!path[0]);
    if (ImGui::Button("Open") || (enter && path[0])) {
        profile_files_open_path(path);
        s_open_window = false;
    }
    ImGui::EndDisabled();
    ImGui::End();
}
