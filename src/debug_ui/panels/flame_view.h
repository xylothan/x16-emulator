// Commander X16 Emulator — ImGui debugger: the Profile panel's Flame graph tab.
//
// The call tree as boxes: each routine as wide as its total time (itself plus
// everything it called), the routines it called drawn under it. A wide box
// with nothing wide beneath it is where the time is actually spent. Click to
// zoom into a box, search to highlight a routine, double-click to open it.
#pragma once

#include "../profile_report.h"

void profile_flame_draw(const prof_report_t *r);
