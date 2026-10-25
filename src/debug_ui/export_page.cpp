// Commander X16 Emulator — ImGui debugger: the Settings window's Export
// folders page. See export_page.h.

#include "export_page.h"

#include "imgui.h"
#include "debug_ui_settings.h"
#include "debug_ui_widgets.h" // dbgui_text_width

#include "../export_paths.h"

#include <stdio.h>
#include <string.h>
#include <string>
#ifdef _WIN32
#include <direct.h>
#define getcwd _getcwd
#else
#include <unistd.h>
#endif

void
debug_ui_apply_export_dirs(void)
{
    const DebugUiSettings &s = debug_ui_settings();
    export_set_dir(EXPORT_PROFILE, s.export_profile_dir.c_str());
    export_set_dir(EXPORT_SCREENSHOT, s.export_screenshot_dir.c_str());
    export_set_dir(EXPORT_DUMP, s.export_dump_dir.c_str());
}

namespace {

struct Row {
    const char *what;
    const char *help;
    std::string DebugUiSettings::*field;
};

const Row ROWS[] = {
    {"Profile exports",
     "The Profile panel's Flame graph (.folded), Everything (.json) and\n"
     "Timeline (.trace.json) files.",
     &DebugUiSettings::export_profile_dir},
    {"Screenshots", "Ctrl+P in the emulator window: x16emu-<date>-<time>.png.",
     &DebugUiSettings::export_screenshot_dir},
    {"Memory dumps",
     "Ctrl+S in the emulator window, and the dump made when the CPU runs\n"
     "off the top of memory: dump.bin, then dump-1.bin, dump-2.bin and so on.",
     &DebugUiSettings::export_dump_dir},
};

// Text boxes for the folders, kept in step with the settings strings.
char        s_buf[sizeof ROWS / sizeof ROWS[0]][1024];
bool        s_seeded = false;
std::string s_msg;

} // namespace

void
export_draw_page(void)
{
    DebugUiSettings &s = debug_ui_settings();
    const int        n = (int)(sizeof ROWS / sizeof ROWS[0]);
    if (!s_seeded) {
        for (int i = 0; i < n; i++)
            snprintf(s_buf[i], sizeof s_buf[i], "%s", (s.*(ROWS[i].field)).c_str());
        s_seeded = true;
    }

    char cwd[1024] = "";
    if (!getcwd(cwd, sizeof cwd))
        cwd[0] = 0;
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("A folder left empty is the emulator's working directory%s%s. A missing folder is made "
                        "when the first file is written to it.",
                        cwd[0] ? ", now " : "", cwd);
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    if (ImGui::BeginTable("exportdirs", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("What");
        ImGui::TableSetupColumn("Folder", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, dbgui_text_width("will be made  Make it now") + 8);
        for (int i = 0; i < n; i++) {
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(ROWS[i].what);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", ROWS[i].help);
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputTextWithHint("##dir", "working directory", s_buf[i], sizeof s_buf[i])) {
                s.*(ROWS[i].field) = s_buf[i];
                debug_ui_apply_export_dirs();
                debug_ui_settings_mark_dirty();
            }
            ImGui::TableSetColumnIndex(2);
            if (!s_buf[i][0]) {
                ImGui::TextDisabled("working directory");
            } else if (export_dir_exists(s_buf[i])) {
                ImGui::TextDisabled("exists");
            } else {
                ImGui::TextDisabled("will be made");
                ImGui::SameLine();
                if (ImGui::SmallButton("Make it now")) {
                    s_msg = export_make_dir(s_buf[i]) ? std::string("Made ") + s_buf[i] + "."
                                                      : std::string("Could not make ") + s_buf[i] + ".";
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!s_msg.empty())
        ImGui::TextDisabled("%s", s_msg.c_str());

    ImGui::SeparatorText("Profile exports");
    static char name[260] = "";
    static bool editing   = false; // typed in last frame
    if (!editing && s.profile_export_name != name)
        snprintf(name, sizeof name, "%s", s.profile_export_name.c_str());
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("File name");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(dbgui_text_width("wwwwwwwwwwwwwwwwwwww"));
    if (ImGui::InputText("##profname", name, sizeof name)) {
        s.profile_export_name = name;
        debug_ui_settings_mark_dirty();
    }
    editing = ImGui::IsItemActive();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("What the Profile panel's exports are called before their extension.\n"
                          "The panel's own File name box is the same setting.");
    if (ImGui::Checkbox("Add the date and time to each export", &s.profile_export_stamp))
        debug_ui_settings_mark_dirty();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("profile-20261025-153012.json rather than profile.json, so an export\n"
                          "never replaces the one before.");
}
