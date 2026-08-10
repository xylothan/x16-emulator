// Commander X16 Emulator — ImGui debugger panel registry (C++).
//
// THE KEY TO PARALLELISM: every panel lives in its own .cpp under panels/ and
// self-registers via a file-static DebugPanelRegistration object. There is NO
// central list any panel must edit — debug_ui.cpp just iterates
// debug_ui_panels(). Five sessions can therefore each own one panel file with
// zero merge conflicts.
//
// To add a panel, create panels/<name>_panel.cpp:
//
//     #include "imgui.h"
//     #include "debug_ui_panels.h"
//     // (optional, to read emulator state) #include "debug_ui_bridge.h"
//
//     static void mypanel_render(bool *p_open) {
//         if (ImGui::Begin("My Panel", p_open)) {
//             ImGui::TextUnformatted("...");
//         }
//         ImGui::End();
//     }
//     static DebugPanelRegistration s_reg("My Panel", mypanel_render, true);
//
// and add the file to CMakeLists.txt X16_SOURCES. (The five foundation panels
// are already listed, so editing their *contents* needs no CMake change.)
#ifndef DEBUG_UI_PANELS_H
#define DEBUG_UI_PANELS_H

#include <vector>

struct DebugPanel {
    const char *name;             // window title / identity
    void (*render)(bool *p_open); // draws the panel; clearing *p_open hides it
    bool open;                    // current visibility
};

// Function-LOCAL static registry. Returning a static defined inside the
// function guarantees the vector is constructed on first use — before any
// panel's file-static registration runs — which sidesteps the static
// initialization-order fiasco across translation units.
std::vector<DebugPanel> &debug_ui_panels();

// Constructing one of these (as a file-static in a panel .cpp) appends the
// panel to the registry. `open` is the initial visibility.
struct DebugPanelRegistration {
    DebugPanelRegistration(const char *name, void (*render)(bool *p_open), bool open);
};

#endif // DEBUG_UI_PANELS_H
