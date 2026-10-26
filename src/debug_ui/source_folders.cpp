// Commander X16 Emulator — ImGui debugger: where to look for source files.
// See source_folders.h.

#include "source_folders.h"

#include "imgui.h"
#include "debug_ui_bridge.h"   // dbg_info_*
#include "debug_ui_settings.h"
#include "debug_ui_widgets.h"  // dbgui_field_width

#include "../source_view.h"

#include <algorithm>
#include <chrono>
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

// Folders searched when looking under a root. The search runs a little each
// frame, so these bound how long it can take, not how long the UI stalls.
constexpr int SEARCH_DEPTH = 16;
constexpr int SEARCH_DIRS  = 50000;

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

namespace {

source_search_t *s_search = nullptr;
std::string      s_status;
std::string      s_search_root;
int              s_ticked_frame = -1;

void
search_summary(void)
{
	const int found = source_search_found(s_search), want = source_search_count(s_search);
	// Each folder a file turned up in, once.
	std::vector<std::string> dirs;
	for (int i = 0; i < want; i++) {
		const char *d = source_search_dir_of(s_search, i);
		if (d && std::find(dirs.begin(), dirs.end(), std::string(d)) == dirs.end())
			dirs.push_back(d);
	}
	for (const std::string &d : dirs)
		source_folders_add(d.c_str(), true);
	char msg[700];
	if (found == 0)
		snprintf(msg, sizeof msg, "Not found under %s (read %d folders%s).", s_search_root.c_str(),
		         source_search_dirs_read(s_search), source_search_limited(s_search) ? ", the most it reads" : "");
	else
		snprintf(msg, sizeof msg, "Found %d of %d file%s in %d folders read; added %d folder%s.%s", found, want,
		         want == 1 ? "" : "s", source_search_dirs_read(s_search), (int)dirs.size(),
		         dirs.size() == 1 ? "" : "s",
		         source_search_limited(s_search) ? " Stopped at the folder limit: point it at a smaller tree for the rest." : "");
	s_status = msg;
	source_search_end(s_search);
	s_search = nullptr;
}

} // namespace

bool
source_folders_search_start(const char *root, const char *only)
{
	source_folders_search_cancel();
	if (!root || !root[0]) {
		s_status = "Type a folder to search first.";
		return false;
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
		s_status = "Nothing is missing: every source file is already found.";
		return false;
	}
	std::vector<const char *> names;
	for (const std::string &w : want)
		names.push_back(w.c_str());
	s_search = source_search_begin(root, names.data(), (int)names.size(), SEARCH_DEPTH, SEARCH_DIRS);
	if (!s_search) {
		s_status = "Could not start the search (out of memory?).";
		return false;
	}
	s_search_root = root;
	s_status      = "Searching...";
	return true;
}

void
source_folders_search_cancel(void)
{
	if (!s_search)
		return;
	char msg[200];
	snprintf(msg, sizeof msg, "Search cancelled after %d folders.", source_search_dirs_read(s_search));
	source_search_end(s_search);
	s_search = nullptr;
	s_status = msg;
}

bool
source_folders_search_busy(void)
{
	return s_search != nullptr;
}

const char *
source_folders_search_status(void)
{
	return s_status.c_str();
}

void
source_folders_search_tick(void)
{
	if (!s_search || ImGui::GetFrameCount() == s_ticked_frame)
		return;
	s_ticked_frame = ImGui::GetFrameCount();
	// A few milliseconds a frame: the debugger stays responsive and the
	// machine keeps its speed however big the tree.
	using clock    = std::chrono::steady_clock;
	const auto end = clock::now() + std::chrono::milliseconds(6);
	bool       more = true;
	while (more && clock::now() < end)
		more = source_search_step(s_search, 1);
	if (!more) {
		search_summary();
		return;
	}
	char msg[200];
	snprintf(msg, sizeof msg, "Searching: %d folders read, %d of %d found...", source_search_dirs_read(s_search),
	         source_search_found(s_search), source_search_count(s_search));
	s_status = msg;
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
	ImGui::BeginDisabled(source_folders_search_busy());
	if (ImGui::Button("Find missing files under this folder"))
		source_folders_search_start(s_folder, nullptr);
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Look through this folder and its subfolders for every source file the\n"
		                  "debug info names that has not been found yet, and add the folders\n"
		                  "they are in. Point it at your project's top folder.");
	if (source_folders_search_busy()) {
		ImGui::SameLine();
		if (ImGui::Button("Cancel##srcsearch"))
			source_folders_search_cancel();
	}
	if (source_folders_search_status()[0])
		ImGui::TextDisabled("%s", source_folders_search_status());
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
