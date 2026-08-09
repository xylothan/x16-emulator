#ifndef TIMING_H
#define TIMING_H

void timing_init();
void timing_update();
void timing_update_no_sleep();
int64_t timing_lead_us();

#endif
