// Commander X16 Emulator — ImGui debugger: the Profile panel's Waterfall tab.
//
// One captured frame as a timeline: routine spans on CPU and IRQ rows, and a
// row per device lane with a tick for every write. Scanlines along the top.
// Hover for the routine and its source line, click to open it in Source or
// Disassembly, drag to ask "what ran here". See waterfall.h for the record.
#pragma once

#include "../profile_report.h"

// Draw the tab's contents. Nothing to draw (no waterfall recorded) is handled.
void profile_waterfall_draw(const prof_report_t *r);
