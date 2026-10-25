// Commander X16 Emulator — ImGui debugger: where to look for source files.
// See source_folders.h.

#include "source_folders.h"

#include "imgui.h"
#include "debug_ui_bridge.h"   // dbg_info_*
#include "debug_ui_settings.h"
#include "debug_ui_widgets.h"  // dbgui_field_width

#include "../source_view.h"

#include <algorithm>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

namespace {

char        s_folder[512] = "";
bool        s_seeded = false;
std::string s_msg;

// The debug info's files not found yet, recomputed only when something that
// could change the answer happens -- each check touches the disk once per
// search folder, too much to repeat every frame.
std::vector<std::string> s_missing;
int                      s_files = -1;   // dbg_info_file_count() when computed; -1 = stale
double                   s_checked_at = 0.0;

void
refresh_missing(bool force)
{
	const int files = dbg_info_file_count();
	const double now = ImGui::GetTime();
	if (!force && files == s_files && now - s_checked_at < 5.0)
		return;
	s_missing.clear();
	for (int i = 0; i < files; i++) {
		const char *nm = nullptr;
		if (dbg_info_file_at(i, &nm) && nm && !source_view_can_find(nm))
			s_missing.push_back(nm);
	}
	s_files      = files;
	s_checked_at = now;
}

// Folders searched when looking under a root: deep enough for a project tree,
// small enough that a click never stalls the debugger for long.
constexpr int SEARCH_DEPTH = 10;
constexpr int SEARCH_DIRS  = 5000;

bool
saved(const char *dir)
{
	const auto &v = debug_ui_settings().source_folders;
	return std::find(v.begin(), v.end(), std::string(dir)) != v.end();
}

void
forget(const char *dir)
{
	auto &v = debug_ui_settings().source_folders;
	v.erase(std::remove(v.begin(), v.end(), std::string(dir)), v.end());
	debug_ui_settings_mark_dirty();
}

void
seed_folder(void)
{
	if (s_seeded)
		return;
	s_seeded = true;
	const char *d = dbg_info_get_dbg_dir();
	snprintf(s_folder, sizeof s_folder, "%s", d ? d : "");
}

} // namespace

void
source_folders_add(const char *dir, bool remember)
{
	if (!dir || !dir[0])
		return;
	source_view_add_path(dir);
	source_view_retry_missing();
	s_files = -1; // the missing list is stale
	if (remember && !saved(dir)) {
		debug_ui_settings().source_folders.push_back(dir);
		debug_ui_settings_mark_dirty();
	}
}

int
source_folders_find_under(const char *root, const char *only, char *msg, int msg_size)
{
	if (!root || !root[0]) {
		snprintf(msg, (size_t)msg_size, "Type a folder to search first.");
		return 0;
	}
	// The files to look for: the one asked about, or every file the debug info
	// names that is not found yet.
	std::vector<std::string> want;
	if (only) {
		want.push_back(only);
	} else {
		for (int i = 0; i < dbg_info_file_count(); i++) {
			const char *nm = nullptr;
			if (dbg_info_file_at(i, &nm) && nm && !source_view_can_find(nm))
				want.push_back(nm);
		}
	}
	if (want.empty()) {
		snprintf(msg, (size_t)msg_size, "Nothing is missing: every source file is already found.");
		return 0;
	}
	int found = 0, added = 0;
	for (const std::string &nm : want) {
		// An earlier hit's folder may already have brought this one in.
		if (source_view_can_find(nm.c_str())) {
			found++;
			continue;
		}
		char dir[1024];
		if (source_view_find_under(root, nm.c_str(), SEARCH_DEPTH, SEARCH_DIRS, dir, sizeof dir)) {
			source_folders_add(dir, true);
			added++;
			found++;
		}
	}
	if (found == 0)
		snprintf(msg, (size_t)msg_size, "Not found under %s (searched %d folders deep, up to %d folders).",
		         root, SEARCH_DEPTH, SEARCH_DIRS);
	else
		snprintf(msg, (size_t)msg_size, "Found %d of %d file%s; added %d folder%s.", found, (int)want.size(),
		         want.size() == 1 ? "" : "s", added, added == 1 ? "" : "s");
	return found;
}

void
debug_ui_open_source_folders(void)
{
	debug_ui_open_settings("Source folders");
}

void
source_folders_draw_page(void)
{
	seed_folder();
	ImGui::TextWrapped("The debug info names each source file. The debugger looks for it at that "
	                   "path first, then in each folder below (top first), then in the emulator's "
	                   "working directory. Folders you add here are saved and searched again next "
	                   "time.");
	ImGui::Separator();

	// The search list.
	const int n = source_view_path_count();
	if (n == 0)
		ImGui::TextDisabled("No folders yet: only the path in the debug info and the working directory "
		                    "are searched.");
	std::string remove;
	if (n > 0 && ImGui::BeginTable("srcfolders", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
		ImGui::TableSetupColumn("Folder", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("From", ImGuiTableColumnFlags_WidthFixed, dbgui_text_width("this run only") + 8);
		ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, dbgui_text_width("Remove") + 16);
		ImGui::TableHeadersRow();
		for (int i = 0; i < n; i++) {
			const char *p = source_view_path_at(i);
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(p);
			ImGui::TableSetColumnIndex(1);
			if (saved(p)) {
				ImGui::TextUnformatted("saved");
			} else {
				ImGui::TextDisabled("this run only");
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("From -srcpath or the debug info's own folder: added at startup,\n"
					                  "not saved.");
			}
			ImGui::TableSetColumnIndex(2);
			ImGui::PushID(i);
			if (ImGui::SmallButton("Remove"))
				remove = p;
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	if (!remove.empty()) {
		source_view_remove_path(remove.c_str());
		forget(remove.c_str());
		source_view_invalidate(); // files found through it are looked up again
		s_files = -1;
		s_msg   = "Removed " + remove + ".";
	}

	// Adding.
	ImGui::Spacing();
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("Folder");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(-1.0f);
	ImGui::InputTextWithHint("##srcfolder", "C:\\path\\to\\your\\project", s_folder, sizeof s_folder);
	if (ImGui::Button("Add this folder")) {
		source_folders_add(s_folder, true);
		s_msg = std::string("Added ") + s_folder + ".";
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Search this exact folder for source files (not its subfolders).");
	ImGui::SameLine();
	if (ImGui::Button("Find missing files under this folder")) {
		char msg[600];
		source_folders_find_under(s_folder, nullptr, msg, sizeof msg);
		s_msg = msg;
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Look through this folder and its subfolders for every source file the\n"
		                  "debug info names that has not been found yet, and add the folders\n"
		                  "they are in. Point it at your project's top folder.");
	if (!s_msg.empty())
		ImGui::TextDisabled("%s", s_msg.c_str());

	// What the debug info names, and how much of it is found.
	ImGui::Separator();
	refresh_missing(false);
	const int files = dbg_info_file_count();
	if (files == 0) {
		ImGui::TextDisabled("No debug info is loaded (start the emulator with -dbgfile).");
	} else {
		ImGui::Text("%d of %d source files in the debug info are found.", files - (int)s_missing.size(),
		            files);
		ImGui::SameLine();
		if (ImGui::SmallButton("Refresh"))
			refresh_missing(true);
		if (!s_missing.empty() && ImGui::TreeNode("missing", "Still missing (%d)", (int)s_missing.size())) {
			for (const std::string &nm : s_missing)
				ImGui::BulletText("%s", nm.c_str());
			ImGui::TreePop();
		}
	}
}
