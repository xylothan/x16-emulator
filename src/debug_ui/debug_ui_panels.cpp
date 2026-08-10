// Commander X16 Emulator — ImGui debugger panel registry implementation.
#include "debug_ui_panels.h"

std::vector<DebugPanel> &
debug_ui_panels()
{
    static std::vector<DebugPanel> panels;
    return panels;
}

DebugPanelRegistration::DebugPanelRegistration(const char *name, void (*render)(bool *p_open), bool open)
{
    debug_ui_panels().push_back(DebugPanel{name, render, open});
}
