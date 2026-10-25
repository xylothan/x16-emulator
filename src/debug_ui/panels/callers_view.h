// Commander X16 Emulator — ImGui debugger: the Profile panel's "Callers &
// callees" tab.
//
// One routine at a time: who called it and how much of its time each caller
// caused (left); what it calls and how much each costs it (right); merged over
// every call path. Click a neighbour to move to it, like following links.
#pragma once

#include "../profile_report.h"

void profile_callers_draw(const prof_report_t *r);

// The routine the tab shows (a report row), shared by every tab so any of them
// can say "show this one". `show` also asks the panel to switch to the tab.
void profile_select_routine(int row, bool show);
int  profile_selected_routine(void);
bool profile_take_show_request(void); // true once after a select with show
