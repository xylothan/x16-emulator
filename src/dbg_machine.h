// The running machine as the expression engine sees it: registers, memory and
// symbols, all read without side effects. Lives outside the DAP server because
// logpoints set from the ImGui debugger need it in a build without DAP.

#ifndef DBG_MACHINE_H
#define DBG_MACHINE_H

#include <stdint.h>

#include "dbg_expr.h"

#ifdef __cplusplus
extern "C" {
#endif

const dbg_expr_env_t *dbg_machine_env(void);

// Direct-page base + offset, matching the CPU's direct_page_add() (support.h).
// The X16 r0-r15 pseudo-registers live at D+$02..D+$21; D is 0 under the KERNAL
// ABI but can be non-zero in 65C816 native (GS) code.
uint16_t dbg_machine_dp_add(uint16_t offset);

#ifdef __cplusplus
}
#endif

#endif
