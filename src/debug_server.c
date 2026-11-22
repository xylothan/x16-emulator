// Commander X16 Emulator — Debug Adapter Protocol (DAP) Server
// Speaks DAP over TCP for direct IDE integration (Visual Studio, VS Code, etc.)
// Transport: Content-Length framed JSON over TCP, default port 9009

#include <SDL.h>
#include "debug_server.h"
#include "screen_text.h"
#include "debug_server_call.h"
#include "mcp_server.h"
#include "dbg_info.h"
#include "dbg_expr.h"
#include "dbg_log.h"
#include "dbg_machine.h"
#include "logpoint.h"
#include "call_stack.h"
#include "debugger.h"
#include "disasm.h"
#include "code_map.h"
#include "perf_budget.h"
#include "perf_hist.h"
#include "host_prof.h"
#include "profile_session.h"
#include "waterfall.h"
#include "vera_bandwidth.h"
#include "sprite_trace.h"
#include "glue.h"
#include "memory.h"
#include "source_view.h"
#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <netinet/tcp.h>
#endif
#include "video.h"
#include "keyboard.h"
#include "joystick.h"
#include "sdcard.h"
#include "sdcard_fat.h"

#include "cpu/fake6502.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <cjson/cJSON.h>
#include "x16_hash.h"
#include "savestate.h"
#include "dbg_load.h"
#include "export_paths.h"
#include "host_write_path.h"
#include "i2c.h"
#include "io_trace.h"
#include "midi.h"
#include "rtc.h"
#include "smc.h"
#include "via.h"
#include "mem_hash.h"
#include "mem_search.h"
#include "audio_events.h"
#include "audio.h"
#include "audio_decode.h"
#include "vera_psg.h"
#include "vera_pcm.h"
#include "ymglue.h"

extern bool headless;   // main.c

// Platform socket abstraction
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef SOCKET socket_t;
  #define SOCKET_INVALID INVALID_SOCKET
  #define CLOSE_SOCKET closesocket
  #define SOCK_ERRNO WSAGetLastError()
  #define WOULD_BLOCK (SOCK_ERRNO == WSAEWOULDBLOCK)
  static bool wsa_initialized = false;
#else
  #include <sys/socket.h>
  #include <sys/select.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <fcntl.h>
  #include <unistd.h>
  #include <errno.h>
  #include <signal.h>
  typedef int socket_t;
  #define SOCKET_INVALID (-1)
  #define CLOSE_SOCKET close
  #define SOCK_ERRNO errno
  #define WOULD_BLOCK (SOCK_ERRNO == EAGAIN || SOCK_ERRNO == EWOULDBLOCK)
#endif

// Receive buffer (large enough for DAP messages with headers)
#define RECV_BUF_SIZE 65536
static char recv_buf[RECV_BUF_SIZE];
static int  recv_buf_len = 0;

// Server state
static socket_t listen_sock = SOCKET_INVALID;
static socket_t client_sock = SOCKET_INVALID;
static bool     server_enabled = false;
static int      server_port = 0;

// Who the request being handled came from. While reply_sink is set, the two
// response functions store the result there instead of writing to the socket.
// Events are never sunk, so an attached editor still hears about the machine.
static dbg_caller_t   current_caller = DBG_CALLER_DAP;
static dbg_reply_t   *reply_sink = NULL;
static bool           reply_answered = false;
static dbg_frontend_t frontend = { NULL, NULL, NULL };

static bool frontend_holds_run_state(void) {
    return frontend.holds_run_state && frontend.holds_run_state();
}

static bool frontend_can_resume(void) {
    return !frontend.can_resume || frontend.can_resume();
}

// DAP sequence counter (for events we send)
static int dap_seq = 1;

// ─── Emulated time, 64-bit ──────────────────────────────────────────
//
// clockticks6502 is 32 bits and wraps after about nine minutes at 8 MHz, and
// video.c's frame_count is an int. A client measuring a long run needs neither
// to wrap, so the deltas are accumulated here into 64 bits.
//
// machine_frames is the machine's own frame count, and the cycle count is the
// machine's too: x16/loadState restores both along with the rest of the
// machine. process_frames counts every frame this process has emulated and is
// never restored, which is what a throughput figure wants.
//
// Both advance in debug_server_frame_end(), which main.c calls at the same point
// as the -input script's frame tick, so frame N here is frame N there.
static uint64_t machine_frames = 0;
static uint64_t process_frames = 0;
static uint64_t cycles_acc     = 0;
static uint32_t cycles_seen    = 0;
// clockticks6502 when the last frame ended. While paused, the machine is at a
// frame boundary if the clock has not moved since then. Conservative: an IRQ
// taken on the frame's last instruction charges its entry cycles after this is
// recorded, so a stop right there reads as mid-frame. It never reads a
// mid-frame stop as a boundary.
static uint32_t frame_end_ticks = 0;
static bool     frame_end_seen  = false;

// Fold the clock's movement since the last call into the 64-bit count. Called
// at least once per frame and once per poll, far more often than the 32-bit
// clock can wrap.
static uint64_t emulated_cycles(void) {
    const uint32_t now = clockticks6502;
    cycles_acc += (uint32_t)(now - cycles_seen);
    cycles_seen = now;
    return cycles_acc;
}

// ─── x16/step: run a set number of frames, then answer ──────────────
//
// The response is deferred. The request starts the machine; the frame-end hook
// counts frames down and stops it; and the poll that next sees the machine
// parked sends the reply. Replying from the hook itself would be too early: the
// rest of that loop pass can still take an interrupt, which pushes onto the
// stack, so reads taken there would not match a readMemory sent right after.
#define STEP_MAX_READS      64
// Base64 grows data by a third, and the whole reply has to fit the send buffer
// or the client is dropped. 32K of raw bytes leaves comfortable room.
#define STEP_MAX_READ_BYTES 32768

typedef struct {
    uint32_t addr;
    uint32_t len;
    int      bank;      // x16 RAM bank for $A000-$BFFF, or USE_CURRENT_X16_BANK
    bool     vram;
} step_read_t;

static struct {
    bool        pending;
    dbg_caller_t caller;       // who gets the deferred reply
    int         seq;
    uint32_t    frames_left;
    uint32_t    frames_run;
    bool        by_frames;     // stopped because the count ran out
    bool        events;        // the client wants stopped/continued events
    bool        hash;          // include the machine state hash in the reply
    bool        audio;         // report what the sound chips were told to play
    int         num_reads;
    step_read_t reads[STEP_MAX_READS];
} step;

static void step_finish(const char *reason);

// ─── x16/runTo: run to one place, then answer ───────────────────────
//
// A DAP reply is deferred, as for x16/step. MCP waits in its own waiter, so a
// call cancelled or timed out there leaves nothing here to answer.
#define RUN_TO_WAIT_DEFAULT 5000
#define RUN_TO_WAIT_LIMIT   30000

static struct {
    bool             pending;
    int              seq;
    Uint32           deadline;
    dbg_run_target_t target;
} run_to;

static void run_to_poll(void);
static void run_to_finish(bool completed, const char *reason);

// DAP variable reference IDs
#define VARREF_REGISTERS  1
#define VARREF_ZEROPAGE   2
#define VARREF_STACK      3
#define VARREF_VREGS      4   // X16 16-bit virtual registers R0-R15 (ZP $02-$21)

// Breakpoint tracking for DAP (maps DAP bp IDs to addresses)
#define MAX_DAP_BREAKPOINTS 256
static struct {
    int  dap_id;       // DAP breakpoint ID
    uint16_t addr;     // resolved address
    char file[256];    // source file
    int  line;         // source line
    bool verified;
    uint8_t bank;      // program bank (65816); addresses are 24-bit
    int  x16Bank;      // the selector it was armed with; a removal that passes
                       // DEBUG_BANK_ANY will not match a bank-pinned entry
    // Kept so the breakpoint can be armed again exactly as the client asked for
    // it when an overlay reload forces it to be re-resolved. Without these, a
    // reload quietly turned a bank-pinned conditional breakpoint into an
    // unconditional one that fires in every bank.
    char cond[192];
    char hit_cond[64];
    char log[DBG_LOG_MAX_MESSAGE + 1];   // empty for a breakpoint that stops
} dap_bps[MAX_DAP_BREAKPOINTS];
static int num_dap_bps = 0;
static int next_dap_bp_id = 1;

// Function/instruction breakpoints tracked separately so each re-set clears its
// own set. All three add into the same core table, which now records which of
// them asked -- so each can clear what it asked for without knowing or caring
// what anyone else wanted at the same address.
static uint16_t func_bp_addrs[128];
static uint8_t  func_bp_banks[128];
static int      func_bp_x16[128];       // bank selector it was armed with
static int      func_bp_ids[128];
static char     func_bp_names[128][64]; // kept to re-pin once the bank is known
static char     func_bp_cond[128][192]; // re-applied when a re-pin moves the entry
static char     func_bp_hit[128][64];
static int      num_func_bps = 0;
static uint16_t instr_bp_addrs[128];
static uint8_t  instr_bp_banks[128];
static int      instr_bp_x16[128];
static int      instr_bp_ids[128];      // 0 for a dump breakpoint, which never stops
static int      num_instr_bps = 0;

// Data breakpoints, so a stop can name the one that fired.
static struct {
    int      id;
    uint8_t  space;
    uint32_t addr;
} data_bps[MAX_WATCHPOINTS];
static int      num_data_bps = 0;

static bool     dap_session_active = false;
// The session began with launch, so the machine's lifetime is the client's. One
// that attached found the machine running and must leave it running.
static bool     dap_launched = false;
// The protocol allows no event until the initialize request has been answered.
static bool     dap_initialize_answered = false;
// Whether the stop the machine is currently sitting in has been announced. A
// breakpoint can hit while the client is still configuring, and configurationDone
// would then report the same halt a second time under a different reason.
static bool     dap_stop_announced = false;
// What the client said in initialize. Lines are held 1-based everywhere inside
// and converted only where they cross the protocol.
static int      dap_line_base = 1;
static int      dap_col_base  = 1;
static bool     dap_paths_are_uris = false;
// launch asked to run without debugging: no breakpoint of this session is armed.
static bool     dap_no_debug = false;

static int dap_line_out(int line) { return line - 1 + dap_line_base; }
static int dap_line_in(int line)  { return line + 1 - dap_line_base; }

// Does anyone other than this DAP session own the run state?
//
// When someone does, a client connecting or going away must not resume the
// machine: somebody is sitting at a breakpoint looking at it. When nobody does
// -- a headless -debugport run -- nothing else can ever resume it, so a session
// ending has to.
//
// Both local UIs count, and both need their own test. -imgui does not set
// debug_window_enabled, and -debugport sets debugger_enabled by itself, so
// neither flag alone answers the question: testing only the first ignored the
// graphical debugger entirely and resumed the machine out from under it.
static bool
dap_other_owner_of_run_state(void)
{
	return (debug_window_enabled && debugger_enabled)
	       || (imgui_debugger_enabled && video_debug_ui_available())
	       || (frontend_holds_run_state() && frontend_can_resume());
}

// A handler's reply can drop a DAP client, and then there is nobody left to act
// for. A reply to any other caller never touches the socket, so its absence of
// a DAP client means nothing.
static bool dap_reply_lost(void) {
    return current_caller == DBG_CALLER_DAP && client_sock == SOCKET_INVALID;
}

// Owners for what a request arms, so that each front end releases only its own.
static debug_owner_t caller_step_owner(void) {
    return current_caller == DBG_CALLER_MCP ? DEBUG_OWNER_MCP : DEBUG_OWNER_DAP_SOURCE;
}
static perf_owner_t caller_perf_owner(void) {
    return current_caller == DBG_CALLER_MCP ? PERF_OWNER_MCP : PERF_OWNER_DAP;
}
static vera_bw_owner_t caller_bw_owner(void) {
    return current_caller == DBG_CALLER_MCP ? VERA_BW_OWNER_MCP : VERA_BW_OWNER_DAP;
}
static sprite_trace_owner_t caller_sprite_owner(void) {
    return current_caller == DBG_CALLER_MCP ? SPRITE_TRACE_OWNER_MCP : SPRITE_TRACE_OWNER_DAP;
}

// A DAP request is answered by its own response. One from elsewhere leaves the
// editor believing the machine is halted, so the flag stays set and the resume
// hook sends it a continued event.
static void note_request_resumes(void) {
    if (current_caller == DBG_CALLER_DAP) dap_stop_announced = false;
}

// Forward declarations
static void send_dap_event(const char *event_name, cJSON *body);
static void send_dap_message(cJSON *json);
static int dap_apply_bp_condition(dbg_addr_t addr, const char *cond_str, const char *hit_str,
                                  int default_bank);
static int dap_cond_bank_pin(dbg_addr_t addr, const char *cond_str, int default_bank);
static bool dap_bp_can_arm(dbg_addr_t addr, int default_bank, const char *cond, const char *hit,
                           char *why, size_t whyn);
static bool dap_check_condition(const char *cond, int *bank_out, char *err, size_t errn);
static bool dap_check_hit_condition(const char *h);
static bool dap_parse_instruction_ref(const char *s, long offset, struct breakpoint *out);
static void dap_fmt_bp(char *out, size_t n, int pc, uint8_t bank, int x16Bank);

// Say which bank a breakpoint is pinned to, so a client can see it.
static void dap_add_bank_message(cJSON *bp, int x16Bank) {
    if (x16Bank == DEBUG_BANK_ANY) return;
    char msg[48];
    snprintf(msg, sizeof msg, "bank $%02X only", (unsigned)x16Bank);
    cJSON_AddStringToObject(bp, "message", msg);
}

// Source files match addresses by basename, so several entries can share one
// core entry and this owner's single claim on it. It is a logpoint only if all of
// them are: any plain breakpoint among them asked for a stop. Run after anything
// that changes the claims.
static void dap_sync_source_logs(void) {
    for (int i = 0; i < num_dap_bps; i++) {
        if (!dap_bps[i].verified || !dap_bps[i].file[0]) continue;
        const int     pc = dap_bps[i].addr, x16 = dap_bps[i].x16Bank;
        const uint8_t bank = dap_bps[i].bank;
        if (!debug_bp_has_owner(pc, bank, x16, DEBUG_OWNER_DAP_SOURCE)) continue;
        bool first = true, plain = false;
        const char *msg = NULL;
        for (int j = 0; j < num_dap_bps; j++) {
            if (!dap_bps[j].verified || !dap_bps[j].file[0] || dap_bps[j].addr != pc ||
                dap_bps[j].bank != bank || dap_bps[j].x16Bank != x16)
                continue;
            if (j < i) first = false;
            if (!dap_bps[j].log[0]) plain = true;
            else if (!msg) msg = dap_bps[j].log;
        }
        if (!first) continue;
        char why[120];
        if (plain || !msg || !logpoint_set(pc, bank, x16, DEBUG_OWNER_DAP_SOURCE, msg, why, sizeof why))
            logpoint_clear(pc, bank, x16, DEBUG_OWNER_DAP_SOURCE);
    }
}

// Re-resolve unverified breakpoints after new .dbg file loads
static void retry_unverified_breakpoints(void) {
    for (int i = 0; i < num_dap_bps; i++) {
        if (!dap_bps[i].verified) {
            dbg_addr_t addr = 0;
            int seg_bank = -1;
            if (dbg_info_source_to_addr_bank(dap_bps[i].file, dap_bps[i].line, &addr, &seg_bank)) {
                // The line has an address now, which is the first time its
                // condition can be checked against what is already there.
                char why[200];
                if (!dap_bp_can_arm(addr, seg_bank, dap_bps[i].cond, dap_bps[i].hit_cond,
                                    why, sizeof why)) {
                    if (client_sock != SOCKET_INVALID) {
                        cJSON *body = cJSON_CreateObject();
                        cJSON *bp = cJSON_CreateObject();
                        cJSON_AddNumberToObject(bp, "id", dap_bps[i].dap_id);
                        cJSON_AddBoolToObject(bp, "verified", false);
                        cJSON_AddStringToObject(bp, "message", why);
                        cJSON_AddNumberToObject(bp, "line", dap_line_out(dap_bps[i].line));
                        cJSON_AddItemToObject(body, "breakpoint", bp);
                        cJSON_AddStringToObject(body, "reason", "changed");
                        send_dap_event("breakpoint", body);
                    }
                    continue;
                }
                // Re-arm it exactly as the client asked for it. Re-applying the
                // condition also recovers the bank pin, which decides the
                // selector the core entry is keyed on -- arming with ANY here
                // turned a bank-pinned conditional breakpoint into one that
                // fires in every bank on its first arrival.
                int bank_pin = dap_apply_bp_condition(addr,
                                                      dap_bps[i].cond,
                                                      dap_bps[i].hit_cond,
                                                      seg_bank);
                dap_bps[i].addr = (uint16_t)(addr & 0xFFFF);
                dap_bps[i].bank = (uint8_t)(addr >> 16);
                dap_bps[i].verified = true;
                dap_bps[i].x16Bank = bank_pin;
                struct breakpoint hw_bp;
                hw_bp.pc = (int)(addr & 0xFFFF);
                hw_bp.bank = (uint8_t)(addr >> 16);
                hw_bp.x16Bank = bank_pin;
                debug_bp_add_for(hw_bp, DEBUG_OWNER_DAP_SOURCE);
                printf("[dap] Resolved pending breakpoint: %s:%d -> $%04X\n",
                       dap_bps[i].file, dap_bps[i].line, (unsigned)addr);
                // Say so. This is reached after invalidate_breakpoints_in_range()
                // has told the client the breakpoint is unverified, so staying
                // quiet would leave it showing one that is in fact armed.
                if (client_sock != SOCKET_INVALID) {
                    cJSON *body = cJSON_CreateObject();
                    cJSON *bp = cJSON_CreateObject();
                    cJSON_AddNumberToObject(bp, "id", dap_bps[i].dap_id);
                    cJSON_AddBoolToObject(bp, "verified", true);
                    cJSON_AddNumberToObject(bp, "line", dap_line_out(dap_bps[i].line));
                    dap_add_bank_message(bp, bank_pin);
                    cJSON_AddItemToObject(body, "breakpoint", bp);
                    cJSON_AddStringToObject(body, "reason", "changed");
                    send_dap_event("breakpoint", body);
                }
            }
        }
    }
    dap_sync_source_logs();
}

// Invalidate breakpoints whose addresses fall within a range being unloaded
static void invalidate_breakpoints_in_range(uint32_t start, uint32_t end) {
    // Compared as full 24-bit addresses, program bank included. Clamping the
    // range to 16 bits and comparing only the low half meant reloading a
    // bank-1 module left its breakpoints armed and stale, while reloading a
    // bank-0 range invalidated unrelated breakpoints in every other bank.
    for (int i = 0; i < num_dap_bps; i++) {
        const uint32_t bp_addr =
            ((uint32_t)dap_bps[i].bank << 16) | dap_bps[i].addr;
        if (dap_bps[i].verified && bp_addr >= start && bp_addr <= end) {
            // Drop this session's claim on the core entry. It goes only if
            // nothing else wants it, so a -bp or an F9 breakpoint at the same
            // address survives a module reload that has nothing to do with it.
            debug_bp_remove_for(dap_bps[i].addr, dap_bps[i].bank, dap_bps[i].x16Bank,
                                DEBUG_OWNER_DAP_SOURCE);

            dap_bps[i].addr = 0;
            dap_bps[i].bank = 0;
            printf("[dap] Invalidated breakpoint: %s:%d (range $%06X-$%06X unloaded)\n",
                   dap_bps[i].file, dap_bps[i].line, start, end);

            // Notify VS that breakpoint is no longer verified
            if (client_sock != SOCKET_INVALID) {
                cJSON *body = cJSON_CreateObject();
                cJSON *bp = cJSON_CreateObject();
                cJSON_AddNumberToObject(bp, "id", dap_bps[i].dap_id);
                cJSON_AddBoolToObject(bp, "verified", false);
                cJSON_AddStringToObject(bp, "message", "Module unloaded");
                cJSON_AddNumberToObject(bp, "line", dap_line_out(dap_bps[i].line));
                cJSON_AddItemToObject(body, "breakpoint", bp);
                cJSON_AddStringToObject(body, "reason", "changed");
                send_dap_event("breakpoint", body);
            }
        }
    }
}

static void send_bp_changed(int id, int line, int x16Bank) {
    if (client_sock == SOCKET_INVALID) return;
    cJSON *body = cJSON_CreateObject();
    cJSON *bp = cJSON_CreateObject();
    cJSON_AddNumberToObject(bp, "id", id);
    cJSON_AddBoolToObject(bp, "verified", true);
    if (line > 0) cJSON_AddNumberToObject(bp, "line", dap_line_out(line));
    dap_add_bank_message(bp, x16Bank);
    cJSON_AddItemToObject(body, "breakpoint", bp);
    cJSON_AddStringToObject(body, "reason", "changed");
    send_dap_event("breakpoint", body);
}

// Move one owner's claim from one bank selector to another. The old key's
// condition record goes too unless another breakpoint still uses it.
static void move_bp_bank(int pc, uint8_t pb, int from, int to, debug_owner_t owner) {
    debug_bp_remove_for(pc, pb, from, owner);
    if (debug_bp_find(pc, pb, from) < 0) debug_bp_forget(pc, pb, from);
    struct breakpoint hw = { pc, pb, to, 0, false };
    debug_bp_add_for(hw, owner);
}

// Source and function breakpoints are resolved as soon as the .dbg arrives,
// which is before the LOAD that shows which RAM bank their code went into, so
// they were armed for any bank. Re-pin them once the bank is known.
static void rebank_breakpoints(void) {
    for (int i = 0; i < num_dap_bps; i++) {
        if (!dap_bps[i].verified || !dap_bps[i].file[0]) continue;
        dbg_addr_t addr;
        int seg_bank = -1;
        if (!dbg_info_source_to_addr_bank(dap_bps[i].file, dap_bps[i].line, &addr, &seg_bank))
            continue;
        const int     pc = (int)(addr & 0xFFFF);
        const uint8_t pb = (uint8_t)(addr >> 16);
        if (pc != dap_bps[i].addr || pb != dap_bps[i].bank) continue;
        const int pin = dap_cond_bank_pin(addr, dap_bps[i].cond, seg_bank);
        if (pin == dap_bps[i].x16Bank) continue;
        move_bp_bank(pc, pb, dap_bps[i].x16Bank, pin, DEBUG_OWNER_DAP_SOURCE);
        dap_apply_bp_condition(addr, dap_bps[i].cond, dap_bps[i].hit_cond, seg_bank);
        dap_bps[i].x16Bank = pin;
        send_bp_changed(dap_bps[i].dap_id, dap_bps[i].line, pin);
    }
    for (int i = 0; i < num_func_bps; i++) {
        dbg_addr_t addr;
        int seg_bank = -1;
        if (!dbg_info_label_to_addr_bank(func_bp_names[i], &addr, &seg_bank)) continue;
        const int     pc = (int)(addr & 0xFFFF);
        const uint8_t pb = (uint8_t)(addr >> 16);
        if (pc != func_bp_addrs[i] || pb != func_bp_banks[i]) continue;
        const int pin = dap_cond_bank_pin(addr, func_bp_cond[i], seg_bank);
        if (pin == func_bp_x16[i]) continue;
        move_bp_bank(pc, pb, func_bp_x16[i], pin, DEBUG_OWNER_DAP_FUNCTION);
        // The move left the condition behind on the old key.
        dap_apply_bp_condition(addr, func_bp_cond[i], func_bp_hit[i], seg_bank);
        func_bp_x16[i] = pin;
        send_bp_changed(func_bp_ids[i], 0, pin);
    }
    dap_sync_source_logs();
}

void debug_server_rebank_breakpoints(void) {
    if (!server_enabled || client_sock == SOCKET_INVALID) return;
    rebank_breakpoints();
}

// Public wrappers so the shared runtime-load handler (debugger.c) can drive the
// DAP-only breakpoint bookkeeping around a module swap. Both are no-ops when no
// DAP client is connected, so the native source window works without DAP.
void debug_server_invalidate_breakpoints_in_range(uint32_t start, uint32_t end) {
    if (!server_enabled || client_sock == SOCKET_INVALID) return;
    invalidate_breakpoints_in_range(start, end);
}

void debug_server_retry_unverified_breakpoints(void) {
    if (!server_enabled || client_sock == SOCKET_INVALID) return;
    retry_unverified_breakpoints();
}

// ─── Platform helpers ───────────────────────────────────────────────

static bool set_nonblocking(socket_t sock) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(sock, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(sock, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

static bool platform_init(void) {
#ifdef _WIN32
    if (!wsa_initialized) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            fprintf(stderr, "[dap] WSAStartup failed: %d\n", WSAGetLastError());
            return false;
        }
        wsa_initialized = true;
    }
#else
    // A client vanishing just before an event is sent has to be an error from
    // send(), which drops that client. The default action ends the emulator.
    signal(SIGPIPE, SIG_IGN);
#endif
    return true;
}

static void platform_cleanup(void) {
#ifdef _WIN32
    if (wsa_initialized) {
        WSACleanup();
        wsa_initialized = false;
    }
#endif
}

// ─── DAP message sending ────────────────────────────────────────────

static void disconnect_client(void);
static void dap_release_session_state(void);
static void dap_release_breakpoints(void);
static int  dap_refuse_breakpoints(int seq, const char *command, cJSON *args);

// See debug_server_set_no_wait().
static bool send_no_wait = false;

void debug_server_set_no_wait(bool on) {
    send_no_wait = on;
}

// Length of the valid UTF-8 sequence at p, or 0 if there is none.
static int dap_utf8_len(const unsigned char *p, const unsigned char *end) {
    const unsigned char c = p[0];
    if (c < 0x80) return 1;
    int n;
    unsigned char lo = 0x80, hi = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) {
        n = 2;
    } else if (c >= 0xE0 && c <= 0xEF) {
        n = 3;
        if (c == 0xE0) lo = 0xA0;   // overlong
        if (c == 0xED) hi = 0x9F;   // surrogates
    } else if (c >= 0xF0 && c <= 0xF4) {
        n = 4;
        if (c == 0xF0) lo = 0x90;   // overlong
        if (c == 0xF4) hi = 0x8F;   // past U+10FFFF
    } else {
        return 0;
    }
    if (end - p < n || p[1] < lo || p[1] > hi) return 0;
    for (int i = 2; i < n; i++)
        if ((p[i] & 0xC0) != 0x80) return 0;
    return n;
}

// A copy of the text with each invalid byte replaced by U+FFFD, or NULL when it
// is already valid. The protocol's content is UTF-8, and source files need not
// be. Everything structural in JSON is ASCII, so this cannot damage the framing.
static char *dap_utf8_repaired(const char *text, size_t len, size_t *out_len) {
    const unsigned char *s = (const unsigned char *)text, *end = s + len, *p = s;
    while (p < end) {
        const int n = dap_utf8_len(p, end);
        if (!n) break;
        p += n;
    }
    if (p == end) return NULL;
    unsigned char *out = (unsigned char *)malloc(len * 3 + 1);
    if (!out) return NULL;
    size_t o = (size_t)(p - s);
    memcpy(out, s, o);
    while (p < end) {
        const int n = dap_utf8_len(p, end);
        if (n) {
            memcpy(out + o, p, (size_t)n);
            o += (size_t)n;
            p += n;
        } else {
            out[o++] = 0xEF; out[o++] = 0xBF; out[o++] = 0xBD;
            p++;
        }
    }
    out[o] = '\0';
    *out_len = o;
    return (char *)out;
}

char *debug_server_utf8_repaired(const char *text, size_t len, size_t *out_len) {
    return dap_utf8_repaired(text, len, out_len);
}

// A message as it goes on the wire, or NULL. `*total` is its length.
static char *dap_frame(cJSON *json, int *total) {
    char *printed = cJSON_PrintUnformatted(json);
    if (!printed) return NULL;
    size_t body_len = strlen(printed);
    char *repaired = dap_utf8_repaired(printed, body_len, &body_len);
    const char *body = repaired ? repaired : printed;
    char header[40];
    const int header_len = snprintf(header, sizeof header, "Content-Length: %zu\r\n\r\n", body_len);
    // Allocated per message: a source file or a large memory read can be
    // any size, and a fixed buffer meant dropping the client over one.
    char *frame = (char *)malloc((size_t)header_len + body_len);
    if (frame) {
        memcpy(frame, header, (size_t)header_len);
        memcpy(frame + header_len, body, body_len);
        *total = header_len + (int)body_len;
    } else {
        fprintf(stderr, "[dap] no memory for a %zu byte message; dropping client\n", body_len);
    }
    free(repaired);
    cJSON_free(printed);
    return frame;
}

static void dap_send_all(const char *buf, int total) {
    int offset = 0;
    // A client that stops reading fills the socket buffer, and this is
    // the emulator's only thread. Retrying forever parks the machine on
    // a peer that may never come back, so give up after a bounded wait
    // and drop the client instead: a dead session is recoverable, a
    // frozen emulator is not.
    int stalls = 0;
    const int max_stalls = 500;          // ~500ms of a peer not reading
    while (offset < total) {
        int n = send(client_sock, buf + offset, total - offset, 0);
        if (n > 0) {
            offset += n;
            stalls = 0;
        } else if (n < 0 && WOULD_BLOCK) {
            if (send_no_wait || ++stalls > max_stalls) {
                fprintf(stderr, "[dap] client is not reading; dropping it\n");
                disconnect_client();
                break;
            }
            // SDL's, rather than Sleep/usleep behind a platform split:
            // usleep needs a feature-test macro this file does not set,
            // which only the -Werror Linux builds noticed.
            SDL_Delay(1);
        } else {
            break; // real error
        }
    }
}

// A frame of logpoint output the socket has not taken all of yet. Log lines never
// wait for a slow client and never cost it the connection: what does not get out
// stays in the ring until it is overwritten and reported as lost. But a frame
// begun has to be finished before anything else goes on the wire.
static char *log_frame = NULL;
static int   log_frame_len = 0;
static int   log_frame_off = 0;

static void log_frame_drop(void) {
    free(log_frame);
    log_frame = NULL;
    log_frame_len = log_frame_off = 0;
}

// Before any other message: the rest of the frame, at that message's terms.
static void log_frame_finish(void) {
    if (!log_frame) return;
    char *rest = log_frame;
    const int off = log_frame_off, len = log_frame_len;
    log_frame = NULL;                  // disconnect_client() must not free it under us
    log_frame_len = log_frame_off = 0;
    dap_send_all(rest + off, len - off);
    free(rest);
}

// As much as the socket will take now, without waiting.
static void log_frame_push(void) {
    while (log_frame && log_frame_off < log_frame_len) {
        const int n = send(client_sock, log_frame + log_frame_off, log_frame_len - log_frame_off, 0);
        if (n > 0) {
            log_frame_off += n;
        } else if (n < 0 && WOULD_BLOCK) {
            return;
        } else {
            disconnect_client();
            return;
        }
    }
    log_frame_drop();
}

static void send_dap_message(cJSON *json) {
    if (client_sock != SOCKET_INVALID) log_frame_finish();
    if (client_sock == SOCKET_INVALID) {
        cJSON_Delete(json);
        return;
    }
    int total = 0;
    char *frame = dap_frame(json, &total);
    cJSON_Delete(json);
    if (!frame) {
        disconnect_client();
        return;
    }
    dap_send_all(frame, total);
    free(frame);
}

static void sink_reply(bool success, cJSON *body, const char *message) {
    if (reply_sink->body) cJSON_Delete(reply_sink->body);
    reply_sink->success = success;
    reply_sink->body = body;
    snprintf(reply_sink->message, sizeof reply_sink->message, "%s", message ? message : "");
    reply_answered = true;
}

static void send_dap_response(int request_seq, const char *command, bool success, cJSON *body) {
    if (reply_sink) {
        sink_reply(success, body, NULL);
        return;
    }
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "seq", dap_seq++);
    cJSON_AddStringToObject(resp, "type", "response");
    cJSON_AddNumberToObject(resp, "request_seq", request_seq);
    cJSON_AddBoolToObject(resp, "success", success);
    cJSON_AddStringToObject(resp, "command", command);
    if (body) {
        cJSON_AddItemToObject(resp, "body", body);
    }
    send_dap_message(resp);
}

// An error response whose body carries one more item, key and value (taken
// over), beside the error; key NULL for none. `message` is the short form a
// client can match on, `format` the text it shows.
static void send_dap_error_response_full(int request_seq, const char *command, const char *message,
                                         const char *format, const char *key, cJSON *item) {
    if (reply_sink) {
        cJSON *extra = NULL;
        if (key && item) {
            extra = cJSON_CreateObject();
            cJSON_AddItemToObject(extra, key, item);
        } else if (item) {
            cJSON_Delete(item);
        }
        sink_reply(false, extra, format);
        return;
    }
    cJSON *body = cJSON_CreateObject();
    cJSON *error_obj = cJSON_CreateObject();
    // An id per message, derived from it so that the hundred-odd call sites
    // need no table to keep in step.
    uint32_t h = 2166136261u;
    for (const char *s = command; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    h = (h ^ ':') * 16777619u;
    for (const char *s = message; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    cJSON_AddNumberToObject(error_obj, "id", (double)(h % 0x7FFFFFFEu + 1));
    // `format` treats {name} as a variable, so text that may contain a brace
    // travels in a variable instead.
    if (strchr(format, '{')) {
        cJSON_AddStringToObject(error_obj, "format", "{text}");
        cJSON *vars = cJSON_CreateObject();
        cJSON_AddStringToObject(vars, "text", format);
        cJSON_AddItemToObject(error_obj, "variables", vars);
    } else {
        cJSON_AddStringToObject(error_obj, "format", format);
    }
    cJSON_AddItemToObject(body, "error", error_obj);
    if (key && item) cJSON_AddItemToObject(body, key, item);
    else if (item) cJSON_Delete(item);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "seq", dap_seq++);
    cJSON_AddStringToObject(resp, "type", "response");
    cJSON_AddNumberToObject(resp, "request_seq", request_seq);
    cJSON_AddBoolToObject(resp, "success", false);
    cJSON_AddStringToObject(resp, "command", command);
    cJSON_AddStringToObject(resp, "message", message);
    cJSON_AddItemToObject(resp, "body", body);
    send_dap_message(resp);
}

static void send_dap_error_response_with(int request_seq, const char *command, const char *message,
                                         const char *key, cJSON *item) {
    send_dap_error_response_full(request_seq, command, message, message, key, item);
}

static void send_dap_error_response(int request_seq, const char *command, const char *message) {
    send_dap_error_response_with(request_seq, command, message, NULL, NULL);
}

static void send_dap_event(const char *event_name, cJSON *body) {
    if (!dap_initialize_answered) {
        cJSON_Delete(body);
        return;
    }
    cJSON *event = cJSON_CreateObject();
    cJSON_AddNumberToObject(event, "seq", dap_seq++);
    cJSON_AddStringToObject(event, "type", "event");
    cJSON_AddStringToObject(event, "event", event_name);
    if (body) {
        cJSON_AddItemToObject(event, "body", body);
    }
    send_dap_message(event);
}

// ContinuedEvent — tell the client execution resumed (all threads).
static void send_continued_event(void) {
    dap_stop_announced = false;   // whatever we were stopped in is over
    if (client_sock == SOCKET_INVALID) return;
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "threadId", 1);
    cJSON_AddBoolToObject(body, "allThreadsContinued", true);
    send_dap_event("continued", body);
}

// OutputEvent — forward text to the client's debug console. Public API so the
// emulator can pipe program/console output to an attached DAP client.
void debug_server_output(const char *category, const char *text) {
    if (!server_enabled || client_sock == SOCKET_INVALID || !text) return;
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "category", category ? category : "console");
    cJSON_AddStringToObject(body, "output", text);
    send_dap_event("output", body);
}

// Pending events to send on next poll (avoid back-to-back sends)
// Announces the single emulated CPU as a DAP thread. Sent after launch and
// after attach, which are the two ways a session starts.
static void send_dap_event_thread_started(void);

// Set by launch when the client asked to be given control before anything runs.
static bool dap_stop_on_entry = false;

// ─── DAP command handlers ───────────────────────────────────────────

// A path from the client, as the emulator holds it. file: URIs are decoded when
// the client said it uses them.
static void dap_path_in(const char *s, char *out, size_t n) {
    if (!dap_paths_are_uris || strncmp(s, "file://", 7) != 0) {
        snprintf(out, n, "%s", s);
        return;
    }
    s += 7;
    const char *slash = strchr(s, '/');   // past any authority
    s = slash ? slash : s + strlen(s);
#ifdef _WIN32
    if (s[0] == '/' && isalpha((unsigned char)s[1]) && (s[2] == ':' || !strncmp(s + 2, "%3A", 3)
                                                        || !strncmp(s + 2, "%3a", 3)))
        s++;
#endif
    size_t o = 0;
    while (*s && o + 1 < n) {
        if (s[0] == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = { s[1], s[2], 0 };
            out[o++] = (char)strtoul(hex, NULL, 16);
            s += 3;
        } else {
            out[o++] = *s++;
        }
    }
    out[o] = '\0';
}

// A path for the client: `path` itself, or a file: URI in `buf`.
static const char *dap_path_out(const char *path, char *buf, size_t n) {
    if (!dap_paths_are_uris) return path;
    const bool drive = isalpha((unsigned char)path[0]) && path[1] == ':';
    if (path[0] != '/' && !drive) return path;   // relative: no URI can name it
    size_t o = (size_t)snprintf(buf, n, "file://%s", drive ? "/" : "");
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if (o + 4 >= n) return path;
        const unsigned char c = (*p == '\\') ? '/' : *p;
        if (isalnum(c) || strchr("-._~/:", c))
            buf[o++] = (char)c;
        else
            o += (size_t)snprintf(buf + o, n - o, "%%%02X", c);
    }
    buf[o] = '\0';
    return buf;
}

// A reply goes to whoever called, and only a DAP client chose a format. An MCP
// tool always gets 1-based lines and native paths, whatever an attached editor
// asked for. Events stay on dap_line_out and dap_path_out: they are the editor's
// even when an MCP request is what caused them.
static int caller_line_out(int line) {
    return current_caller == DBG_CALLER_DAP ? dap_line_out(line) : line;
}
static const char *caller_path_out(const char *path, char *buf, size_t n) {
    return current_caller == DBG_CALLER_DAP ? dap_path_out(path, buf, n) : path;
}
static int caller_line_in(int line) {
    return current_caller == DBG_CALLER_DAP ? dap_line_in(line) : line;
}
static void caller_path_in(const char *path, char *out, size_t n) {
    if (current_caller == DBG_CALLER_DAP) dap_path_in(path, out, n);
    else snprintf(out, n, "%s", path);
}

// ─── Stops on interrupts and exceptions ─────────────────────────────

static const struct {
    const char *filter, *label, *description;
    uint16_t    bit;
} stop_filter_table[] = {
    { "irq", "IRQ", "Stop at the first instruction of the handler when an IRQ is taken.",
      DEBUG_STOP_IRQ },
    { "nmi", "NMI", "Stop at the first instruction of the handler when an NMI is taken.",
      DEBUG_STOP_NMI },
    { "brk", "BRK", "Stop at the first instruction of the handler after a BRK.",
      DEBUG_STOP_BRK },
    { "stp", "STP", "Report the STP trap as an exception. A debugger stops on it either way.",
      DEBUG_STOP_STP },
};
#define NUM_STOP_FILTERS ((int)(sizeof stop_filter_table / sizeof stop_filter_table[0]))

static uint16_t stop_filter_bit(const char *name) {
    for (int i = 0; i < NUM_STOP_FILTERS; i++) {
        if (!strcmp(name, stop_filter_table[i].filter)) return stop_filter_table[i].bit;
    }
    return 0;
}

static cJSON *stop_filter_names(uint16_t mask) {
    cJSON *names = cJSON_CreateArray();
    for (int i = 0; i < NUM_STOP_FILTERS; i++) {
        if (mask & stop_filter_table[i].bit)
            cJSON_AddItemToArray(names, cJSON_CreateString(stop_filter_table[i].filter));
    }
    return names;
}

static int stop_filter_index(uint16_t bit) {
    for (int i = 0; i < NUM_STOP_FILTERS; i++) {
        if (stop_filter_table[i].bit == bit) return i;
    }
    return 0;
}

static void exception_text(const debug_stop_exception_t *ex, char *out, size_t n) {
    const char *label = stop_filter_table[stop_filter_index(ex->kind)].label;
    char where[16];
    if (ex->bank) snprintf(where, sizeof where, "$%02X:%04X", ex->bank, ex->address);
    else          snprintf(where, sizeof where, "$%04X", ex->address);
    if (ex->has_vector) snprintf(out, n, "%s at %s, vector $%04X", label, where, ex->vector);
    else                snprintf(out, n, "%s at %s", label, where);
}

static int handle_dap_initialize(int seq, cJSON *args) {
    cJSON *lb = args ? cJSON_GetObjectItemCaseSensitive(args, "linesStartAt1") : NULL;
    cJSON *cb = args ? cJSON_GetObjectItemCaseSensitive(args, "columnsStartAt1") : NULL;
    cJSON *pf = args ? cJSON_GetObjectItemCaseSensitive(args, "pathFormat") : NULL;
    dap_line_base = (lb && cJSON_IsFalse(lb)) ? 0 : 1;
    dap_col_base  = (cb && cJSON_IsFalse(cb)) ? 0 : 1;
    dap_paths_are_uris = pf && cJSON_IsString(pf) && !strcmp(pf->valuestring, "uri");

    cJSON *caps = cJSON_CreateObject();
    cJSON_AddBoolToObject(caps, "supportsConfigurationDoneRequest", true);
    cJSON_AddBoolToObject(caps, "supportsDelayedStackTraceLoading", true);
    cJSON_AddBoolToObject(caps, "supportsSetVariable", true);
    cJSON_AddBoolToObject(caps, "supportsRestartRequest", true);
    cJSON_AddBoolToObject(caps, "supportsTerminateRequest", true);
    cJSON_AddBoolToObject(caps, "supportsReadMemoryRequest", true);
    cJSON_AddBoolToObject(caps, "supportsWriteMemoryRequest", true);
    cJSON_AddBoolToObject(caps, "supportsDisassembleRequest", true);
    cJSON_AddBoolToObject(caps, "supportTerminateDebuggee", true);
    cJSON_AddBoolToObject(caps, "supportsEvaluateForHovers", true);
    cJSON_AddBoolToObject(caps, "supportsSteppingGranularity", true);
    cJSON_AddBoolToObject(caps, "supportsGotoTargetsRequest", true);
    cJSON_AddBoolToObject(caps, "supportsSetExpression", true);
    cJSON_AddBoolToObject(caps, "supportsCompletionsRequest", true);
    cJSON_AddBoolToObject(caps, "supportsFunctionBreakpoints", true);
    // Newly implemented in this build:
    cJSON_AddBoolToObject(caps, "supportsDataBreakpoints", true);
    cJSON_AddBoolToObject(caps, "supportsDataBreakpointBytes", true);
    cJSON_AddBoolToObject(caps, "supportsInstructionBreakpoints", true);
    cJSON_AddBoolToObject(caps, "supportsLoadedSourcesRequest", true);
    cJSON_AddBoolToObject(caps, "supportsBreakpointLocationsRequest", true);
    cJSON_AddBoolToObject(caps, "supportsStepInTargetsRequest", true);
    cJSON_AddBoolToObject(caps, "supportsConditionalBreakpoints", true);
    cJSON_AddBoolToObject(caps, "supportsHitConditionalBreakpoints", true);
    cJSON_AddBoolToObject(caps, "supportsLogPoints", true);
    cJSON_AddBoolToObject(caps, "supportsExceptionInfoRequest", true);
    cJSON *filters = cJSON_AddArrayToObject(caps, "exceptionBreakpointFilters");
    for (int i = 0; i < NUM_STOP_FILTERS; i++) {
        cJSON *f = cJSON_CreateObject();
        cJSON_AddStringToObject(f, "filter", stop_filter_table[i].filter);
        cJSON_AddStringToObject(f, "label", stop_filter_table[i].label);
        cJSON_AddStringToObject(f, "description", stop_filter_table[i].description);
        cJSON_AddBoolToObject(f, "default", false);
        cJSON_AddItemToArray(filters, f);
    }
    // Non-standard, so that a client can tell this emulator's performance
    // budget surface from one that would reject the requests.
    cJSON_AddBoolToObject(caps, "supportsX16PerfStats", true);
    // Non-standard: a full breakpoint/watchpoint listing regardless of who
    // set each one, and per-frame sprite-multiplexing stats.
    cJSON_AddBoolToObject(caps, "supportsX16ListBreakpoints", true);
    cJSON_AddBoolToObject(caps, "supportsX16SpriteMuxStats", true);
    send_dap_response(seq, "initialize", true, caps);

    // Sending can drop the client, and a session must not start for nobody.
    if (dap_reply_lost()) return 0;
    dap_initialize_answered = true;
    dap_session_active = true;

    // Per the DAP spec this follows the initialize response immediately: a
    // client is entitled to wait for it before sending configurationDone, so
    // deferring it to the next request deadlocks anything strict about that.
    send_dap_event("initialized", NULL);
    return 0;
}

static int handle_dap_launch(int seq, cJSON *args) {
    // Load .dbg file if specified in launch arguments (additive, don't clear existing)
    if (args) {
        cJSON *dbg_file = cJSON_GetObjectItemCaseSensitive(args, "dbgFile");
        if (dbg_file && cJSON_IsString(dbg_file)) {
            if (dbg_info_load(dbg_file->valuestring) == 0) {
                printf("[dap] Loaded debug info: %s\n", dbg_file->valuestring);
            } else {
                fprintf(stderr, "[dap] Warning: failed to load debug info: %s\n",
                        dbg_file->valuestring);
            }
        }
        cJSON *soe = cJSON_GetObjectItemCaseSensitive(args, "stopOnEntry");
        dap_stop_on_entry = (soe && cJSON_IsTrue(soe));
        cJSON *nd = cJSON_GetObjectItemCaseSensitive(args, "noDebug");
        dap_no_debug = nd && cJSON_IsTrue(nd);
        if (dap_no_debug) {
            // Breakpoints may have been set before launch arrived.
            dap_release_breakpoints();
            dap_stop_on_entry = false;
        }
    }
    dap_launched = true;
    send_dap_response(seq, "launch", true, NULL);

    send_dap_event_thread_started();

    return 0;
}

static int handle_dap_configuration_done(int seq) {
    send_dap_response(seq, "configurationDone", true, NULL);

    // Sending can drop the client -- a peer that stops reading is disconnected
    // rather than allowed to freeze the emulator -- so there may be no session
    // left to configure. Carrying on would halt a headless machine with nobody
    // able to resume it.
    if (dap_reply_lost()) {
        dap_stop_on_entry = false;
        return 0;
    }

    // Already halted, because a breakpoint set moments ago has hit or a
    // previous client left it that way. Report where we actually are; resuming
    // would drive past a stop the client should see, and saying nothing leaves
    // it showing a running target it cannot pause. Checked before stopOnEntry
    // so the same halt is not announced twice under two different reasons.
    if (currentMode == DMODE_STOP) {
        dap_stop_on_entry = false;
        // Unless it has already been reported -- a breakpoint that hit while
        // the client was still configuring emits its own stopped event, and
        // announcing the same halt again under a different reason confuses a
        // client into thinking it stopped twice.
        if (!dap_stop_announced) {
            const char *why = DEBUGGetStopReason();
            debug_server_notify_stopped(!strcmp(why, "data breakpoint") || !strcmp(why, "exception")
                                            ? why : "breakpoint");
        }
        return 1;
    }

    // The client wants control before anything executes, and is waiting for the
    // stopped event to know it has it.
    if (dap_stop_on_entry) {
        dap_stop_on_entry = false;
        currentMode = DMODE_STOP;
        DEBUGCancelStep();
        debug_server_notify_stopped("entry");
        return 1;
    }

    currentMode = DMODE_RUN;
    return 1;
}

static int handle_dap_threads(int seq) {
    cJSON *body = cJSON_CreateObject();
    cJSON *threads = cJSON_CreateArray();
    cJSON *thread = cJSON_CreateObject();
    cJSON_AddNumberToObject(thread, "id", 1);
    cJSON_AddStringToObject(thread, "name", "6502 CPU");
    cJSON_AddItemToArray(threads, thread);
    cJSON_AddItemToObject(body, "threads", threads);
    send_dap_response(seq, "threads", true, body);
    return 0;
}

// Build a DAP stack frame object for a code address. `bank` is the 65C816
// program bank (0 on the 65C02); it makes the instructionPointerReference a
// full 24-bit address so disassembly opens in the right bank on GS.
static cJSON *build_stack_frame(int id, uint16_t addr, uint8_t bank, bool is_current) {
    cJSON *frame = cJSON_CreateObject();
    cJSON_AddNumberToObject(frame, "id", id);

    // `addr` stays the frame's instruction pointer. Its name and line come from
    // the instruction it is at, which for a caller is the JSR.
    const uint16_t site = call_stack_site(is_current, addr);
    const int      ram_bank = (int)memory_get_ram_bank();

    const char *src_file = NULL;
    int         src_line = 0;
    bool        has_source = dbg_info_addr_to_source_banked(site, ram_bank, &src_file, &src_line);

    char addr_text[16];
    if (bank != 0 || is_gen2) snprintf(addr_text, sizeof(addr_text), "$%02X:%04X", bank, addr);
    else                      snprintf(addr_text, sizeof(addr_text), "$%04X", addr);

    const char *label = NULL;
    dbg_addr_t  label_at = 0;
    char        name_buf[80];
    if (dbg_info_frame_label(site, ram_bank, &label, &label_at)) {
        const unsigned offset = (uint16_t)(addr - label_at);
        if (offset) snprintf(name_buf, sizeof(name_buf), "%s+%u (%s)", label, offset, addr_text);
        else        snprintf(name_buf, sizeof(name_buf), "%s (%s)", label, addr_text);
    } else {
        snprintf(name_buf, sizeof(name_buf), "%s", addr_text);
    }
    cJSON_AddStringToObject(frame, "name", name_buf);
    // Without a source the protocol wants both at 0, whatever the client's base.
    cJSON_AddNumberToObject(frame, "line", has_source ? dap_line_out(src_line) : 0);
    cJSON_AddNumberToObject(frame, "column", has_source ? dap_col_base : 0);

    if (has_source) {
        char uri[1024];
        cJSON *source = cJSON_CreateObject();
        cJSON_AddStringToObject(source, "path", dap_path_out(src_file, uri, sizeof uri));
        const char *basename = strrchr(src_file, '/');
        if (!basename) basename = strrchr(src_file, '\\');
        cJSON_AddStringToObject(source, "name", basename ? basename + 1 : src_file);
        cJSON_AddItemToObject(frame, "source", source);
    } else {
        // instructionPointerReference below is what opens the disassembly.
        cJSON_AddStringToObject(frame, "presentationHint", is_current ? "normal" : "subtle");
    }

    char instr_ref[16];
    if (bank != 0 || is_gen2)
        snprintf(instr_ref, sizeof(instr_ref), "0x%06X", ((uint32_t)bank << 16) | addr);
    else
        snprintf(instr_ref, sizeof(instr_ref), "0x%04X", addr);
    cJSON_AddStringToObject(frame, "instructionPointerReference", instr_ref);
    return frame;
}

static uint8_t call_stack_read(uint16_t address, uint8_t bank) {
    return debug_read6502(address, bank, USE_CURRENT_X16_BANK);
}

static int unwind_cpu_stack(uint16_t *frames) {
    return call_stack_unwind(regs.pc, regs.sp, regs.k, regs.is65c816 && !regs.e,
                             call_stack_read, frames);
}

// The frames a request asked for. No levels, or 0, means all that remain.
static void stack_window(double start, double levels, int total, int *first, int *last) {
    *first = start <= 0 ? 0 : start >= total ? total : (int)start;
    const int left = total - *first;
    *last = *first + (levels >= 1 && levels < left ? (int)levels : left);
}

static int handle_dap_stack_trace(int seq, cJSON *args) {
    double startFrame = 0, levels = 0;
    if (args) {
        cJSON *sf = cJSON_GetObjectItemCaseSensitive(args, "startFrame");
        if (sf && cJSON_IsNumber(sf)) startFrame = sf->valuedouble;
        cJSON *lv = cJSON_GetObjectItemCaseSensitive(args, "levels");
        if (lv && cJSON_IsNumber(lv)) levels = lv->valuedouble;
    }

    uint16_t addrs[CALL_STACK_MAX];
    const int nf = unwind_cpu_stack(addrs);

    cJSON *body   = cJSON_CreateObject();
    cJSON *frames = cJSON_CreateArray();
    int    first, last;
    stack_window(startFrame, levels, nf, &first, &last);
    for (int i = first; i < last; i++) {
        cJSON_AddItemToArray(frames, build_stack_frame(i + 1, addrs[i], regs.k, i == 0));
    }
    cJSON_AddItemToObject(body, "stackFrames", frames);
    cJSON_AddNumberToObject(body, "totalFrames", nf);
    send_dap_response(seq, "stackTrace", true, body);
    return 0;
}

static bool stack_arg_ok(const cJSON *v) {
    return !v || (cJSON_IsNumber(v) && v->valuedouble >= 0 && v->valuedouble <= CALL_STACK_MAX &&
                  v->valuedouble == (int)v->valuedouble);
}

// The same unwind as stackTrace, as flat fields rather than a display name.
static int handle_x16_call_stack(int seq, cJSON *args) {
    static const char cmd[] = "x16/callStack";
    cJSON *sf = args ? cJSON_GetObjectItemCaseSensitive(args, "startFrame") : NULL;
    cJSON *lv = args ? cJSON_GetObjectItemCaseSensitive(args, "levels") : NULL;
    if (!stack_arg_ok(sf) || !stack_arg_ok(lv)) {
        char msg[80];
        snprintf(msg, sizeof msg, "'startFrame' and 'levels' are whole numbers from 0 to %d",
                 CALL_STACK_MAX);
        send_dap_error_response(seq, cmd, msg);
        return 0;
    }

    uint16_t addrs[CALL_STACK_MAX];
    const int nf = unwind_cpu_stack(addrs);
    const int ram_bank = (int)memory_get_ram_bank();
    int first, last;
    stack_window(sf ? sf->valuedouble : 0, lv ? lv->valuedouble : 0, nf, &first, &last);

    cJSON *body   = cJSON_CreateObject();
    cJSON *frames = cJSON_CreateArray();
    for (int i = first; i < last; i++) {
        const uint16_t addr = addrs[i];
        const uint16_t site = call_stack_site(i == 0, addr);
        cJSON *f = cJSON_CreateObject();
        cJSON_AddNumberToObject(f, "index", i);
        cJSON_AddStringToObject(f, "kind", i == 0 ? "current" : "return");
        cJSON_AddNumberToObject(f, "address", addr);
        if (i > 0) {
            cJSON_AddNumberToObject(f, "callSite", site);
        }
        if (regs.k != 0 || is_gen2) {
            cJSON_AddNumberToObject(f, "programBank", regs.k);
        }
        if (regs.k == 0 && addr >= 0xA000) {
            cJSON_AddNumberToObject(f, "bank", addr < 0xC000 ? ram_bank : memory_get_rom_bank());
        }

        const char *name = NULL;
        dbg_addr_t  at   = 0;
        if (dbg_info_frame_label(site, ram_bank, &name, &at)) {
            cJSON_AddStringToObject(f, "symbol", name);
            cJSON_AddNumberToObject(f, "symbolOffset", (uint16_t)(addr - at));
        }
        const char *file = NULL;
        int         line = 0;
        if (dbg_info_addr_to_source_banked(site, ram_bank, &file, &line) && file) {
            char uri[1024];
            cJSON_AddStringToObject(f, "file", caller_path_out(file, uri, sizeof uri));
            cJSON_AddNumberToObject(f, "line", caller_line_out(line));
        }
        cJSON_AddItemToArray(frames, f);
    }
    cJSON_AddBoolToObject(body, "heuristic", true);
    cJSON_AddItemToObject(body, "stackFrames", frames);
    cJSON_AddNumberToObject(body, "totalFrames", nf);
    send_dap_response(seq, cmd, true, body);
    return 0;
}

static int handle_dap_scopes(int seq, cJSON *args) {
    (void)args;
    cJSON *body = cJSON_CreateObject();
    cJSON *scopes = cJSON_CreateArray();

    // Registers scope
    cJSON *reg_scope = cJSON_CreateObject();
    cJSON_AddStringToObject(reg_scope, "name", "Registers");
    cJSON_AddNumberToObject(reg_scope, "variablesReference", VARREF_REGISTERS);
    cJSON_AddBoolToObject(reg_scope, "expensive", false);
    cJSON_AddStringToObject(reg_scope, "presentationHint", "registers");
    cJSON_AddItemToArray(scopes, reg_scope);

    // Virtual registers scope — the X16's sixteen 16-bit pseudo-registers R0-R15
    // (a software convention shadowed in zero page $02-$21; used by the KERNAL/
    // VERA/GRAPH APIs and cc65). Presented like CPU registers so they can be
    // watched/edited as first-class 16-bit registers.
    cJSON *vreg_scope = cJSON_CreateObject();
    cJSON_AddStringToObject(vreg_scope, "name", "Virtual Regs (R0-R15)");
    cJSON_AddNumberToObject(vreg_scope, "variablesReference", VARREF_VREGS);
    cJSON_AddBoolToObject(vreg_scope, "expensive", false);
    cJSON_AddStringToObject(vreg_scope, "presentationHint", "registers");
    cJSON_AddItemToArray(scopes, vreg_scope);

    // Zero Page scope
    cJSON *zp_scope = cJSON_CreateObject();
    cJSON_AddStringToObject(zp_scope, "name", "Zero Page");
    cJSON_AddNumberToObject(zp_scope, "variablesReference", VARREF_ZEROPAGE);
    cJSON_AddBoolToObject(zp_scope, "expensive", false);
    cJSON_AddItemToArray(scopes, zp_scope);

    // Stack scope
    cJSON *stk_scope = cJSON_CreateObject();
    cJSON_AddStringToObject(stk_scope, "name", "Stack");
    cJSON_AddNumberToObject(stk_scope, "variablesReference", VARREF_STACK);
    cJSON_AddBoolToObject(stk_scope, "expensive", false);
    cJSON_AddItemToArray(scopes, stk_scope);

    cJSON_AddItemToObject(body, "scopes", scopes);
    send_dap_response(seq, "scopes", true, body);
    return 0;
}

static void add_variable(cJSON *vars, const char *name, const char *value, const char *type) {
    cJSON *var = cJSON_CreateObject();
    cJSON_AddStringToObject(var, "name", name);
    cJSON_AddStringToObject(var, "value", value);
    if (type) cJSON_AddStringToObject(var, "type", type);
    cJSON_AddNumberToObject(var, "variablesReference", 0);
    cJSON_AddItemToArray(vars, var);
}

// Parse a number from a DAP value string: "$hex", "0xhex", or decimal.
static uint32_t dap_parse_num(const char *s) {
    if (!s) return 0;
    while (*s == ' ') s++;
    if (s[0] == '$')
        return (uint32_t)strtoul(s + 1, NULL, 16);
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        return (uint32_t)strtoul(s + 2, NULL, 16);
    return (uint32_t)strtoul(s, NULL, 0);
}

// One number filling [s, end), with spaces allowed around it: $hex, 0xhex or
// decimal. Fails on anything else, where dap_parse_num() would quietly read a
// prefix of it, or read a leading zero as octal.
static bool dap_strict_num(const char *s, const char *end, uint32_t *out) {
    while (s < end && *s == ' ') s++;
    while (end > s && end[-1] == ' ') end--;
    unsigned base = 10;
    if (s < end && *s == '$') {
        s++;
        base = 16;
    } else if (end - s >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        base = 16;
    }
    if (s == end) return false;
    uint64_t v = 0;
    for (; s < end; s++) {
        const unsigned char c = (unsigned char)*s;
        unsigned d;
        if (isdigit(c)) d = (unsigned)(c - '0');
        else if (base == 16 && isxdigit(c)) d = (unsigned)(tolower(c) - 'a' + 10);
        else return false;
        v = v * base + d;
        if (v > 0xFFFFFFFFu) return false;
    }
    *out = (uint32_t)v;
    return true;
}

static bool dap_has_prefix_i(const char *s, const char *prefix) {
    while (*prefix) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return false;
        s++; prefix++;
    }
    return true;
}

// Parse a DAP memoryReference. A bare number is a CPU address (24-bit
// bank:addr); a "vram:" prefix selects VERA's 17-bit video address space, which
// is not in the CPU map and so cannot be reached through a CPU address at all.
// VRAM references default to hex, since every VRAM address in the docs and the
// debugger is written that way.
//
// Fails rather than falling back to zero on a reference it cannot read. The old
// behaviour ran the string through strtoul and used whatever came out, so a
// typo, a "$C000" and a "vram:1B000" all quietly returned the bytes at CPU
// address 0 with success:true -- the wrong memory, with nothing in the response
// to say so.
static bool dap_parse_mem_ref(const char *s, bool *is_vram, uint32_t *out) {
    if (!s) return false;
    while (*s == ' ') s++;

    *is_vram = false;
    // A symbol, spelled out because a bare token here has always been a number.
    // One in a bank that is not mapped would read some other bank's bytes.
    if (dap_has_prefix_i(s, "sym:")) {
        dbg_resolved_t r;
        char why[160];
        if (!dbg_info_resolve(s + 4, &r, why, sizeof why)) return false;
        if (r.bank >= 0 && r.addr <= 0xFFFF && debug_current_x16_bank((int)r.addr, 0) != r.bank)
            return false;
        *out = r.addr;
        return true;
    }
    if (dap_has_prefix_i(s, "vram:")) {
        *is_vram = true;
        s += 5;
        while (*s == ' ') s++;
    }

    int base = *is_vram ? 16 : 0;
    if (s[0] == '$') {
        s++;
        base = 16;
    } else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        base = 16;
    }

    char *endp = NULL;
    unsigned long long v = strtoull(s, &endp, base);
    if (!endp || endp == s || v > 0xFFFFFFFFull) return false;
    while (*endp == ' ') endp++;
    if (*endp) return false;

    *out = (uint32_t)v;
    return true;
}

// Non-intrusive VERA register read for debug views. AUDIO_CTRL (0x1B) runs
// audio_render() and SPI_DATA (0x1E) starts a real SPI transfer *on read*
// (video_read ignores debugOn for them), so never poll those — return 0.
static uint8_t vera_read_dbg(uint8_t reg) {
    reg &= 0x1F;
    if (reg == 0x1B || reg == 0x1E) return 0;
    return video_read(reg, true);
}

static int handle_dap_variables(int seq, cJSON *args) {
    int ref = 0;
    if (args) {
        cJSON *ref_item = cJSON_GetObjectItemCaseSensitive(args, "variablesReference");
        if (ref_item && cJSON_IsNumber(ref_item)) ref = (int)ref_item->valuedouble;
    }

    cJSON *body = cJSON_CreateObject();
    cJSON *vars = cJSON_CreateArray();
    char buf[32];

    if (ref == VARREF_REGISTERS) {
        // PC is 24-bit in gen2/GS (program bank K : 16-bit PC).
        if (is_gen2 || (regs.is65c816 && regs.k != 0))
            snprintf(buf, sizeof(buf), "$%02X:%04X", regs.k, regs.pc);
        else
            snprintf(buf, sizeof(buf), "$%04X (%d)", regs.pc, regs.pc);
        add_variable(vars, "PC", buf, "uint16");

        // A/X/Y: show 16-bit in native mode, 8-bit in emulation mode
        if (regs.is65c816 && !regs.e) {
            // Native mode: A width depends on M flag, X/Y on X flag
            if (regs.status & FLAG_MEMORY_WIDTH) {
                snprintf(buf, sizeof(buf), "$%02X (%d)", regs.a, regs.a);
                add_variable(vars, "A", buf, "uint8");
            } else {
                snprintf(buf, sizeof(buf), "$%04X (%d)", regs.c, regs.c);
                add_variable(vars, "A", buf, "uint16");
            }
            if (regs.status & FLAG_INDEX_WIDTH) {
                snprintf(buf, sizeof(buf), "$%02X (%d)", (uint8_t)regs.x, (uint8_t)regs.x);
                add_variable(vars, "X", buf, "uint8");
                snprintf(buf, sizeof(buf), "$%02X (%d)", (uint8_t)regs.y, (uint8_t)regs.y);
                add_variable(vars, "Y", buf, "uint8");
            } else {
                snprintf(buf, sizeof(buf), "$%04X (%d)", regs.x, regs.x);
                add_variable(vars, "X", buf, "uint16");
                snprintf(buf, sizeof(buf), "$%04X (%d)", regs.y, regs.y);
                add_variable(vars, "Y", buf, "uint16");
            }
            snprintf(buf, sizeof(buf), "$%04X (%d)", regs.sp, regs.sp);
            add_variable(vars, "SP", buf, "uint16");
            snprintf(buf, sizeof(buf), "$%04X (%d)", regs.dp, regs.dp);
            add_variable(vars, "D", buf, "uint16");
            snprintf(buf, sizeof(buf), "$%02X (%d)", regs.db, regs.db);
            add_variable(vars, "DBR", buf, "uint8");
            snprintf(buf, sizeof(buf), "$%02X (%d)", regs.k, regs.k);
            add_variable(vars, "PBR", buf, "uint8");
        } else {
            // Emulation mode: all 8-bit
            snprintf(buf, sizeof(buf), "$%02X (%d)", regs.a, regs.a);
            add_variable(vars, "A", buf, "uint8");
            snprintf(buf, sizeof(buf), "$%02X (%d)", (uint8_t)regs.x, (uint8_t)regs.x);
            add_variable(vars, "X", buf, "uint8");
            snprintf(buf, sizeof(buf), "$%02X (%d)", (uint8_t)regs.y, (uint8_t)regs.y);
            add_variable(vars, "Y", buf, "uint8");
            snprintf(buf, sizeof(buf), "$%02X (%d)", (uint8_t)regs.sp, (uint8_t)regs.sp);
            add_variable(vars, "SP", buf, "uint8");
        }

        // Status flags
        uint8_t p = regs.status;
        if (regs.is65c816 && !regs.e) {
            snprintf(buf, sizeof(buf), "$%02X [%c%c%c%c%c%c%c%c]", p,
                     (p & 0x80) ? 'N' : 'n', (p & 0x40) ? 'V' : 'v',
                     (p & 0x20) ? 'M' : 'm', (p & 0x10) ? 'X' : 'x',
                     (p & 0x08) ? 'D' : 'd', (p & 0x04) ? 'I' : 'i',
                     (p & 0x02) ? 'Z' : 'z', (p & 0x01) ? 'C' : 'c');
        } else {
            snprintf(buf, sizeof(buf), "$%02X [%c%c-%c%c%c%c%c]", p,
                     (p & 0x80) ? 'N' : 'n', (p & 0x40) ? 'V' : 'v',
                     (p & 0x10) ? 'B' : 'b',
                     (p & 0x08) ? 'D' : 'd', (p & 0x04) ? 'I' : 'i',
                     (p & 0x02) ? 'Z' : 'z', (p & 0x01) ? 'C' : 'c');
        }
        add_variable(vars, "P (Status)", buf, "uint8");

        snprintf(buf, sizeof(buf), "$%02X (%d)", memory_get_ram_bank(), memory_get_ram_bank());
        add_variable(vars, "RAM Bank", buf, "uint8");

        snprintf(buf, sizeof(buf), "$%02X (%d)", memory_get_rom_bank(), memory_get_rom_bank());
        add_variable(vars, "ROM Bank", buf, "uint8");

    } else if (ref == VARREF_ZEROPAGE) {
        // Show zero page in groups of 16 bytes
        for (int row = 0; row < 16; row++) {
            char name[16], value[80];
            int base = row * 16;
            snprintf(name, sizeof(name), "$%02X-$%02X", base, base + 15);
            char *p = value;
            for (int col = 0; col < 16; col++) {
                uint8_t b = debug_read6502((uint16_t)(base + col), 0, USE_CURRENT_X16_BANK);
                p += snprintf(p, sizeof(value) - (p - value), "%02X ", b);
            }
            add_variable(vars, name, value, NULL);
        }

    } else if (ref == VARREF_STACK) {
        // Show stack contents from SP+1 to $FF (what's currently on the stack)
        uint8_t sp = (uint8_t)regs.sp;
        int depth = 0xFF - sp;
        if (depth > 32) depth = 32; // cap display
        for (int i = 0; i < depth; i++) {
            char name[16], value[16];
            uint16_t addr = 0x0100 + sp + 1 + i;
            uint8_t b = debug_read6502(addr, 0, USE_CURRENT_X16_BANK);
            snprintf(name, sizeof(name), "$%04X", addr);
            snprintf(value, sizeof(value), "$%02X (%d)", b, b);
            add_variable(vars, name, value, "uint8");
        }

    } else if (ref == VARREF_VREGS) {
        // X16 virtual registers R0-R15: 16-bit little-endian at direct-page +
        // $02 + 2*n (D is 0 under the KERNAL ABI, non-zero possible in GS native).
        for (int n = 0; n < 16; n++) {
            char name[8], value[48];
            uint16_t a  = dbg_machine_dp_add((uint16_t)(0x02 + 2 * n));
            uint8_t  lo = debug_read6502(a, 0, USE_CURRENT_X16_BANK);
            uint8_t  hi = debug_read6502((uint16_t)(a + 1), 0, USE_CURRENT_X16_BANK);
            uint16_t v  = (uint16_t)(lo | (hi << 8));
            snprintf(name, sizeof(name), "R%d", n);
            snprintf(value, sizeof(value), "$%04X (%d)  @$%04X", v, v, a);
            add_variable(vars, name, value, "uint16");
        }
    }

    cJSON_AddItemToObject(body, "variables", vars);
    send_dap_response(seq, "variables", true, body);
    return 0;
}

// Case-insensitive prefix test (pfx must be lowercase).
static bool dap_ci_prefix(const char *s, const char *pfx) {
    for (; *pfx; s++, pfx++) {
        char a = (*s >= 'A' && *s <= 'Z') ? (char)(*s + 32) : *s;
        if (a != *pfx) return false;
    }
    return true;
}

// Parse a single "OPERAND OP VALUE" term: OPERAND is A/X/Y/SP/P, byte[$addr],
// word[$addr] or a bare $addr (byte); OP is == != < <= > >= (or =). Fills the
// outputs and returns true on success. Codes match debugger.c's bp_cond enums
// (A=0..WORD=6; EQ=0..GE=5).
static bool dap_parse_value_term(const char *s, int *out_operand, uint16_t *out_oaddr,
                                 int *out_op, uint32_t *out_value) {
    while (*s == ' ') s++;
    int      operand = -1;
    uint16_t oaddr   = 0;
    #define ISALNUM(c) (((c) >= '0' && (c) <= '9') || ((c) >= 'a' && (c) <= 'z') || ((c) >= 'A' && (c) <= 'Z'))
    if (!strncmp(s, "byte[", 5) || !strncmp(s, "word[", 5)) {
        const char *b = strchr(s, ']');
        uint32_t a;
        if (!b || !dap_strict_num(s + 5, b, &a) || a > 0xFFFF) return false;
        operand = (s[0] == 'b') ? 5 : 6;
        oaddr = (uint16_t)a;
        s = b + 1;
    } else if (*s == '$' || (*s == '0' && (s[1] == 'x' || s[1] == 'X'))) {
        const char *e = s + strcspn(s, " =!<>");
        uint32_t a;
        if (!dap_strict_num(s, e, &a) || a > 0xFFFF) return false;
        operand = 5;
        oaddr = (uint16_t)a;
        s = e;
    }
    else if ((s[0] == 'A' || s[0] == 'a') && !ISALNUM(s[1])) { operand = 0; s++; }
    else if ((s[0] == 'X' || s[0] == 'x') && !ISALNUM(s[1])) { operand = 1; s++; }
    else if ((s[0] == 'Y' || s[0] == 'y') && !ISALNUM(s[1])) { operand = 2; s++; }
    else if ((s[0] == 'S' || s[0] == 's') && (s[1] == 'P' || s[1] == 'p')) { operand = 3; s += 2; }
    else if ((s[0] == 'P' || s[0] == 'p') && !ISALNUM(s[1])) { operand = 4; s++; }
    #undef ISALNUM
    if (operand < 0) return false;
    while (*s == ' ') s++;
    int op = -1;
    if (!strncmp(s, "==", 2)) { op = 0; s += 2; }
    else if (!strncmp(s, "!=", 2)) { op = 1; s += 2; }
    else if (!strncmp(s, "<=", 2)) { op = 3; s += 2; }
    else if (!strncmp(s, ">=", 2)) { op = 5; s += 2; }
    else if (*s == '=') { op = 0; s++; }
    else if (*s == '<') { op = 2; s++; }
    else if (*s == '>') { op = 4; s++; }
    if (op < 0) return false;
    uint32_t value;
    if (!dap_strict_num(s, s + strlen(s), &value)) return false;
    *out_operand = operand;
    *out_oaddr   = oaddr;
    *out_op      = op;
    *out_value   = value;
    return true;
}

// If a term is "bank[==]N" or "rombank[==]N" (case-insensitive), return the bank
// number; "bank==any" returns DAP_BANK_TERM_ANY; a bank term whose value cannot
// be read returns DAP_BANK_TERM_BAD; anything else -1. Used to pin a breakpoint
// to a specific RAM/ROM bank, or to keep it in every bank.
#define DAP_BANK_TERM_ANY (-2)
#define DAP_BANK_TERM_BAD (-3)
static int dap_parse_bank_term(const char *s) {
    while (*s == ' ') s++;
    if (dap_ci_prefix(s, "rombank")) s += 7;
    else if (dap_ci_prefix(s, "bank")) s += 4;
    else return -1;
    // The keyword must be followed by a separator / number / end — reject longer
    // identifiers like "banker" so they don't silently parse as bank 0.
    if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z')) return -1;
    while (*s == ' ' || *s == '=' || *s == '#') s++;
    if (!*s) return -1;
    if (dap_ci_prefix(s, "any") || *s == '*') {
        const char *r = s + (*s == '*' ? 1 : 3);
        while (*r == ' ') r++;
        return *r ? DAP_BANK_TERM_BAD : DAP_BANK_TERM_ANY;
    }
    uint32_t v;
    if (!dap_strict_num(s, s + strlen(s), &v) || v > 0xFF) return DAP_BANK_TERM_BAD;
    return (int)v;
}

// Parse an optional leading comparison then a number ("== $05", "!=0", "$05",
// "5"). Default op is EQ. Returns false if no number is present.
static bool dap_parse_simple_cmp(const char *s, int *out_op, uint32_t *out_val) {
    while (*s == ' ') s++;
    int op = 0; // default EQ
    if (!strncmp(s, "==", 2)) { op = 0; s += 2; }
    else if (!strncmp(s, "!=", 2)) { op = 1; s += 2; }
    else if (!strncmp(s, "<=", 2)) { op = 3; s += 2; }
    else if (!strncmp(s, ">=", 2)) { op = 5; s += 2; }
    else if (*s == '=') { op = 0; s++; }
    else if (*s == '<') { op = 2; s++; }
    else if (*s == '>') { op = 4; s++; }
    uint32_t v;
    if (!dap_strict_num(s, s + strlen(s), &v)) return false;
    *out_op  = op;
    *out_val = v;
    return true;
}

// The bank selector a source breakpoint is armed with: a "bank==N" term in its
// condition, else `default_bank`. Normalised as the core will store it -- on a
// gen2 with a non-zero program bank nothing above $A000 is banked either -- so
// the condition records are keyed the same way as the breakpoint.
static int dap_cond_bank_pin(dbg_addr_t addr, const char *cond_str, int default_bank) {
    int bank_pin = default_bank;
    if (cond_str && cond_str[0]) {
        char buf[192];
        strncpy(buf, cond_str, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char *scan = buf;
        while (scan && *scan) {
            char *amp = strstr(scan, "&&");
            if (amp) *amp = '\0';
            int b = dap_parse_bank_term(scan);
            if (b >= 0) bank_pin = b;
            else if (b == DAP_BANK_TERM_ANY) bank_pin = DEBUG_BANK_ANY;
            scan = amp ? amp + 2 : NULL;
        }
    }
    return debug_normalise_bank(bank_pin, (int)(addr & 0xFFFF), (uint8_t)(addr >> 16));
}

// What a condition and hit count ask the core for.
typedef struct {
    bool     has;
    int      operand;
    uint16_t oaddr;
    int      op;
    uint32_t value;
    uint32_t ignore;
} dap_cond_t;

// The value of a hitCondition: "N" or ">= N". Plain decimal only.
static bool dap_parse_hit(const char *h, uint32_t *n) {
    while (*h == ' ') h++;
    if (h[0] == '>' && h[1] == '=') h += 2;
    if (strspn(h, "0123456789 ") != strlen(h)) return false;
    return dap_strict_num(h, h + strlen(h), n);
}

static void dap_read_condition(const char *cond, const char *hit, dap_cond_t *out) {
    memset(out, 0, sizeof *out);
    if (cond && cond[0]) {
        char buf[192];
        strncpy(buf, cond, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        for (char *p = buf; p && *p && !out->has; ) {
            char *amp = strstr(p, "&&");
            if (amp) *amp = '\0';
            if (dap_parse_bank_term(p) == -1
                && dap_parse_value_term(p, &out->operand, &out->oaddr, &out->op, &out->value))
                out->has = true;
            p = amp ? amp + 2 : NULL;
        }
    }
    uint32_t n;
    if (hit && hit[0] && dap_parse_hit(hit, &n) && n > 0) out->ignore = n - 1;
}

// Parse a DAP source-breakpoint "condition"/"hitCondition" and apply it to the
// conditional-breakpoint core for the breakpoint at (addr, bank 0). The condition
// may combine a bank pin and a value test with "&&", e.g.
//   "bank == 0 && byte[$1234] != 0"  or  "A == $05"  or  "rombank==3".
// The value term (A/X/Y/SP/P, byte[$a], word[$a], $a  OP  value; only one is
// applied) drives the conditional-BP core; the bank term is returned so the
// caller can pin the breakpoint's x16Bank. hitCondition's number N means "stop
// on the Nth hit". Returns the bank pin (0..255), or -1 for any bank.
//
// `default_bank` is used when the condition names no bank: the RAM bank the
// debug info saw this code loaded into, or -1.
static int dap_apply_bp_condition(dbg_addr_t addr, const char *cond_str, const char *hit_str,
                                  int default_bank) {
    // The condition record is keyed on the same (pc, bank, x16Bank) triple as
    // the breakpoint. Applying it at bank 0 while the breakpoint is armed at
    // addr >> 16 wrote it under a key nothing looks up, and arrival then built
    // a fresh empty record -- so the breakpoint fired unconditionally on its
    // first hit.
    const uint16_t pc   = (uint16_t)(addr & 0xFFFF);
    const uint8_t  bank = (uint8_t)(addr >> 16);
    // Work out the bank pin FIRST, across the whole expression, before applying
    // anything keyed on it. The terms are applied in the order written, so
    // "A == 5 && bank == 3" used to store the value condition against
    // x16Bank = ANY while the breakpoint itself was armed for bank 3; arrival
    // then found no matching condition record and the breakpoint fired
    // unconditionally. Writing the same two terms the other way round worked,
    // which is not a distinction the user should be able to feel.
    const int bank_pin = dap_cond_bank_pin(addr, cond_str, default_bank);
    dap_cond_t want;
    dap_read_condition(cond_str, hit_str, &want);

    debug_bp_clear_condition(pc, bank, bank_pin);
    debug_bp_set_ignore(pc, bank, bank_pin, 0);
    // Stated in full every time, deliberately. Removing a breakpoint leaves its
    // condition record behind -- that is how a hit count survives an
    // enable/disable toggle -- so a client re-sending a breakpoint without a
    // condition would otherwise inherit the one it had before. The hit count
    // itself is left alone: an editor re-sends a whole file's breakpoints on
    // every keystroke, and a count that restarted each time could not drive a
    // hit condition at all.
    if (want.has)
        debug_bp_set_condition(pc, bank, bank_pin, want.operand, want.oaddr, want.op, want.value);
    if (want.ignore)
        debug_bp_set_ignore(pc, bank, bank_pin, want.ignore);   // stop on the Nth hit
    return bank_pin;
}

// A condition and hit count belong to the core entry, so one owner's would
// replace another's at a shared address. Called after the caller has dropped
// its own previous claims, so any entry still here belongs to someone else or
// to an earlier item in the same request.
static bool dap_bp_cond_conflicts(dbg_addr_t addr, int pin, const char *cond, const char *hit) {
    const int     pc   = (int)(addr & 0xFFFF);
    const uint8_t bank = (uint8_t)(addr >> 16);
    if (debug_bp_find(pc, bank, pin) < 0) return false;
    dap_cond_t want;
    dap_read_condition(cond, hit, &want);
    int has = 0, operand = 0, op = 0;
    uint16_t oaddr = 0;
    uint32_t value = 0, ignore = 0;
    debug_bp_get_condition(pc, bank, pin, &has, &operand, &oaddr, &op, &value, &ignore);
    if (ignore != want.ignore || (has != 0) != want.has) return true;
    return want.has && (operand != want.operand || oaddr != want.oaddr
                        || op != want.op || value != want.value);
}

// Can this condition and hit count be applied to a breakpoint at `addr`? When
// not, says why, and the caller must report it unverified and arm nothing.
static bool dap_bp_can_arm(dbg_addr_t addr, int default_bank, const char *cond, const char *hit,
                           char *why, size_t whyn) {
    int cond_bank = -1;
    if (!dap_check_condition(cond, &cond_bank, why, whyn)) return false;
    if (hit && hit[0] && !dap_check_hit_condition(hit)) {
        snprintf(why, whyn, "cannot parse hitCondition \"%.60s\" (expected N: stop from the Nth hit on)", hit);
        return false;
    }
    const int     pc   = (int)(addr & 0xFFFF);
    const uint8_t bank = (uint8_t)(addr >> 16);
    if (cond_bank >= 0 && debug_normalise_bank(cond_bank, pc, bank) == DEBUG_BANK_ANY) {
        snprintf(why, whyn, "$%04X is not in a banked window, so a bank cannot apply", (unsigned)pc);
        return false;
    }
    if (dap_bp_cond_conflicts(addr, dap_cond_bank_pin(addr, cond, default_bank), cond, hit)) {
        snprintf(why, whyn, "another breakpoint at $%04X has a different condition or hit count",
                 (unsigned)pc);
        return false;
    }
    return true;
}

// Report one requested source breakpoint as not armed.
static void dap_add_refused_bp(cJSON *arr, int id, int line, const char *path, const char *why) {
    cJSON *bp = cJSON_CreateObject();
    cJSON_AddNumberToObject(bp, "id", id);
    cJSON_AddBoolToObject(bp, "verified", false);
    cJSON_AddStringToObject(bp, "message", why);
    if (line > 0) cJSON_AddNumberToObject(bp, "line", dap_line_out(line));
    cJSON *src = cJSON_CreateObject();
    cJSON_AddStringToObject(src, "path", path);
    cJSON_AddItemToObject(bp, "source", src);
    cJSON_AddItemToArray(arr, bp);
}

static int handle_dap_set_breakpoints(int seq, cJSON *args) {
    if (!args) {
        send_dap_error_response(seq, "setBreakpoints", "missing arguments");
        return 0;
    }

    cJSON *source = cJSON_GetObjectItemCaseSensitive(args, "source");
    cJSON *bps = cJSON_GetObjectItemCaseSensitive(args, "breakpoints");
    cJSON *lines = cJSON_GetObjectItemCaseSensitive(args, "lines");
    if (!source) {
        send_dap_error_response(seq, "setBreakpoints", "missing source");
        return 0;
    }
    if ((bps && !cJSON_IsArray(bps)) || (!bps && lines && !cJSON_IsArray(lines))) {
        send_dap_error_response(seq, "setBreakpoints", "breakpoints and lines must be arrays");
        return 0;
    }
    // Only `source` is required. `lines` is the deprecated form of the list, and
    // neither being present means this file now has no breakpoints.
    cJSON *list = bps ? bps : lines;

    cJSON *path_item = cJSON_GetObjectItemCaseSensitive(source, "path");
    // Replies echo the client's own spelling of the path.
    const char *client_path = (path_item && cJSON_IsString(path_item)) ? path_item->valuestring : "";
    char src_buf[1024];
    dap_path_in(client_path, src_buf, sizeof src_buf);
    const char *src_path = src_buf;

    // Drop this file's breakpoints, then restate what this owner still wants.
    //
    // Removing them one at a time looks simpler and is wrong. Source files are
    // matched to addresses by basename, so src/main.s:10 and overlay/main.s:10
    // resolve to the SAME core entry -- while this loop matches on the full
    // path. One owner's two claims on an address are one bit, so removing the
    // first would disarm a breakpoint the other file still wants, leaving the
    // client showing a verified breakpoint that no longer stops anything.
    //
    // So: forget every source claim and re-assert the ones that survive. Other
    // owners are untouched throughout, and conditions and hit counts live on
    // the address rather than the claim, so nothing is lost by re-adding.
    for (int i = num_dap_bps - 1; i >= 0; i--) {
        if (strcmp(dap_bps[i].file, src_path) == 0) {
            for (int j = i; j < num_dap_bps - 1; j++) {
                dap_bps[j] = dap_bps[j + 1];
            }
            num_dap_bps--;
        }
    }

    debug_bp_clear_owner(DEBUG_OWNER_DAP_SOURCE);
    for (int i = 0; i < num_dap_bps; i++) {
        // A pending entry holds address 0 and was never armed; re-adding it
        // would arm an unrelated breakpoint at $0000.
        if (!dap_bps[i].verified)
            continue;
        struct breakpoint keep = { dap_bps[i].addr, dap_bps[i].bank, dap_bps[i].x16Bank, 0, false };
        debug_bp_add_for(keep, DEBUG_OWNER_DAP_SOURCE);
    }

    // Set new breakpoints
    cJSON *result_body = cJSON_CreateObject();
    cJSON *result_bps = cJSON_CreateArray();

    int bp_count = list ? cJSON_GetArraySize(list) : 0;
    for (int i = 0; i < bp_count; i++) {
        cJSON *bp_item = cJSON_GetArrayItem(list, i);
        cJSON *line_item = bps ? cJSON_GetObjectItemCaseSensitive(bp_item, "line") : bp_item;
        const bool has_line = line_item && cJSON_IsNumber(line_item);
        int line = has_line ? dap_line_in((int)line_item->valuedouble) : 0;
        int bp_id = next_dap_bp_id++;

        // Kept verbatim so the breakpoint can be re-armed exactly as asked for
        // if an overlay reload forces it to be resolved again.
        const char *cond_str = "";
        const char *hit_str  = "";
        const char *log_str  = "";
        if (bps) {
            cJSON *c = cJSON_GetObjectItemCaseSensitive(bp_item, "condition");
            if (c && cJSON_IsString(c) && c->valuestring) cond_str = c->valuestring;
            cJSON *h = cJSON_GetObjectItemCaseSensitive(bp_item, "hitCondition");
            if (h && cJSON_IsString(h) && h->valuestring) hit_str = h->valuestring;
            cJSON *m = cJSON_GetObjectItemCaseSensitive(bp_item, "logMessage");
            if (m && cJSON_IsString(m) && m->valuestring) log_str = m->valuestring;
        }

        // Every entry gets a reply, in order, whether or not it could be armed.
        char why[200];
        int  cond_bank = -1;
        if (!has_line) {
            dap_add_refused_bp(result_bps, bp_id, line, client_path, "breakpoint has no line");
            continue;
        }
        if (num_dap_bps >= MAX_DAP_BREAKPOINTS) {
            snprintf(why, sizeof why, "too many breakpoints (limit %d)", MAX_DAP_BREAKPOINTS);
            dap_add_refused_bp(result_bps, bp_id, line, client_path, why);
            continue;
        }
        if (strlen(src_path) >= sizeof dap_bps[0].file) {
            dap_add_refused_bp(result_bps, bp_id, line, client_path, "source path is too long");
            continue;
        }
        if (!dap_check_condition(cond_str, &cond_bank, why, sizeof why)) {
            dap_add_refused_bp(result_bps, bp_id, line, client_path, why);
            continue;
        }
        if (hit_str[0] && !dap_check_hit_condition(hit_str)) {
            snprintf(why, sizeof why,
                     "cannot parse hitCondition \"%.60s\" (expected N: stop from the Nth hit on)", hit_str);
            dap_add_refused_bp(result_bps, bp_id, line, client_path, why);
            continue;
        }
        if (log_str[0] && !dbg_log_check(log_str, why, sizeof why)) {
            dap_add_refused_bp(result_bps, bp_id, line, client_path, why);
            continue;
        }

        // Try to resolve source:line to address
        dbg_addr_t addr = 0;
        int seg_bank = -1;
        bool resolved = dbg_info_source_to_addr_bank(src_path, line, &addr, &seg_bank);

        if (resolved && !dap_bp_can_arm(addr, seg_bank, cond_str, hit_str, why, sizeof why)) {
            dap_add_refused_bp(result_bps, bp_id, line, client_path, why);
            continue;
        }

        cJSON *result_bp = cJSON_CreateObject();
        if (resolved) {
            // The condition is parsed first so a "bank==N" term pins x16Bank at
            // add time; the value term and hit count go to the core as well.
            int bank_pin = dap_apply_bp_condition(addr, cond_str, hit_str, seg_bank);
            struct breakpoint hw_bp;
            hw_bp.pc = (int)(addr & 0xFFFF);
            hw_bp.bank = (uint8_t)(addr >> 16);
            hw_bp.x16Bank = bank_pin;
            hw_bp.owners = 0;
            hw_bp.enabled = false;
            const debug_add_result_t added = debug_bp_add_for(hw_bp, DEBUG_OWNER_DAP_SOURCE);

            if (added == DEBUG_ADD_FULL) {
                // Nothing was armed, so say so rather than reporting a verified
                // breakpoint the machine will never stop on.
                cJSON_Delete(result_bp);
                dap_add_refused_bp(result_bps, bp_id, line, client_path,
                                   "Out of memory; breakpoint not armed");
                continue;
            }

            dap_bps[num_dap_bps].dap_id = bp_id;
            dap_bps[num_dap_bps].addr = (uint16_t)(addr & 0xFFFF);
            dap_bps[num_dap_bps].bank = (uint8_t)(addr >> 16);
            dap_bps[num_dap_bps].x16Bank = bank_pin;
            memcpy(dap_bps[num_dap_bps].file, src_path, strlen(src_path) + 1);   // length checked above
            dap_bps[num_dap_bps].line = line;
            dap_bps[num_dap_bps].verified = true;
            strncpy(dap_bps[num_dap_bps].cond, cond_str, sizeof(dap_bps[num_dap_bps].cond) - 1);
            dap_bps[num_dap_bps].cond[sizeof(dap_bps[num_dap_bps].cond) - 1] = '\0';
            strncpy(dap_bps[num_dap_bps].hit_cond, hit_str, sizeof(dap_bps[num_dap_bps].hit_cond) - 1);
            dap_bps[num_dap_bps].hit_cond[sizeof(dap_bps[num_dap_bps].hit_cond) - 1] = '\0';
            snprintf(dap_bps[num_dap_bps].log, sizeof dap_bps[num_dap_bps].log, "%s", log_str);
            num_dap_bps++;


            cJSON_AddNumberToObject(result_bp, "id", bp_id);
            cJSON_AddBoolToObject(result_bp, "verified", true);
            cJSON_AddNumberToObject(result_bp, "line", dap_line_out(line));
            dap_add_bank_message(result_bp, bank_pin);

            // Add source info
            cJSON *bp_source = cJSON_CreateObject();
            cJSON_AddStringToObject(bp_source, "path", client_path);
            cJSON_AddItemToObject(result_bp, "source", bp_source);
        } else {
            // Nothing describes this line yet -- typically the guest has not
            // loaded the program. Say so rather than claiming it is verified:
            // retry_unverified_breakpoints() arms it when the .dbg arrives and
            // sends a verified event then, so the client's view catches up on
            // its own. Reporting it verified here made an unarmable breakpoint
            // look identical to a working one.
            cJSON_AddNumberToObject(result_bp, "id", bp_id);
            cJSON_AddBoolToObject(result_bp, "verified", false);
            cJSON_AddStringToObject(result_bp, "message",
                                    "No debug info for this line yet; will arm when it loads");
            cJSON_AddNumberToObject(result_bp, "line", dap_line_out(line));
            cJSON *bp_source = cJSON_CreateObject();
            cJSON_AddStringToObject(bp_source, "path", client_path);
            cJSON_AddItemToObject(result_bp, "source", bp_source);

            // Store as pending — will set hardware BP when matching .dbg loads
            dap_bps[num_dap_bps].dap_id = bp_id;
            dap_bps[num_dap_bps].addr = 0;
            dap_bps[num_dap_bps].bank = 0;
            dap_bps[num_dap_bps].x16Bank = DEBUG_BANK_ANY;
            memcpy(dap_bps[num_dap_bps].file, src_path, strlen(src_path) + 1);   // length checked above
            dap_bps[num_dap_bps].line = line;
            dap_bps[num_dap_bps].verified = false;
            strncpy(dap_bps[num_dap_bps].cond, cond_str, sizeof(dap_bps[num_dap_bps].cond) - 1);
            dap_bps[num_dap_bps].cond[sizeof(dap_bps[num_dap_bps].cond) - 1] = '\0';
            strncpy(dap_bps[num_dap_bps].hit_cond, hit_str, sizeof(dap_bps[num_dap_bps].hit_cond) - 1);
            dap_bps[num_dap_bps].hit_cond[sizeof(dap_bps[num_dap_bps].hit_cond) - 1] = '\0';
            snprintf(dap_bps[num_dap_bps].log, sizeof dap_bps[num_dap_bps].log, "%s", log_str);
            num_dap_bps++;
        }
        cJSON_AddItemToArray(result_bps, result_bp);
    }
    dap_sync_source_logs();

    cJSON_AddItemToObject(result_body, "breakpoints", result_bps);
    send_dap_response(seq, "setBreakpoints", true, result_body);
    return 0;
}

static int handle_dap_continue(int seq, cJSON *args) {
    (void)args;
    // Cleared first so the resume hook stays quiet: this request sends its own
    // continued event after the response, and the hook exists for resumes the
    // client did not ask for.
    dap_stop_announced = false;
    DEBUGContinue();
    cJSON *body = cJSON_CreateObject();
    cJSON_AddBoolToObject(body, "allThreadsContinued", true);
    send_dap_response(seq, "continue", true, body);
    send_continued_event();
    return 1;
}

// Stepping is delegated to the debugger's own execution control rather than
// reimplemented here. Its step-over knows the call opcodes of every CPU variant
// the emulator supports -- the version this replaces recognised only JSR $20,
// so on a 65816 it stepped INTO a JSL -- and its step-out finds the return
// address by unwinding rather than assuming it is at SP+1, which is wrong the
// moment the routine has pushed anything. It also parks its breakpoint in
// `stepBreakPoint`, separate from the user's table, so stepping can no longer
// collide with a breakpoint the user set at the same address. That target
// carries its owner, so a session teardown can retract a step it asked for
// without touching one the user started at the keyboard.
//
// `granularity: instruction` is what a disassembly view sends. It has to be the
// plain step: the auto step runs a whole source line in C code, so that view
// could not otherwise move one instruction at a time.
static bool dap_step_by_instruction(cJSON *args) {
    cJSON *g = args ? cJSON_GetObjectItemCaseSensitive(args, "granularity") : NULL;
    return g && cJSON_IsString(g) && !strcmp(g->valuestring, "instruction");
}

static int handle_dap_next(int seq, cJSON *args) {
    note_request_resumes();   // this request implies execution continues
    if (dap_step_by_instruction(args))
        DEBUGStepOver(caller_step_owner());
    else
        DEBUGStepOverAuto(caller_step_owner());
    send_dap_response(seq, "next", true, NULL);
    return 1;
}

static int handle_dap_step_in(int seq, cJSON *args) {
    note_request_resumes();   // this request implies execution continues
    if (dap_step_by_instruction(args))
        DEBUGStepInto();
    else
        DEBUGStepIntoAuto(caller_step_owner());
    send_dap_response(seq, "stepIn", true, NULL);
    return 1;
}

static int handle_dap_step_out(int seq, cJSON *args) {
    (void)args;
    note_request_resumes();   // this request implies execution continues
    DEBUGStepOut(caller_step_owner());
    send_dap_response(seq, "stepOut", true, NULL);
    return 1;
}

static int handle_dap_pause(int seq, cJSON *args) {
    (void)args;
    const bool wasRunning = (currentMode != DMODE_STOP);
    // The protocol wants the response first, and the break below announces the
    // stop. Sending can drop the client, and halting after that would strand a
    // headless machine with nobody able to resume it.
    send_dap_response(seq, "pause", true, NULL);
    if (dap_reply_lost()) return 0;
    // Emits the stopped event itself on the RUN->STOP transition.
    DEBUGBreakToDebugger();
    // Already halted -- a previous client left it that way, or a breakpoint got
    // there first. There is no transition for DEBUGBreakToDebugger() to report,
    // and the protocol still owes this request a stopped event; without one the
    // client waits forever for a pause that has in fact already happened.
    if (!wasRunning && !dap_stop_announced) {
        debug_server_notify_stopped("pause");
    }
    return 1;
}

static void dap_quit_emulator(void) {
    SDL_Event quit_event;
    quit_event.type = SDL_QUIT;
    SDL_PushEvent(&quit_event);
}

static int handle_dap_disconnect(int seq, cJSON *args) {
    // A launched machine ends with its session. One the client attached to, or
    // never claimed at all, was not the client's to end, and neither is one
    // that another front end is still using, unless the client says so outright.
    bool terminate = dap_launched && !frontend_holds_run_state();
    if (args) {
        cJSON *term_item = cJSON_GetObjectItemCaseSensitive(args, "terminateDebuggee");
        if (term_item && cJSON_IsBool(term_item)) {
            terminate = cJSON_IsTrue(term_item);
        }
    }
    send_dap_response(seq, "disconnect", true, NULL);

    if (terminate) {
        dap_quit_emulator();
    } else {
        // Keep emulator running — prevent disconnect_client from killing it
        dap_session_active = false;
        // Release what the session installed BEFORE resuming. Resuming first
        // meant the machine ran on with this client's breakpoints still armed
        // until the socket actually closed, and stopping at one of them then
        // left it halted with the session already marked inactive -- so the
        // socket cleanup would not resume it either.
        dap_release_session_state();
        // Resume only when there is no interactive debugger UI to drive the run
        // state; otherwise leave it as the user left it (see disconnect_client).
        if (currentMode == DMODE_STOP && !dap_other_owner_of_run_state()) {
            currentMode = DMODE_RUN;
        }
    }
    return 0;
}

static int handle_dap_restart(int seq, cJSON *args) {
    (void)args;
    machine_reset();
    DEBUGCancelStep();   // a target from before the reset means nothing after it
    send_dap_response(seq, "restart", true, NULL);
    return 1;
}

// ─── Run to Cursor / Set Next Statement ────────────────────────────

static int handle_dap_goto_targets(int seq, cJSON *args) {
    // Returns possible goto targets for a source location
    if (!args) {
        send_dap_error_response(seq, "gotoTargets", "missing arguments");
        return 0;
    }

    cJSON *source = cJSON_GetObjectItemCaseSensitive(args, "source");
    cJSON *line_item = cJSON_GetObjectItemCaseSensitive(args, "line");
    if (!source || !line_item) {
        send_dap_error_response(seq, "gotoTargets", "missing source or line");
        return 0;
    }

    cJSON *path = cJSON_GetObjectItemCaseSensitive(source, "path");
    int line = dap_line_in(line_item->valueint);

    // Look up the address for this source line using debug info
    dbg_addr_t addr = 0;
    bool found = false;
    if (path && cJSON_IsString(path)) {
        char local[1024];
        dap_path_in(path->valuestring, local, sizeof local);
        found = dbg_info_source_to_addr(local, line, &addr);
    }

    cJSON *body = cJSON_CreateObject();
    cJSON *targets = cJSON_CreateArray();

    if (found) {
        cJSON *target = cJSON_CreateObject();
        cJSON_AddNumberToObject(target, "id", (int)addr);
        char label[32];
        snprintf(label, sizeof(label), "$%04X", addr);
        cJSON_AddStringToObject(target, "label", label);
        cJSON_AddNumberToObject(target, "line", dap_line_out(line));
        cJSON_AddItemToArray(targets, target);
    }

    cJSON_AddItemToObject(body, "targets", targets);
    send_dap_response(seq, "gotoTargets", true, body);
    return 0;
}

static int handle_dap_goto(int seq, cJSON *args) {
    // Set Next Statement: move PC to target address
    if (!args) {
        send_dap_error_response(seq, "goto", "missing arguments");
        return 0;
    }

    cJSON *target_id = cJSON_GetObjectItemCaseSensitive(args, "targetId");
    if (!target_id || !cJSON_IsNumber(target_id)) {
        send_dap_error_response(seq, "goto", "missing targetId");
        return 0;
    }

    // The stopped event below has to describe a real stop.
    if (currentMode != DMODE_STOP) {
        send_dap_error_response_full(seq, "goto", "notStopped",
                                     "the machine is running; pause it first", NULL, NULL);
        return 0;
    }

    // A target above $FFFF carries a 65C816 program bank.
    const uint32_t target = (uint32_t)target_id->valueint;
    regs.pc = (uint16_t)target;
    if (regs.is65c816 && target > 0xFFFF) regs.k = (uint8_t)(target >> 16);

    send_dap_response(seq, "goto", true, NULL);
    debug_server_notify_stopped("goto");
    return 0;
}

// The console commands that change the machine or the breakpoint tables. A
// hover or a watch re-evaluates on its own, so these must never run from one:
// resting the mouse on the word "reset" reset the machine.
static bool dap_eval_changes_state(const char *expr) {
    return !strncmp(expr, "bp_add ", 7) || !strncmp(expr, "bp_remove ", 10) ||
           !strcmp(expr, "bp_clear") || !strcmp(expr, "reset") ||
           !strncmp(expr, "watch_add ", 10) || !strncmp(expr, "watch_remove ", 13) ||
           !strcmp(expr, "watch_clear");
}

// "$hex", "0xhex" or hex with a leading zero, all of it. Text that merely starts
// like an address used to read as whatever digits it began with, so hovering on
// a name such as $ZZZZ showed the byte at $0000.
static bool dap_parse_eval_addr(const char *expr, uint32_t *addr) {
    const char *digits = expr;
    if (expr[0] == '$') digits = expr + 1;
    else if (expr[0] == '0' && (expr[1] == 'x' || expr[1] == 'X')) digits = expr + 2;
    if (!isxdigit((unsigned char)digits[0])) return false;
    if (digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) return false;
    char *end;
    const unsigned long v = strtoul(digits, &end, 16);
    if (*end) return false;
    *addr = (uint32_t)v & 0xFFFFFF;
    return true;
}

// vera_line LINE: both layers as they stood when that display scanline was
// drawn. The scroll registers are one generation behind the row at a split (see
// video_get_layer_line_state), so the row is reported rather than left for the
// reader to work out from them. On failure `out` holds the error message.
static bool dap_eval_vera_line(const char *arg, char *out, size_t size) {
    const unsigned count = video_get_scanline_count();
    char *end = NULL;
    unsigned long line = 0;
    if (isdigit((unsigned char)arg[0])) line = strtoul(arg, &end, 10);
    if (!end || *end || line >= count) {
        snprintf(out, size, "vera_line takes a decimal scanline from 0 to %u", count - 1);
        return false;
    }
    int len = snprintf(out, size, "line %lu:", line);
    for (uint8_t layer = 0; layer < 2; layer++) {
        uint8_t r[7];
        bool enabled = false;
        uint16_t row = 0;
        if (!video_get_layer_line_state(layer, (uint16_t)line, r, NULL, &enabled, &row)) {
            snprintf(out, size, "scanline %lu has not been rendered", line);
            return false;
        }
        len += snprintf(out + len, size - len,
                        "%s L%u %s row %u config $%02X map $%02X tile $%02X hscroll $%04X vscroll $%04X",
                        layer ? " |" : "", (unsigned)layer, enabled ? "on" : "off", (unsigned)row,
                        r[0], r[1], r[2], (unsigned)(r[3] | (r[4] << 8)),
                        (unsigned)(r[5] | (r[6] << 8)));
    }
    return true;
}

// ─── Expressions ─────────────────────────────────────────────────────

static void dap_fmt_expr_value(char *out, size_t n, const dbg_expr_value_t *v) {
    const unsigned u = v->value;
    if (v->bank >= 0)
        snprintf(out, n, "$%02X:%04X (%u)", (unsigned)v->bank, u, u);
    else
        snprintf(out, n, "$%0*X (%u)", u <= 0xFF ? 2 : u <= 0xFFFF ? 4 : u <= 0xFFFFFF ? 6 : 8, u, u);
}

// On failure `out` holds the reason.
static bool dap_eval_expression(const char *text, char *out, size_t n) {
    dbg_expr_t       expr;
    dbg_expr_value_t value;
    if (!dbg_expr_compile(text, dbg_machine_env(), &expr, out, n) ||
        !dbg_expr_eval(&expr, dbg_machine_env(), &value, out, n))
        return false;
    dap_fmt_expr_value(out, n, &value);
    return true;
}

static int handle_dap_evaluate(int seq, cJSON *args) {
    if (!args) {
        send_dap_error_response(seq, "evaluate", "missing arguments");
        return 0;
    }
    cJSON *expr_item = cJSON_GetObjectItemCaseSensitive(args, "expression");
    if (!expr_item || !cJSON_IsString(expr_item)) {
        send_dap_error_response(seq, "evaluate", "missing expression");
        return 0;
    }
    const char *expr = expr_item->valuestring;

    // A missing context is a console too: not every client sends one.
    cJSON *ctx_item = cJSON_GetObjectItemCaseSensitive(args, "context");
    const bool from_console = !ctx_item || !cJSON_IsString(ctx_item) ||
                              !strcmp(ctx_item->valuestring, "repl");
    if (!from_console && dap_eval_changes_state(expr)) {
        send_dap_error_response(seq, "evaluate", "this command only runs from the debug console");
        return 0;
    }

    // Parse expression: $XXXX = memory peek, register names, bp_* commands
    char result[1024] = "";
    uint32_t peek_addr = 0;
    dbg_addr_t sym_val = 0;
    int sym_bank = -1;

    // Breakpoint management via evaluate. bp_add and bp_remove take the same
    // address forms as an instructionReference, so "bb:aaaa" pins a bank. They
    // used to read hex up to the first non-digit, so "2:A000" meant $0002.
    struct breakpoint bp = { 0 };
    char where[16];
    const char *bp_arg = !strncmp(expr, "bp_add ", 7)     ? expr + 7
                       : !strncmp(expr, "bp_remove ", 10) ? expr + 10
                       : NULL;
    if (bp_arg && !dap_parse_instruction_ref(bp_arg, 0, &bp)) {
        snprintf(result, sizeof(result),
                 "not a breakpoint address: %s (use aaaa, or bb:aaaa for bank bb)", bp_arg);
    } else if (strncmp(expr, "bp_add ", 7) == 0) {
        dap_fmt_bp(where, sizeof where, bp.pc, bp.bank, bp.x16Bank);
        // Owned by the console, so it goes away with the session even when the
        // table below is full and nothing here remembers it. An untracked one
        // used to outlive the client that asked for it and could halt a
        // headless machine with nobody able to resume it.
        const debug_add_result_t added = debug_bp_add_for(bp, DEBUG_OWNER_DAP_CONSOLE);
        if (added == DEBUG_ADD_FULL) {
            snprintf(result, sizeof(result), "out of memory; no breakpoint at %s", where);
        } else {
            // Also recorded here, when there is room, so the breakpoint has an
            // id to report hits against.
            if (num_dap_bps < MAX_DAP_BREAKPOINTS) {
                dap_bps[num_dap_bps].dap_id  = next_dap_bp_id++;
                dap_bps[num_dap_bps].addr    = (uint16_t)bp.pc;
                dap_bps[num_dap_bps].bank    = bp.bank;
                dap_bps[num_dap_bps].x16Bank = bp.x16Bank;
                dap_bps[num_dap_bps].line    = 0;
                dap_bps[num_dap_bps].file[0] = '\0';
                dap_bps[num_dap_bps].cond[0] = '\0';
                dap_bps[num_dap_bps].hit_cond[0] = '\0';
                dap_bps[num_dap_bps].log[0] = '\0';
                dap_bps[num_dap_bps].verified = true;
                num_dap_bps++;
            }
            snprintf(result, sizeof(result), "breakpoint added at %s", where);
        }
    } else if (strncmp(expr, "bp_remove ", 10) == 0) {
        // "bb:aaaa" removes that bank's breakpoint; a plain address removes
        // every breakpoint at it, whichever bank each is pinned to.
        dap_fmt_bp(where, sizeof where, bp.pc, bp.bank, bp.x16Bank);
        const bool any = (bp.x16Bank == DEBUG_BANK_ANY);
        for (int i = num_dap_bps - 1; i >= 0; i--) {
            if (dap_bps[i].addr == bp.pc && dap_bps[i].bank == bp.bank
                && (any || dap_bps[i].x16Bank == bp.x16Bank)) {
                for (int j = i; j < num_dap_bps - 1; j++) dap_bps[j] = dap_bps[j + 1];
                num_dap_bps--;
            }
        }
        // Typed by a person, so it takes the breakpoint away whoever asked for
        // it -- the same rule as F9 in the debug window. Being told "removed"
        // and finding it still armed because something else also wanted it is
        // not an answer anyone can act on.
        int gone = 0;
        for (int i = debug_bp_count() - 1; i >= 0; i--) {
            const struct breakpoint *b = debug_bp_at(i);
            if (b->pc == bp.pc && b->bank == bp.bank && (any || b->x16Bank == bp.x16Bank)
                && debug_bp_delete(b->pc, b->bank, b->x16Bank))
                gone++;
        }
        if (gone)
            snprintf(result, sizeof(result), "%d breakpoint(s) removed at %s", gone, where);
        else
            snprintf(result, sizeof(result), "no breakpoint at %s", where);
    } else if (strcmp(expr, "bp_list") == 0) {
        // Each address in the form bp_add takes, so a bank pin is visible: it
        // used to print $A000 for a breakpoint pinned to bank 3.
        int len = 0;
        const int n = debug_bp_count();
        len += snprintf(result + len, sizeof(result) - len, "%d breakpoints:", n);
        for (int i = 0; i < n && len < (int)sizeof(result) - 16; i++) {
            const struct breakpoint *b = debug_bp_at(i);
            char at[16];
            dap_fmt_bp(at, sizeof at, b->pc, b->bank, b->x16Bank);
            len += snprintf(result + len, sizeof(result) - len, " %s%s",
                            at, b->enabled ? "" : "(off)");
        }
    } else if (strcmp(expr, "bp_clear") == 0) {
        // Typed by a person, and "clear all breakpoints" means all of them --
        // including any set with -bp or F9. The core's own clear takes the
        // condition and hit-count records with them; zeroing numBreakpoints
        // behind its back left those behind, and a breakpoint later re-created
        // at the same address inherited a stale hit count -- an "ignore 5" that
        // had already been reached fired on the second arrival, not the sixth.
        debug_bp_clear_all();
        num_dap_bps   = 0;
        num_func_bps  = 0;
        num_instr_bps = 0;
        snprintf(result, sizeof(result), "all breakpoints cleared");
    } else if (strcmp(expr, "reset") == 0) {
        machine_reset();
        snprintf(result, sizeof(result), "CPU reset");
    } else if ((expr[0] == '$' || expr[0] == '0') && dap_parse_eval_addr(expr, &peek_addr)) {
        // Memory address (24-bit: high byte is the 65C816 bank byte).
        uint8_t val = debug_read6502((uint16_t)(peek_addr & 0xFFFF), (uint8_t)(peek_addr >> 16),
                                     USE_CURRENT_X16_BANK);
        snprintf(result, sizeof(result), "$%02X (%d)", val, val);
    } else if (!strcmp(expr, "A") || !strcmp(expr, "a")) {
        if (regs.is65c816 && !regs.e && !(regs.status & FLAG_MEMORY_WIDTH))
            snprintf(result, sizeof(result), "$%04X (%d)", regs.c, regs.c);
        else
            snprintf(result, sizeof(result), "$%02X (%d)", regs.a, regs.a);
    } else if (!strcmp(expr, "X") || !strcmp(expr, "x")) {
        if (regs.is65c816 && !regs.e && !(regs.status & FLAG_INDEX_WIDTH))
            snprintf(result, sizeof(result), "$%04X (%d)", regs.x, regs.x);
        else
            snprintf(result, sizeof(result), "$%02X (%d)", (uint8_t)regs.x, (uint8_t)regs.x);
    } else if (!strcmp(expr, "Y") || !strcmp(expr, "y")) {
        if (regs.is65c816 && !regs.e && !(regs.status & FLAG_INDEX_WIDTH))
            snprintf(result, sizeof(result), "$%04X (%d)", regs.y, regs.y);
        else
            snprintf(result, sizeof(result), "$%02X (%d)", (uint8_t)regs.y, (uint8_t)regs.y);
    } else if (!strcmp(expr, "SP") || !strcmp(expr, "sp")) {
        snprintf(result, sizeof(result), "$%02X (%d)", (uint8_t)regs.sp, (uint8_t)regs.sp);
    } else if (!strcmp(expr, "PC") || !strcmp(expr, "pc")) {
        if (is_gen2 || (regs.is65c816 && regs.k != 0))
            snprintf(result, sizeof(result), "$%02X:%04X", regs.k, regs.pc);
        else
            snprintf(result, sizeof(result), "$%04X (%d)", regs.pc, regs.pc);
    } else if (!strcmp(expr, "P") || !strcmp(expr, "p")) {
        snprintf(result, sizeof(result), "$%02X", regs.status);
    } else if (!strcmp(expr, "regs_all")) {
        snprintf(result, sizeof(result), "PC=$%04X A=$%02X X=$%02X Y=$%02X SP=$%02X P=$%02X",
                 regs.pc, regs.a, (uint8_t)regs.x, (uint8_t)regs.y, (uint8_t)regs.sp, regs.status);
    } else if (strncmp(expr, "watch_add ", 10) == 0) {
        // watch_add ADDR [LEN]  — add memory write watchpoint
        char *endp;
        uint16_t addr = (uint16_t)strtoul(expr + 10, &endp, 16);
        uint16_t len = 1;
        if (*endp == ' ') len = (uint16_t)strtoul(endp + 1, NULL, 16);
        if (len == 0) len = 1;
        // Owned by the console, so it goes away with the session. An untracked
        // one outlived the client that made it and could halt a headless
        // machine with nobody able to resume it.
        const debug_add_result_t added =
            debug_wp_add_for(addr, len, DEBUG_BANK_ANY, DEBUG_OWNER_DAP_CONSOLE);
        if (added == DEBUG_ADD_FULL)
            snprintf(result, sizeof(result), "watchpoint table full (max %d)", MAX_WATCHPOINTS);
        else if (added == DEBUG_ADD_CONFLICT)
            snprintf(result, sizeof(result), "a read watch is already set at $%04X", addr);
        else
            snprintf(result, sizeof(result), "watchpoint added at $%04X len %d", addr, len);
    } else if (strncmp(expr, "watch_remove ", 13) == 0) {
        uint16_t addr = (uint16_t)strtoul(expr + 13, NULL, 16);
        // Typed by a person, so it takes the watch away whoever asked for it.
        if (debug_wp_delete(addr, DEBUG_BANK_ANY))
            snprintf(result, sizeof(result), "watchpoint removed at $%04X", addr);
        else
            snprintf(result, sizeof(result), "no watchpoint at $%04X", addr);
    } else if (!strcmp(expr, "watch_list")) {
        int len = 0;
        int n   = debug_wp_count();
        len += snprintf(result + len, sizeof(result) - len, "%d watchpoints:", n);
        for (int i = 0; i < n && len < (int)sizeof(result) - 20; i++) {
            const struct watchpoint *wp = debug_wp_at(i);
            if (!wp) break;
            len += snprintf(result + len, sizeof(result) - len,
                            wp->space == DEBUG_WP_SPACE_VRAM ? " vram:%05X(%u)" : " $%04X(%u)",
                            (unsigned)wp->addr, (unsigned)wp->len);
        }
    } else if (!strcmp(expr, "watch_clear")) {
        debug_wp_clear_all();   // the console command clears everyone's, ours included
        snprintf(result, sizeof(result), "all watchpoints cleared");
    } else if (strncmp(expr, "vram ", 5) == 0) {
        // vram ADDR [COUNT] — read VERA VRAM bytes (17-bit address)
        char *endp;
        uint32_t addr = (uint32_t)strtoul(expr + 5, &endp, 16);
        int count = 1;
        if (*endp == ' ') count = (int)strtoul(endp + 1, NULL, 10);
        if (count < 1) count = 1;
        if (count > 128) count = 128;
        int len = 0;
        len += snprintf(result + len, sizeof(result) - len, "$%05X:", addr);
        for (int i = 0; i < count && len < (int)sizeof(result) - 5; i++) {
            uint8_t val = video_space_read((addr + i) & 0x1FFFF);
            len += snprintf(result + len, sizeof(result) - len, " %02X", val);
        }
    } else if (!strncmp(expr, "vera_line", 9) && (expr[9] == ' ' || !expr[9])) {
        if (!dap_eval_vera_line(expr[9] ? expr + 10 : "", result, sizeof(result))) {
            send_dap_error_response(seq, "evaluate", result);
            return 0;
        }
    } else if (!strcmp(expr, "vera_reg")) {
        // vera_reg — dump all VERA registers ($9F20-$9F3F). Exact, so that a
        // program's own label beginning with these letters still resolves below.
        int len = 0;
        for (int i = 0; i < 32 && len < (int)sizeof(result) - 10; i++) {
            uint8_t val = vera_read_dbg((uint8_t)i);
            len += snprintf(result + len, sizeof(result) - len, "%02X ", val);
        }
    } else if (dbg_info_label_to_addr_bank(expr, &sym_val, &sym_bank)) {
        // Last, so a register or command of the same name keeps its meaning.
        if (sym_bank >= 0)
            snprintf(result, sizeof(result), "$%02X:%04X (label)", (unsigned)sym_bank, (unsigned)sym_val);
        else
            snprintf(result, sizeof(result), "$%04X (label)", (unsigned)sym_val);
    } else if (dbg_info_equate_to_value(expr, &sym_val)) {
        snprintf(result, sizeof(result), "$%04X (%u, equate)", (unsigned)sym_val, (unsigned)sym_val);
    } else if (!dap_eval_expression(expr, result, sizeof(result))) {
        // Last, so that every command and form above keeps its meaning.
        send_dap_error_response(seq, "evaluate", result);
        return 0;
    }

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "result", result);
    cJSON_AddNumberToObject(body, "variablesReference", 0);
    send_dap_response(seq, "evaluate", true, body);
    return 0;
}

// Base64, for the requests that carry binary. Returns NULL if out of memory.
static char *dap_base64(const uint8_t *data, size_t len) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *out = (char *)malloc((len + 2) / 3 * 4 + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        const size_t left = len - i;
        unsigned t = (unsigned)data[i] << 16;
        if (left > 1) t |= (unsigned)data[i + 1] << 8;
        if (left > 2) t |= data[i + 2];
        out[o++] = alphabet[(t >> 18) & 0x3F];
        out[o++] = alphabet[(t >> 12) & 0x3F];
        out[o++] = left > 1 ? alphabet[(t >> 6) & 0x3F] : '=';
        out[o++] = left > 2 ? alphabet[t & 0x3F] : '=';
    }
    out[o] = '\0';
    return out;
}

static int handle_dap_read_memory(int seq, cJSON *args) {
    if (!args) {
        send_dap_error_response(seq, "readMemory", "missing arguments");
        return 0;
    }
    cJSON *ref_item = cJSON_GetObjectItemCaseSensitive(args, "memoryReference");
    cJSON *count_item = cJSON_GetObjectItemCaseSensitive(args, "count");
    cJSON *offset_item = cJSON_GetObjectItemCaseSensitive(args, "offset");

    if (!ref_item || !cJSON_IsString(ref_item) || !count_item || !cJSON_IsNumber(count_item)) {
        send_dap_error_response(seq, "readMemory", "missing memoryReference or count");
        return 0;
    }

    uint32_t addr = 0;
    bool is_vram = false;
    if (!dap_parse_mem_ref(ref_item->valuestring, &is_vram, &addr)) {
        send_dap_error_response(seq, "readMemory", "unparseable memoryReference (expected a CPU address like 0xC000, or vram:1B000)");
        return 0;
    }
    int offset = (offset_item && cJSON_IsNumber(offset_item)) ? (int)offset_item->valuedouble : 0;
    const uint32_t addr_mask = is_vram ? 0x1FFFF : 0xFFFFFF;
    addr = (uint32_t)((int)addr + offset) & addr_mask;

    // What lies past the top of the space is unreadable, and is reported as
    // such: a short read with unreadableBytes 0 would say memory ends there.
    const double asked = count_item->valuedouble;
    const uint32_t room = addr_mask + 1 - addr;
    const int count = asked <= 0 ? 0 : asked >= (double)room ? (int)room : (int)asked;
    const double unreadable = asked > (double)count ? asked - (double)count : 0;

    // Optional: read banked RAM or ROM from a bank other than the current one.
    int x16_bank = USE_CURRENT_X16_BANK;
    cJSON *bank_item = cJSON_GetObjectItemCaseSensitive(args, "x16Bank");
    if (bank_item) {
        if (is_vram || !cJSON_IsNumber(bank_item) ||
            bank_item->valuedouble < 0 || bank_item->valuedouble > 255) {
            send_dap_error_response(seq, "readMemory", "'x16Bank' must be 0-255, and VRAM has no banks");
            return 0;
        }
        x16_bank = (int)bank_item->valuedouble;
    }

    unsigned char *raw = (unsigned char *)malloc((size_t)count + 1);
    if (!raw) {
        send_dap_error_response(seq, "readMemory", "memory allocation failed");
        return 0;
    }

    // 24-bit linear read: the high byte of the address is the 65C816 bank byte.
    // For bank 0 this is the classic X16 map (I/O + windowed banks following the
    // live mapping); banks 1-255 are flat RAM in gen2/GS. Spans bank boundaries.
    // VRAM is a separate 17-bit space behind VERA, so it wraps within itself.
    for (int i = 0; i < count; i++) {
        uint32_t cur = (addr + (uint32_t)i) & addr_mask;
        raw[i] = is_vram ? video_space_read(cur)
                         : debug_read6502((uint16_t)(cur & 0xFFFF), (uint8_t)(cur >> 16), x16_bank);
    }

    char *b64 = dap_base64(raw, (size_t)count);
    if (!b64) {
        free(raw);
        send_dap_error_response(seq, "readMemory", "memory allocation failed");
        return 0;
    }

    cJSON *body = cJSON_CreateObject();
    char addr_str[16];
    // `address` is a plain number by the protocol's definition. For VRAM the
    // reference to page with travels beside it, since the number alone would
    // read back as a CPU address.
    if (is_vram) {
        char ref[24];
        snprintf(addr_str, sizeof(addr_str), "0x%05X", addr & 0x1FFFF);
        snprintf(ref, sizeof ref, "vram:0x%05X", addr & 0x1FFFF);
        cJSON_AddStringToObject(body, "x16MemoryReference", ref);
    } else if (is_gen2 || addr > 0xFFFF) {
        snprintf(addr_str, sizeof(addr_str), "0x%06X", addr & 0xFFFFFF);
    } else {
        snprintf(addr_str, sizeof(addr_str), "0x%04X", addr & 0xFFFF);
    }
    cJSON_AddStringToObject(body, "address", addr_str);
    cJSON_AddNumberToObject(body, "unreadableBytes", unreadable);
    cJSON_AddStringToObject(body, "data", b64);

    free(raw);
    free(b64);
    send_dap_response(seq, "readMemory", true, body);
    return 0;
}

// Decode a single base64 character to its 6-bit value, or -1 if not a data char.
static int dap_b64_decode_char(char ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1; // '=', whitespace, or anything else
}

static int handle_dap_write_memory(int seq, cJSON *args) {
    if (!args) {
        send_dap_error_response(seq, "writeMemory", "missing arguments");
        return 0;
    }
    cJSON *ref_item = cJSON_GetObjectItemCaseSensitive(args, "memoryReference");
    cJSON *data_item = cJSON_GetObjectItemCaseSensitive(args, "data");
    cJSON *offset_item = cJSON_GetObjectItemCaseSensitive(args, "offset");

    if (!ref_item || !cJSON_IsString(ref_item) || !data_item || !cJSON_IsString(data_item)) {
        send_dap_error_response(seq, "writeMemory", "missing memoryReference or data");
        return 0;
    }

    uint32_t addr = 0;
    bool is_vram = false;
    if (!dap_parse_mem_ref(ref_item->valuestring, &is_vram, &addr)) {
        send_dap_error_response(seq, "writeMemory", "unparseable memoryReference (expected a CPU address like 0xC000, or vram:1B000)");
        return 0;
    }
    int offset = (offset_item && cJSON_IsNumber(offset_item)) ? (int)offset_item->valuedouble : 0;
    const uint32_t addr_mask = is_vram ? 0x1FFFF : 0xFFFFFF;
    addr = (uint32_t)((int)addr + offset) & addr_mask;

    const char *b64 = data_item->valuestring;
    int b64_len = (int)strlen(b64);
    unsigned char *raw = (unsigned char *)malloc((b64_len / 4) * 3 + 4);
    if (!raw) {
        send_dap_error_response(seq, "writeMemory", "memory allocation failed");
        return 0;
    }

    // Base64 decode
    int out = 0;
    int acc = 0, bits = -8;
    for (int i = 0; i < b64_len; i++) {
        int d = dap_b64_decode_char(b64[i]);
        if (d < 0) continue; // skip padding/whitespace
        acc = (acc << 6) | d;
        bits += 6;
        if (bits >= 0) {
            raw[out++] = (unsigned char)((acc >> bits) & 0xFF);
            bits -= 8;
        }
    }

    // Write into emulated RAM using 24-bit linear addressing: the high byte of
    // the address is the 65C816 bank byte (bank 0 = classic X16 map incl. the
    // windowed banks + I/O side effects; banks 1-255 = flat RAM in gen2/GS).
    // A vram: reference goes straight into VERA's 17-bit space instead, which is
    // the only way to write it -- the CPU reaches VRAM only through the data
    // port, one auto-incrementing byte at a time.
    for (int i = 0; i < out; i++) {
        uint32_t cur = (addr + (uint32_t)i) & addr_mask;
        if (is_vram) {
            video_space_write(cur, raw[i]);
        } else {
            debug_write6502((uint16_t)(cur & 0xFFFF), (uint8_t)(cur >> 16), raw[i]);
        }
    }

    free(raw);

    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "bytesWritten", out);
    cJSON_AddNumberToObject(body, "offset", 0);
    send_dap_response(seq, "writeMemory", true, body);
    return 0;
}

// ─── Custom: x16/sendKey — inject a key press for automated testing ──
//
// Arguments:
//   key:       SDL key name (e.g. "Return", "A", "Space", "Left", "F1") or a
//              numeric SDL scancode.
//   action:    "press" (default) sends down+up, "down" holds, "up" releases.
//   modifiers: optional array of key names/scancodes held around the key
//              (e.g. ["Left Shift"]). Applied as down before / up after.
static SDL_Scancode dap_resolve_scancode(cJSON *item) {
    if (!item) return SDL_SCANCODE_UNKNOWN;
    if (cJSON_IsNumber(item)) {
        int sc = (int)item->valuedouble;
        if (sc < 0 || sc >= SDL_NUM_SCANCODES) return SDL_SCANCODE_UNKNOWN;
        return (SDL_Scancode)sc;
    }
    if (cJSON_IsString(item) && item->valuestring && item->valuestring[0]) {
        return SDL_GetScancodeFromName(item->valuestring);
    }
    return SDL_SCANCODE_UNKNOWN;
}

static void dap_key_event(SDL_Scancode sc, bool down) {
    handle_keyboard(down, SDL_GetKeyFromScancode(sc), sc);
}

static int handle_x16_send_key(int seq, cJSON *args) {
    if (!args) {
        send_dap_error_response(seq, "x16/sendKey", "missing arguments");
        return 0;
    }
    SDL_Scancode sc = dap_resolve_scancode(cJSON_GetObjectItemCaseSensitive(args, "key"));
    if (sc == SDL_SCANCODE_UNKNOWN) {
        send_dap_error_response(seq, "x16/sendKey", "unknown or missing 'key'");
        return 0;
    }

    const char *action = "press";
    cJSON *action_item = cJSON_GetObjectItemCaseSensitive(args, "action");
    if (action_item && cJSON_IsString(action_item)) action = action_item->valuestring;

    // Resolve modifiers (held down around the key for press/down).
    SDL_Scancode mods[8];
    int num_mods = 0;
    cJSON *mod_arr = cJSON_GetObjectItemCaseSensitive(args, "modifiers");
    if (mod_arr && cJSON_IsArray(mod_arr)) {
        cJSON *m;
        cJSON_ArrayForEach(m, mod_arr) {
            if (num_mods >= 8) break;
            SDL_Scancode msc = dap_resolve_scancode(m);
            if (msc != SDL_SCANCODE_UNKNOWN) mods[num_mods++] = msc;
        }
    }

    bool do_down = (!strcmp(action, "press") || !strcmp(action, "down"));
    bool do_up   = (!strcmp(action, "press") || !strcmp(action, "up"));

    if (do_down) {
        for (int i = 0; i < num_mods; i++) dap_key_event(mods[i], true);
        dap_key_event(sc, true);
    }
    if (do_up) {
        dap_key_event(sc, false);
        for (int i = num_mods - 1; i >= 0; i--) dap_key_event(mods[i], false);
    }

    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "scancode", sc);
    cJSON_AddStringToObject(body, "action", action);
    send_dap_response(seq, "x16/sendKey", true, body);
    return 0;
}

// ─── Custom: x16/type — type a UTF-8/PETSCII string into the keyboard ──
//
// Arguments: text — the string to type. Routed through the same path as a
// clipboard paste, so it lands in the KERNAL keyboard buffer (ideal for BASIC
// lines, filenames, etc.). Supports the "\Xnn" hex escape the paste path does.
static int handle_x16_type(int seq, cJSON *args) {
    cJSON *text_item = args ? cJSON_GetObjectItemCaseSensitive(args, "text") : NULL;
    if (!text_item || !cJSON_IsString(text_item)) {
        send_dap_error_response(seq, "x16/type", "missing 'text'");
        return 0;
    }
    // machine_paste takes ownership and frees with SDL_free when handle_free=true.
    char *copy = SDL_strdup(text_item->valuestring);
    if (!copy) {
        send_dap_error_response(seq, "x16/type", "out of memory");
        return 0;
    }
    machine_paste(copy, true);

    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "length", (double)strlen(text_item->valuestring));
    send_dap_response(seq, "x16/type", true, body);
    return 0;
}

// ─── Custom: x16/joystick — drive a SNES controller port from a script ──
//
// Arguments:
//   index:   slot 0-3 (required).
//   buttons: array of names held down. The array is the complete held set, so
//            anything absent is released -- that is what lets a script hold a
//            direction across frames and then let go in a single request.
//   mask:    raw 16-bit active-low mask (0 = pressed), an alternative to
//            `buttons` for replaying a captured state verbatim.
//   enabled: false releases the slot back to a physical controller.
//
// A request carrying neither `buttons` nor `mask` reports the slot's state
// without changing it, so a test can assert what it just pressed.

static const struct {
    const char *name;
    uint16_t    bit;
    bool        alias;
} dap_joy_buttons[] = {
    {"up",     JOY_BIT_DPAD_UP,        false},
    {"down",   JOY_BIT_DPAD_DOWN,      false},
    {"left",   JOY_BIT_DPAD_LEFT,      false},
    {"right",  JOY_BIT_DPAD_RIGHT,     false},
    {"a",      JOY_BIT_A,              false},
    {"b",      JOY_BIT_B,              false},
    {"x",      JOY_BIT_X,              false},
    {"y",      JOY_BIT_Y,              false},
    {"start",  JOY_BIT_START,          false},
    {"select", JOY_BIT_SELECT,         false},
    {"l",      JOY_BIT_LEFT_SHOULDER,  false},
    {"r",      JOY_BIT_RIGHT_SHOULDER, false},
    // Aliases: accepted on input, never used to name a button on output.
    {"back",   JOY_BIT_SELECT,         true},
    {"lshoulder", JOY_BIT_LEFT_SHOULDER,  true},
    {"rshoulder", JOY_BIT_RIGHT_SHOULDER, true},
};

#define DAP_JOY_BUTTON_COUNT ((int)(sizeof(dap_joy_buttons) / sizeof(dap_joy_buttons[0])))

static bool dap_str_ieq(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++; b++;
    }
    return *a == *b;
}

static uint16_t dap_joy_bit_from_name(const char *name) {
    for (int i = 0; i < DAP_JOY_BUTTON_COUNT; i++) {
        if (dap_str_ieq(name, dap_joy_buttons[i].name)) return dap_joy_buttons[i].bit;
    }
    return 0;
}

// The held set, named. Active-low, so a cleared bit is a pressed button.
static cJSON *dap_joy_buttons_array(uint16_t mask) {
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < DAP_JOY_BUTTON_COUNT; i++) {
        if (dap_joy_buttons[i].alias) continue;
        if (!(mask & dap_joy_buttons[i].bit)) {
            cJSON_AddItemToArray(arr, cJSON_CreateString(dap_joy_buttons[i].name));
        }
    }
    return arr;
}

// A pad's button mask from "mask" and/or "buttons", active low: start from
// nothing pressed, so an absent button is a released one. Shared by
// x16/joystick and x16/step, so the two cannot drift apart.
static bool dap_joy_mask_from_args(cJSON *mask_item, cJSON *buttons_item, uint16_t *out,
                                   char *err, size_t errn) {
    if (buttons_item && !cJSON_IsArray(buttons_item)) {
        snprintf(err, errn, "'buttons' must be an array of names");
        return false;
    }
    if (mask_item && !cJSON_IsNumber(mask_item)) {
        snprintf(err, errn, "'mask' must be a number");
        return false;
    }
    uint16_t mask = 0xFFFF;
    if (mask_item) {
        mask = (uint16_t)(unsigned)mask_item->valuedouble;
    }
    if (buttons_item) {
        cJSON *b;
        cJSON_ArrayForEach(b, buttons_item) {
            if (!cJSON_IsString(b) || !b->valuestring) continue;
            uint16_t bit = dap_joy_bit_from_name(b->valuestring);
            if (bit == 0) {
                snprintf(err, errn, "unknown button '%.40s'", b->valuestring);
                return false;
            }
            mask &= (uint16_t)~bit;
        }
    }
    *out = mask;
    return true;
}

static int handle_x16_joystick(int seq, cJSON *args) {
    if (!args) {
        send_dap_error_response(seq, "x16/joystick", "missing arguments");
        return 0;
    }

    cJSON *index_item = cJSON_GetObjectItemCaseSensitive(args, "index");
    if (!index_item || !cJSON_IsNumber(index_item)) {
        send_dap_error_response(seq, "x16/joystick", "missing or non-numeric 'index'");
        return 0;
    }
    int index = (int)index_item->valuedouble;
    if (index < 0 || index >= NUM_JOYSTICKS) {
        send_dap_error_response(seq, "x16/joystick", "'index' out of range (expected 0-3)");
        return 0;
    }

    cJSON *enabled_item = cJSON_GetObjectItemCaseSensitive(args, "enabled");
    cJSON *mask_item    = cJSON_GetObjectItemCaseSensitive(args, "mask");
    cJSON *buttons_item = cJSON_GetObjectItemCaseSensitive(args, "buttons");

    // Validated whatever "enabled" says, so a bad name is never silently ignored.
    char err[96];
    uint16_t mask = 0xFFFF;
    if (!dap_joy_mask_from_args(mask_item, buttons_item, &mask, err, sizeof err)) {
        send_dap_error_response(seq, "x16/joystick", err);
        return 0;
    }

    if (enabled_item && cJSON_IsBool(enabled_item) && !cJSON_IsTrue(enabled_item)) {
        joystick_clear_virtual(index);
    } else if (buttons_item || mask_item) {
        joystick_set_virtual(index, mask);
    } else if (enabled_item && cJSON_IsBool(enabled_item)) {
        // enabled:true on its own plugs a controller in with nothing held. That
        // is a distinct state from an empty port -- a port being driven runs its
        // shift register out, so the KERNAL reports a controller present -- and
        // it is what a game that waits for one to be connected needs to see.
        joystick_debug_state_t cur;
        joystick_debug_get_state(&cur);
        joystick_set_virtual(index,
                             joystick_virtual_active(index) ? cur.slots[index].button_mask : 0xFFFF);
    }
    // Otherwise: no enabled/buttons/mask, so this is a query and changes nothing.

    joystick_debug_state_t js;
    joystick_debug_get_state(&js);

    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "index", index);
    cJSON_AddNumberToObject(body, "mask", js.slots[index].button_mask);
    cJSON_AddItemToObject(body, "buttons", dap_joy_buttons_array(js.slots[index].button_mask));
    cJSON_AddBoolToObject(body, "enabled", js.slots[index].virtual_active);
    cJSON_AddBoolToObject(body, "slotEnabled", js.slots[index].enabled);
    cJSON_AddBoolToObject(body, "controllerBound", js.slots[index].controller_bound);
    cJSON_AddNumberToObject(body, "shift", js.slots[index].shift_mask);
    cJSON_AddNumberToObject(body, "data", js.data);
    cJSON_AddBoolToObject(body, "latch", js.latch);
    send_dap_response(seq, "x16/joystick", true, body);
    return 0;
}

// ─── Custom: x16/registers — all CPU + KERNAL + VERA state ─────────

// ─── x16/status: where the machine is in emulated time ──────────────
//
// Cheap enough to call between every bot decision. JSON numbers are doubles,
// exact to 2^53: about 4.7 million years of frames, or 35 years of cycles at
// 8 MHz.
// A hash of the whole machine: the bytes a save state of it would hold, but for
// the build ID (savestate_hash), so two machines with the same hash would save
// the same state, whichever build of one machine version runs them.
// Sent as a hex string, since a JSON number cannot carry 64 bits exactly.
static void dap_add_hex64(cJSON *body, const char *key, uint64_t h) {
    char hex[24];
    snprintf(hex, sizeof hex, "%016llx", (unsigned long long)h);
    cJSON_AddStringToObject(body, key, hex);
}

static void dap_add_hash_of(cJSON *body, const void *data, size_t len) {
    dap_add_hex64(body, "hash", savestate_hash(data, len));
}

static void dap_add_state_hash(cJSON *body) {
    // Kept between calls: a client asking for the hash every frame should not
    // cost an allocation of the whole machine each time.
    static ss_writer w;
    static bool w_ready = false;
    if (!w_ready) {
        ss_writer_init(&w, false, NULL);
        w_ready = true;
    }
    ss_writer_rewind(&w);
    savestate_write(&w);
    if (w.failed) {
        cJSON_AddNullToObject(body, "hash");
        return;
    }
    dap_add_hash_of(body, w.data, w.len);
}

static bool dap_arg_true(cJSON *args, const char *name) {
    cJSON *v = args ? cJSON_GetObjectItemCaseSensitive(args, name) : NULL;
    return v && cJSON_IsTrue(v);
}

static const char *dap_wp_access_name(uint8_t access) {
    if (access == DEBUG_WP_ACCESS_BOTH) return "readWrite";
    return access == DEBUG_WP_ACCESS_READ ? "read" : "write";
}

void debug_server_add_stop_info(cJSON *body) {
    if (!DEBUGIsPaused()) return;
    debug_stop_exception_t ex;
    if (DEBUGGetStopException(&ex)) {
        cJSON *e = cJSON_AddObjectToObject(body, "stop");
        cJSON_AddStringToObject(e, "reason", "exception");
        cJSON_AddStringToObject(e, "kind", stop_filter_table[stop_filter_index(ex.kind)].filter);
        if (ex.has_vector) cJSON_AddNumberToObject(e, "vector", ex.vector);
        else               cJSON_AddNullToObject(e, "vector");
        cJSON_AddNumberToObject(e, "address", ex.address);
        cJSON_AddNumberToObject(e, "programBank", ex.bank);
        return;
    }
    debug_stop_info_t s;
    DEBUGGetStopInfo(&s);
    if (!s.has_access) return;
    cJSON *o = cJSON_AddObjectToObject(body, "stop");
    cJSON_AddStringToObject(o, "reason", s.reason);
    cJSON_AddStringToObject(o, "space", s.space == DEBUG_STOP_SPACE_VRAM ? "vram" : "cpu");
    cJSON_AddStringToObject(o, "access", s.access == DEBUG_STOP_ACCESS_READ ? "read" : "write");
    cJSON_AddBoolToObject(o, "fetch", s.fetch);
    cJSON_AddNumberToObject(o, "address", s.address);
    if (s.x16Bank >= 0) cJSON_AddNumberToObject(o, "bank", s.x16Bank);
    else                cJSON_AddNullToObject(o, "bank");
    cJSON_AddNumberToObject(o, "value", s.value);
    if (s.data_port >= 0) cJSON_AddNumberToObject(o, "dataPort", s.data_port);
    else                  cJSON_AddNullToObject(o, "dataPort");
    cJSON_AddNumberToObject(o, "accessingPc", s.accessing_pc);
    if (s.accessing_bank >= 0) cJSON_AddNumberToObject(o, "accessingBank", s.accessing_bank);
    else                       cJSON_AddNullToObject(o, "accessingBank");
}

static int handle_x16_status(int seq, cJSON *args) {
    const uint64_t cycles = emulated_cycles();
    const bool paused = DEBUGIsPaused();
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "frame", (double)machine_frames);
    cJSON_AddNumberToObject(body, "framesTotal", (double)process_frames);
    cJSON_AddNumberToObject(body, "cycles", (double)cycles);
    cJSON_AddBoolToObject(body, "paused", paused);
    // Only meaningful while paused: a running machine is never "at" anything.
    cJSON_AddBoolToObject(body, "atFrameBoundary",
                          paused && frame_end_seen && clockticks6502 == frame_end_ticks);
    cJSON_AddBoolToObject(body, "deterministic", deterministic_mode);
    if (random_seed_set) cJSON_AddNumberToObject(body, "seed", random_seed);
    else                 cJSON_AddNullToObject(body, "seed");
    // Which states this build loads: those of the same layout. One from another
    // machine version or toolchain loads, and x16/loadState says so (savestate.c).
    cJSON_AddNumberToObject(body, "layout", savestate_layout_version());
    cJSON_AddNumberToObject(body, "machine", savestate_machine_version());
    cJSON_AddStringToObject(body, "toolchain", savestate_toolchain());
    cJSON_AddStringToObject(body, "build", savestate_build_id());
    cJSON_AddStringToObject(body, "sourceId", savestate_source_id());
    if (dap_arg_true(args, "hash")) dap_add_state_hash(body);
    // The picture is output, not machine state, so it has its own hash.
    if (dap_arg_true(args, "screenHash")) dap_add_hex64(body, "screenHash", video_screen_hash());
    debug_server_add_stop_info(body);
    send_dap_response(seq, "x16/status", true, body);
    return 0;
}

// ─── x16/saveState, x16/loadState: save states ──────────────────────
//
// A state goes to a slot, a name the client picks, kept in memory; or to a
// file. Saving is allowed only at a frame boundary, so every state starts a
// frame and stepping on from one is unambiguous; loading works from any pause,
// since it replaces everything. Breakpoints, the session and the debugger's
// settings are configuration and are left alone.

static bool dap_at_frame_boundary(void) {
    return DEBUGIsPaused() && frame_end_seen && clockticks6502 == frame_end_ticks;
}

// ─── x16/vera: what VERA holds that its registers do not read back ──
//
// Both data ports whatever ADDRSEL says, and the FX state whatever DCSEL says.
// Copies only. The FX positions and increments are the emulator's 16.16 fixed
// point, and the field names say so.

static int handle_x16_vera(int seq, cJSON *args) {
    (void)args;
    struct video_state v;
    video_get_state(&v);

    cJSON *body = cJSON_CreateObject();
    cJSON *ports = cJSON_AddArrayToObject(body, "ports");
    for (int i = 0; i < 2; i++) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "address", v.port[i].address);
        cJSON_AddNumberToObject(p, "incrementCode", v.port[i].increment_code);
        cJSON_AddNumberToObject(p, "increment", v.port[i].increment);
        cJSON_AddNumberToObject(p, "prefetch", v.port[i].prefetch);
        cJSON_AddBoolToObject(p, "nibble", v.port[i].nibble);
        cJSON_AddBoolToObject(p, "nibbleIncrement", v.port[i].nibble_increment);
        cJSON_AddItemToArray(ports, p);
    }
    cJSON_AddNumberToObject(body, "addrsel", v.addrsel);
    cJSON_AddNumberToObject(body, "dcsel", v.dcsel);
    cJSON_AddNumberToObject(body, "ien", v.ien);
    cJSON_AddNumberToObject(body, "isr", v.isr);
    cJSON_AddNumberToObject(body, "irqLine", v.irq_line);
    cJSON_AddBoolToObject(body, "irqOut", v.irq_out);
    cJSON_AddBoolToObject(body, "ntsc", v.ntsc);
    cJSON_AddNumberToObject(body, "scanline", v.scanline);

    cJSON *fx = cJSON_AddObjectToObject(body, "fx");
    cJSON_AddNumberToObject(fx, "addr1Mode", v.fx_addr1_mode);
    cJSON_AddBoolToObject(fx, "fourBitMode", v.fx_4bit_mode);
    cJSON_AddBoolToObject(fx, "hop16", v.fx_16bit_hop);
    cJSON_AddNumberToObject(fx, "hopAlign", v.fx_16bit_hop_align);
    cJSON_AddBoolToObject(fx, "cacheByteCycling", v.fx_cache_byte_cycling);
    cJSON_AddBoolToObject(fx, "cacheFill", v.fx_cache_fill);
    cJSON_AddBoolToObject(fx, "cacheWrite", v.fx_cache_write);
    cJSON_AddBoolToObject(fx, "transparentWrites", v.fx_trans_writes);
    cJSON_AddBoolToObject(fx, "twoBitPoly", v.fx_2bit_poly);
    cJSON_AddBoolToObject(fx, "twoBitPoking", v.fx_2bit_poking);
    cJSON_AddBoolToObject(fx, "cacheIncrementMode", v.fx_cache_increment_mode);
    cJSON_AddBoolToObject(fx, "cacheNibbleIndex", v.fx_cache_nibble_index);
    cJSON_AddNumberToObject(fx, "cacheByteIndex", v.fx_cache_byte_index);
    cJSON_AddBoolToObject(fx, "multiplier", v.fx_multiplier);
    cJSON_AddBoolToObject(fx, "subtract", v.fx_subtract);
    cJSON_AddBoolToObject(fx, "affineClip", v.fx_affine_clip);
    cJSON_AddNumberToObject(fx, "xIncrement16_16", v.fx_x_increment);
    cJSON_AddNumberToObject(fx, "yIncrement16_16", v.fx_y_increment);
    cJSON_AddNumberToObject(fx, "xPosition16_16", v.fx_x_position);
    cJSON_AddNumberToObject(fx, "yPosition16_16", v.fx_y_position);
    cJSON_AddNumberToObject(fx, "polyFillLength", v.fx_poly_fill_length);
    cJSON_AddNumberToObject(fx, "affineTileBase", v.fx_affine_tile_base);
    cJSON_AddNumberToObject(fx, "affineMapBase", v.fx_affine_map_base);
    cJSON_AddNumberToObject(fx, "affineMapSize", v.fx_affine_map_size);
    cJSON *cache = cJSON_AddArrayToObject(fx, "cache");
    for (int i = 0; i < 4; i++) cJSON_AddItemToArray(cache, cJSON_CreateNumber(v.fx_cache[i]));
    cJSON_AddNumberToObject(fx, "accumulator", v.fx_accumulator);

    send_dap_response(seq, "x16/vera", true, body);
    return 0;
}

// ─── x16/screen, x16/screenshot: what the machine is showing ────────
//
// x16/screen reads the text layer back out of VRAM. x16/screenshot is the picture
// as it stands: VERA draws it a scanline at a time, so it is only a whole frame
// at a frame boundary, and under warp most frames are not drawn at all. The reply
// says both, rather than a client taking a torn or old image for the current one.

static int handle_x16_screen(int seq, cJSON *args) {
    cJSON *cs = args ? cJSON_GetObjectItemCaseSensitive(args, "charset") : NULL;
    cJSON *ly = args ? cJSON_GetObjectItemCaseSensitive(args, "layer") : NULL;

    screen_charset_t charset = SCREEN_CHARSET_PETSCII_UPPER;
    if (cs && (!cJSON_IsString(cs) || !screen_charset_parse(cs->valuestring, &charset))) {
        send_dap_error_response(seq, "x16/screen",
                                "'charset' must be \"petscii-upper\", \"petscii-lower\" or \"iso\"");
        return 0;
    }
    if (ly && (!cJSON_IsNumber(ly) || (ly->valuedouble != 0 && ly->valuedouble != 1))) {
        send_dap_error_response(seq, "x16/screen", "'layer' must be 0 or 1");
        return 0;
    }

    screen_text_layout_t layout;
    int layer = -1;
    if (ly) {
        layer = (int)ly->valuedouble;
        video_get_text_layout(layer, &layout);
        if (!layout.enabled || !layout.is_text) {
            send_dap_error_response(seq, "x16/screen", !layout.enabled ? "that layer is not being displayed"
                                                                       : "that layer is not in text mode");
            return 0;
        }
    } else {
        // Layer 1 first: it is the one the KERNAL's text screen uses.
        for (int i = 1; i >= 0 && layer < 0; i--) {
            video_get_text_layout(i, &layout);
            if (layout.enabled && layout.is_text) layer = i;
        }
        if (layer < 0) {
            send_dap_error_response(seq, "x16/screen", "neither layer is showing text; use x16/screenshot");
            return 0;
        }
    }

    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "layer", layer);
    cJSON_AddStringToObject(body, "charset", screen_charset_name(charset));
    cJSON_AddNumberToObject(body, "columns", layout.columns);
    cJSON_AddNumberToObject(body, "rows", layout.rows);
    cJSON *lines = cJSON_AddArrayToObject(body, "lines");
    for (int row = 0; row < layout.rows; row++) {
        char line[SCREEN_TEXT_LINE_MAX];
        screen_text_line(&layout, row, charset, video_space_read, line);
        cJSON_AddItemToArray(lines, cJSON_CreateString(line));
    }
    send_dap_response(seq, "x16/screen", true, body);
    return 0;
}

static int handle_x16_screenshot(int seq, cJSON *args) {
    (void)args;
    if (headless) {
        send_dap_error_response(seq, "x16/screenshot",
                                "a -testbench run has no video, so no picture; use -hidden instead");
        return 0;
    }
    size_t len = 0;
    int w = 0, h = 0;
    uint8_t *png = video_screenshot_png(&len, &w, &h);
    char *b64 = png ? dap_base64(png, len) : NULL;
    free(png);
    if (!b64) {
        send_dap_error_response(seq, "x16/screenshot", "could not encode the picture");
        return 0;
    }
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "data", b64);
    free(b64);
    cJSON_AddStringToObject(body, "mimeType", "image/png");
    cJSON_AddNumberToObject(body, "width", w);
    cJSON_AddNumberToObject(body, "height", h);
    cJSON_AddBoolToObject(body, "paused", DEBUGIsPaused());
    cJSON_AddNumberToObject(body, "frame", (double)machine_frames);
    cJSON_AddBoolToObject(body, "atFrameBoundary", dap_at_frame_boundary());
    cJSON_AddBoolToObject(body, "warp", warp_mode);
    send_dap_response(seq, "x16/screenshot", true, body);
    return 0;
}

// Where a request's state goes or comes from: a slot or a file, one of the two.
static bool dap_state_target(int seq, const char *cmd, cJSON *args,
                             const char **slot, const char **path) {
    cJSON *s = args ? cJSON_GetObjectItemCaseSensitive(args, "slot") : NULL;
    cJSON *p = args ? cJSON_GetObjectItemCaseSensitive(args, "path") : NULL;
    *slot = (s && cJSON_IsString(s)) ? s->valuestring : NULL;
    *path = (p && cJSON_IsString(p)) ? p->valuestring : NULL;
    if ((s && !*slot) || (p && !*path) || (*slot != NULL) == (*path != NULL)) {
        send_dap_error_response(seq, cmd, "give 'slot' (a name) or 'path' (a file), one of the two");
        return false;
    }
    return true;
}

static void dap_add_state_position(cJSON *body) {
    cJSON_AddNumberToObject(body, "frame", (double)machine_frames);
    cJSON_AddNumberToObject(body, "framesTotal", (double)process_frames);
    cJSON_AddNumberToObject(body, "cycles", (double)emulated_cycles());
}

static int handle_x16_save_state(int seq, cJSON *args) {
    const char *slot, *path;
    if (!dap_state_target(seq, "x16/saveState", args, &slot, &path)) {
        return 0;
    }
    if (step.pending || !DEBUGIsPaused()) {
        send_dap_error_response(seq, "x16/saveState", "the machine must be paused");
        return 0;
    }
    if (!dap_at_frame_boundary()) {
        send_dap_error_response(seq, "x16/saveState",
            "the machine is not at a frame boundary; x16/step {frames: 1} gets it to one");
        return 0;
    }
    char err[512];
    const size_t size = slot ? savestate_slot_save(slot, err, sizeof err)
                             : savestate_file_save(path, err, sizeof err);
    if (size == 0) {
        send_dap_error_response(seq, "x16/saveState", err);
        return 0;
    }
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, slot ? "slot" : "path", slot ? slot : path);
    cJSON_AddNumberToObject(body, "size", (double)size);
    dap_add_state_position(body);
    if (dap_arg_true(args, "hash")) {
        if (slot) {
            size_t len = 0;
            const void *data = savestate_slot_data(slot, &len);
            dap_add_hash_of(body, data, len);
        } else {
            // Saving changed nothing, so the machine hashes as the file does.
            dap_add_state_hash(body);
        }
    }
    send_dap_response(seq, "x16/saveState", true, body);
    return 0;
}

// Who saved a state, for x16/loadState's reply, refusals included.
static cJSON *dap_origin(const savestate_origin *o) {
    cJSON *m = cJSON_CreateObject();
    cJSON_AddNumberToObject(m, "saved", o->machine);
    cJSON_AddNumberToObject(m, "current", savestate_machine_version());
    cJSON_AddNumberToObject(m, "layout", o->layout);
    cJSON_AddStringToObject(m, "savedToolchain", o->toolchain);
    cJSON_AddStringToObject(m, "currentToolchain", savestate_toolchain());
    cJSON_AddBoolToObject(m, "behaviourChanged", o->machine_changed);
    cJSON_AddBoolToObject(m, "toolchainChanged", o->toolchain_changed);
    return m;
}

static int handle_x16_load_state(int seq, cJSON *args) {
    const char *slot, *path;
    if (!dap_state_target(seq, "x16/loadState", args, &slot, &path)) {
        return 0;
    }
    if (step.pending || !DEBUGIsPaused()) {
        send_dap_error_response(seq, "x16/loadState", "the machine must be paused");
        return 0;
    }
    char err[512];
    size_t size = 0;
    const bool allow = dap_arg_true(args, "allowBehaviourChange");
    const bool loaded = slot ? savestate_slot_load(slot, &size, err, sizeof err)
                             : savestate_file_load(path, &size, allow, err, sizeof err);
    if (!loaded) {
        // Who saved it goes with a refusal too, when the header named a known
        // writer, so a client can tell "another machine version" from damage.
        const savestate_origin *o = savestate_checked_origin();
        send_dap_error_response_with(seq, "x16/loadState", err, o ? "machine" : NULL, o ? dap_origin(o) : NULL);
        return 0;
    }
    // The debugger's idea of where the machine is stopped follows the load.
    // It is already stopped, so this announces nothing by itself.
    DEBUGBreakToDebugger();
    DEBUGSetStopReason("restore");

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, slot ? "slot" : "path", slot ? slot : path);
    cJSON_AddNumberToObject(body, "size", (double)size);
    dap_add_state_position(body);
    // Who saved it: another machine version loads only when asked for, and
    // another toolchain loads but may not run on exactly as it did then.
    const savestate_origin *o = savestate_last_origin();
    if (o) cJSON_AddItemToObject(body, "machine", dap_origin(o));
    if (dap_arg_true(args, "hash")) dap_add_state_hash(body);
    send_dap_response(seq, "x16/loadState", true, body);
    if (dap_arg_true(args, "events")) debug_server_notify_stopped("restore");
    return 0;
}

// The DAP server's part of a state: the machine's frame and cycle counts.
void debug_server_state_save(ss_writer *w) {
    ss_begin(w, "DAP ", 1);
    ss_put_u64(w, machine_frames);
    ss_put_u64(w, emulated_cycles());
    ss_end(w);
}

void debug_server_state_load(ss_reader *r) {
    ss_open(r, "DAP ");
    machine_frames = ss_get_u64(r);
    cycles_acc     = ss_get_u64(r);
    ss_close(r);
    // Measured against the restored clock from here on. A state is taken at a
    // frame boundary, which is where the loaded machine now stands.
    cycles_seen     = clockticks6502;
    frame_end_ticks = clockticks6502;
    frame_end_seen  = true;
}

// Standard base64, NUL-terminated. The caller frees it.
static char *dap_b64_encode(const uint8_t *raw, int count) {
    static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *b64 = (char *)malloc((size_t)((count + 2) / 3) * 4 + 1);
    if (!b64) return NULL;
    int n = 0;
    for (int i = 0; i < count; i += 3) {
        const int remaining = count - i;
        unsigned int triplet = (unsigned int)raw[i] << 16;
        if (remaining > 1) triplet |= (unsigned int)raw[i + 1] << 8;
        if (remaining > 2) triplet |= raw[i + 2];
        b64[n++] = b64chars[(triplet >> 18) & 0x3F];
        b64[n++] = b64chars[(triplet >> 12) & 0x3F];
        b64[n++] = (remaining > 1) ? b64chars[(triplet >> 6) & 0x3F] : '=';
        b64[n++] = (remaining > 2) ? b64chars[triplet & 0x3F] : '=';
    }
    b64[n] = '\0';
    return b64;
}

// x16/step reason names for what stopped the machine before the count ran out.
static const char *step_reason_for_stop(const char *stop) {
    if (!stop || !*stop)                     return "pause";
    if (!strcmp(stop, "user"))               return "pause";
    if (!strcmp(stop, "data breakpoint"))    return "watchpoint";
    return stop;   // breakpoint, step, interrupt, ...
}

// Send the deferred x16/step reply with the machine as it is now.
static void step_finish(const char *reason) {
    if (!step.pending) return;
    step.pending = false;

    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "framesRun", step.frames_run);
    cJSON_AddStringToObject(body, "reason", reason);
    cJSON_AddNumberToObject(body, "frame", (double)machine_frames);
    cJSON_AddNumberToObject(body, "framesTotal", (double)process_frames);
    cJSON_AddNumberToObject(body, "cycles", (double)emulated_cycles());

    cJSON *reads = cJSON_CreateArray();
    for (int r = 0; r < step.num_reads; r++) {
        const step_read_t *rd = &step.reads[r];
        uint8_t *raw = (uint8_t *)malloc(rd->len ? rd->len : 1);
        char *b64 = NULL;
        if (raw) {
            const uint32_t mask = rd->vram ? 0x1FFFF : 0xFFFFFF;
            for (uint32_t i = 0; i < rd->len; i++) {
                const uint32_t cur = (rd->addr + i) & mask;
                raw[i] = rd->vram ? video_space_read(cur)
                                  : debug_read6502((uint16_t)(cur & 0xFFFF), (uint8_t)(cur >> 16), rd->bank);
            }
            b64 = dap_b64_encode(raw, (int)rd->len);
        }
        cJSON_AddItemToArray(reads, cJSON_CreateString(b64 ? b64 : ""));
        free(b64);
        free(raw);
    }
    cJSON_AddItemToObject(body, "reads", reads);
    if (step.audio) {
        audio_events_end();
        cJSON *ym = cJSON_CreateArray();
        for (int i = 0; i < audio_events_ym_count(); i++) {
            const audio_ym_event *e = audio_events_ym(i);
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "frame", e->frame);
            cJSON_AddNumberToObject(o, "channel", e->channel);
            if (e->off) {
                cJSON_AddTrueToObject(o, "off");
            } else {
                cJSON_AddNumberToObject(o, "ops", e->ops);
                cJSON_AddNumberToObject(o, "kc", e->kc);
                cJSON_AddNumberToObject(o, "kf", e->kf);
                cJSON_AddNumberToObject(o, "voice", e->voice);
            }
            cJSON_AddItemToArray(ym, o);
        }
        cJSON_AddItemToObject(body, "ym", ym);
        if (audio_events_ym_dropped())
            cJSON_AddNumberToObject(body, "ymDropped", audio_events_ym_dropped());
        cJSON *pcm = cJSON_CreateArray();
        for (int i = 0; i < audio_events_pcm_count(); i++)
            cJSON_AddItemToArray(pcm, cJSON_CreateNumber(audio_events_pcm_frame(i)));
        cJSON_AddItemToObject(body, "pcm", pcm);
    }
    if (step.hash) dap_add_state_hash(body);

    if (step.caller != DBG_CALLER_DAP) {
        if (frontend.step_done) frontend.step_done(body);
        else cJSON_Delete(body);
        return;
    }
    // This can run in the middle of another front end's request, whose
    // `continue` is what cancelled the step. The reply is still the DAP client's.
    dbg_reply_t *const sink = reply_sink;
    const dbg_caller_t caller = current_caller;
    reply_sink = NULL;
    current_caller = DBG_CALLER_DAP;
    send_dap_response(step.seq, "x16/step", true, body);
    reply_sink = sink;
    current_caller = caller;
}

// Called from every poll. Completes a step once the machine has parked, for
// whatever reason: the count ran out, a breakpoint or watchpoint, a pause
// request, or the user breaking in.
static void step_poll(void) {
    if (!step.pending || currentMode != DMODE_STOP) return;
    const bool by_frames = step.by_frames;
    const char *reason = by_frames ? "frames" : step_reason_for_stop(DEBUGGetStopReason());
    if (by_frames) {
        // Refresh the debugger's idea of where it stopped, then record why. The
        // first call announces nothing, since the machine is already stopped.
        DEBUGBreakToDebugger();
        DEBUGCancelStep();
        DEBUGSetStopReason("step");
        // This is a frame boundary by construction. Recorded here rather than
        // trusting the hook's clock, which an interrupt taken in the same loop
        // pass has since moved on.
        frame_end_ticks = clockticks6502;
        frame_end_seen  = true;
    }
    const bool events = step.events;
    step_finish(reason);
    if (by_frames && events) debug_server_notify_stopped("step");
}

cJSON *debug_server_run_to_outcome(const dbg_run_target_t *t, bool completed, const char *reason) {
    const bool paused  = DEBUGIsPaused();
    const bool arrived = completed && paused && regs.pc == t->pc && regs.k == t->bank &&
                         debug_bank_selector_matches(t->x16Bank, t->pc, t->bank);
    if (!reason) {
        const char *stop = DEBUGGetStopReason();
        reason = (arrived && !strcmp(stop, "step")) ? "target" : step_reason_for_stop(stop);
    }
    cJSON *body = cJSON_CreateObject();
    cJSON_AddBoolToObject(body, "completed", completed);
    cJSON_AddBoolToObject(body, "arrived", arrived);
    cJSON_AddStringToObject(body, "reason", reason);
    cJSON_AddNumberToObject(body, "address", ((uint32_t)t->bank << 16) | t->pc);
    if (t->x16Bank != DEBUG_BANK_ANY) cJSON_AddNumberToObject(body, "bank", t->x16Bank);
    else                              cJSON_AddNullToObject(body, "bank");
    if (paused) {
        cJSON_AddNumberToObject(body, "pc", regs.pc);
        cJSON_AddNumberToObject(body, "pbr", regs.k);
    }
    return body;
}

static void run_to_finish(bool completed, const char *reason) {
    if (!run_to.pending) return;
    run_to.pending = false;
    cJSON *body = debug_server_run_to_outcome(&run_to.target, completed, reason);
    // This can run in the middle of another front end's request.
    dbg_reply_t *const sink = reply_sink;
    const dbg_caller_t caller = current_caller;
    reply_sink = NULL;
    current_caller = DBG_CALLER_DAP;
    send_dap_response(run_to.seq, "x16/runTo", true, body);
    reply_sink = sink;
    current_caller = caller;
}

// A timeout leaves the target armed and the machine going.
static void run_to_poll(void) {
    if (!run_to.pending) return;
    if (DEBUGIsPaused())
        run_to_finish(true, NULL);
    else if (DEBUGStepOwner() != DEBUG_OWNER_DAP_SOURCE)
        run_to_finish(false, "cancelled");
    else if (SDL_TICKS_PASSED(SDL_GetTicks(), run_to.deadline))
        run_to_finish(false, "timeout");
}

static bool dap_parse_step_read(cJSON *r, step_read_t *out, char *err, size_t errn) {
    cJSON *a = cJSON_GetObjectItemCaseSensitive(r, "addr");
    cJSON *l = cJSON_GetObjectItemCaseSensitive(r, "len");
    cJSON *b = cJSON_GetObjectItemCaseSensitive(r, "bank");
    if (!a || !l || !cJSON_IsNumber(l) || l->valuedouble < 1) {
        snprintf(err, errn, "each read needs an 'addr' and a 'len' of at least 1");
        return false;
    }
    out->vram = false;
    if (cJSON_IsNumber(a)) {
        out->addr = (uint32_t)a->valuedouble;
    } else if (!cJSON_IsString(a) || !dap_parse_mem_ref(a->valuestring, &out->vram, &out->addr)) {
        snprintf(err, errn, "unreadable read 'addr' (expected a CPU address like 0xC000, or vram:1B000)");
        return false;
    }
    if (b && (!cJSON_IsNumber(b) || out->vram)) {
        snprintf(err, errn, out->vram ? "a read of VRAM takes no 'bank'" : "'bank' must be a number");
        return false;
    }
    if (l->valuedouble > STEP_MAX_READ_BYTES) {
        snprintf(err, errn, "reads total more than %d bytes", STEP_MAX_READ_BYTES);
        return false;
    }
    out->len  = (uint32_t)l->valuedouble;
    out->bank = b ? (int)b->valuedouble : USE_CURRENT_X16_BANK;
    return true;
}

// Requires a paused machine. Joysticks apply before the first instruction runs, so
// at the frame boundary when the machine is sitting on one. A machine paused
// mid-frame counts the rest of that frame as the first.
static int handle_x16_step(int seq, cJSON *args) {
    char err[160];
    if (headless) {
        send_dap_error_response(seq, "x16/step", "a -testbench run has no video, so no frames to step; use -hidden instead");
        return 0;
    }
    if (step.pending) {
        send_dap_error_response(seq, "x16/step", "a step is already in progress");
        return 0;
    }
    if (!DEBUGIsPaused()) {
        send_dap_error_response(seq, "x16/step", "the machine must be paused");
        return 0;
    }

    uint32_t frames = 1;
    cJSON *fr = args ? cJSON_GetObjectItemCaseSensitive(args, "frames") : NULL;
    if (fr) {
        if (!cJSON_IsNumber(fr) || fr->valuedouble < 1 || fr->valuedouble > 1e9) {
            send_dap_error_response(seq, "x16/step", "'frames' must be a number of at least 1");
            return 0;
        }
        frames = (uint32_t)fr->valuedouble;
    }

    // Check everything before changing anything, so a refused request leaves
    // the machine exactly as it was.
    int      joy_index[NUM_JOYSTICKS];
    uint16_t joy_mask[NUM_JOYSTICKS];
    bool     joy_off[NUM_JOYSTICKS];
    int      num_joys = 0;
    cJSON *joys = args ? cJSON_GetObjectItemCaseSensitive(args, "joysticks") : NULL;
    if (joys) {
        if (!cJSON_IsArray(joys)) {
            send_dap_error_response(seq, "x16/step", "'joysticks' must be an array");
            return 0;
        }
        cJSON *p;
        cJSON_ArrayForEach(p, joys) {
            cJSON *ix = cJSON_GetObjectItemCaseSensitive(p, "index");
            bool ok = ix && cJSON_IsNumber(ix) && ix->valuedouble >= 0
                      && ix->valuedouble < NUM_JOYSTICKS && num_joys < NUM_JOYSTICKS;
            const int index = ok ? (int)ix->valuedouble : -1;
            for (int i = 0; ok && i < num_joys; i++) {
                if (joy_index[i] == index) ok = false;
            }
            if (!ok) {
                send_dap_error_response(seq, "x16/step", "each joystick needs an 'index' of 0-3, at most once each");
                return 0;
            }
            cJSON *en = cJSON_GetObjectItemCaseSensitive(p, "enabled");
            joy_index[num_joys] = index;
            joy_off[num_joys]   = en && cJSON_IsBool(en) && !cJSON_IsTrue(en);
            joy_mask[num_joys]  = 0xFFFF;
            if (!joy_off[num_joys] &&
                !dap_joy_mask_from_args(cJSON_GetObjectItemCaseSensitive(p, "mask"),
                                        cJSON_GetObjectItemCaseSensitive(p, "buttons"),
                                        &joy_mask[num_joys], err, sizeof err)) {
                send_dap_error_response(seq, "x16/step", err);
                return 0;
            }
            num_joys++;
        }
    }

    step_read_t reads[STEP_MAX_READS];
    int num_reads = 0;
    uint32_t read_bytes = 0;
    cJSON *rds = args ? cJSON_GetObjectItemCaseSensitive(args, "read") : NULL;
    if (rds) {
        if (!cJSON_IsArray(rds) || cJSON_GetArraySize(rds) > STEP_MAX_READS) {
            send_dap_error_response(seq, "x16/step", "'read' must be an array of at most 64 regions");
            return 0;
        }
        cJSON *r;
        cJSON_ArrayForEach(r, rds) {
            if (!dap_parse_step_read(r, &reads[num_reads], err, sizeof err)) {
                send_dap_error_response(seq, "x16/step", err);
                return 0;
            }
            read_bytes += reads[num_reads].len;
            if (read_bytes > STEP_MAX_READ_BYTES) {
                snprintf(err, sizeof err, "reads total more than %d bytes", STEP_MAX_READ_BYTES);
                send_dap_error_response(seq, "x16/step", err);
                return 0;
            }
            num_reads++;
        }
    }
    cJSON *ev = args ? cJSON_GetObjectItemCaseSensitive(args, "events") : NULL;

    // "audio": true, or {"keyOff": true} for key-offs too.
    cJSON *au        = args ? cJSON_GetObjectItemCaseSensitive(args, "audio") : NULL;
    bool   audio     = false;
    bool   audio_off = false;
    if (au) {
        if (cJSON_IsBool(au)) {
            audio = cJSON_IsTrue(au);
        } else if (cJSON_IsObject(au)) {
            cJSON *ko = cJSON_GetObjectItemCaseSensitive(au, "keyOff");
            if (ko && !cJSON_IsBool(ko)) {
                send_dap_error_response(seq, "x16/step", "'audio.keyOff' must be true or false");
                return 0;
            }
            audio     = true;
            audio_off = ko && cJSON_IsTrue(ko);
        } else {
            send_dap_error_response(seq, "x16/step", "'audio' must be true, false or {\"keyOff\": true}");
            return 0;
        }
    }

    for (int i = 0; i < num_joys; i++) {
        if (joy_off[i]) joystick_clear_virtual(joy_index[i]);
        else            joystick_set_virtual(joy_index[i], joy_mask[i]);
    }

    step.pending     = true;
    step.caller      = current_caller;
    step.seq         = seq;
    step.frames_left = frames;
    step.frames_run  = 0;
    step.by_frames   = false;
    // An editor watching a step it did not ask for still has to see it happen.
    step.events      = (ev && cJSON_IsTrue(ev)) ||
                       (current_caller != DBG_CALLER_DAP && client_sock != SOCKET_INVALID);
    step.hash        = dap_arg_true(args, "hash");
    step.audio       = audio;
    step.num_reads   = num_reads;
    memcpy(step.reads, reads, sizeof(reads[0]) * (size_t)num_reads);

    if (audio) audio_events_begin(audio_off);
    else       audio_events_end();

    // As continue does, but the continued event is optional: a client stepping
    // many times a second does not want two events per step.
    dap_stop_announced = false;
    DEBUGContinue();
    if (step.events) send_continued_event();
    return 1;
}

static int handle_x16_registers(int seq, cJSON *args) {
    (void)args;
    cJSON *body = cJSON_CreateObject();

    // CPU registers
    cJSON *cpu = cJSON_CreateObject();
    cJSON_AddNumberToObject(cpu, "pc", regs.pc);
    cJSON_AddNumberToObject(cpu, "a", regs.c);       // full 16-bit accumulator
    cJSON_AddNumberToObject(cpu, "x", regs.x);       // full 16-bit X
    cJSON_AddNumberToObject(cpu, "y", regs.y);       // full 16-bit Y
    cJSON_AddNumberToObject(cpu, "sp", regs.sp);
    cJSON_AddNumberToObject(cpu, "dp", regs.dp);     // direct page
    cJSON_AddNumberToObject(cpu, "dbr", regs.db);    // data bank
    cJSON_AddNumberToObject(cpu, "pbr", regs.k);     // program bank
    cJSON_AddNumberToObject(cpu, "status", regs.status);
    cJSON_AddNumberToObject(cpu, "e", regs.e);       // emulation flag
    cJSON_AddBoolToObject(cpu, "is65c816", regs.is65c816);
    cJSON_AddNumberToObject(cpu, "ramBank", memory_get_ram_bank());
    cJSON_AddNumberToObject(cpu, "romBank", memory_get_rom_bank());
    cJSON_AddItemToObject(body, "cpu", cpu);

    // KERNAL r0-r15 pseudo-registers (ZP $02-$21, 16-bit little-endian)
    cJSON *kregs = cJSON_CreateArray();
    for (int i = 0; i < 16; i++) {
        uint8_t lo = debug_read6502(0x02 + i * 2, 0, USE_CURRENT_X16_BANK);
        uint8_t hi = debug_read6502(0x03 + i * 2, 0, USE_CURRENT_X16_BANK);
        cJSON_AddItemToArray(kregs, cJSON_CreateNumber(lo | (hi << 8)));
    }
    cJSON_AddItemToObject(body, "kernal", kregs);

    // VERA registers ($9F20-$9F3F, read via video_read)
    cJSON *vera = cJSON_CreateObject();
    uint8_t addr0_l = video_read(0, true);
    uint8_t addr0_m = video_read(1, true);
    uint8_t addr0_h = video_read(2, true);
    uint32_t addr0 = addr0_l | (addr0_m << 8) | ((addr0_h & 0x01) << 16);
    uint8_t addr0_inc = (addr0_h >> 3) & 0x0F;
    cJSON_AddNumberToObject(vera, "addr0", addr0);
    cJSON_AddNumberToObject(vera, "addr0Inc", addr0_inc);

    uint8_t ctrl = video_read(5, true);
    cJSON_AddNumberToObject(vera, "ctrl", ctrl);

    // If ADDRSEL=1 in CTRL, read addr1 via the alt port
    // VERA ADDR1 is accessible when ADDRSEL bit is set
    // We read the raw register values for display
    cJSON_AddNumberToObject(vera, "ien", video_read(6, true));
    cJSON_AddNumberToObject(vera, "isr", video_read(7, true));
    cJSON_AddNumberToObject(vera, "irqLineL", video_read(8, true));

    // Display composer (DCSEL-banked in HW; read via video_get_dc_value so the
    // values are correct regardless of the guest's current DCSEL). These index
    // reg_composer[]: 0-3 = VIDEO/HSCALE/VSCALE/BORDER, 4-7 = HSTART/HSTOP/VSTART/VSTOP.
    cJSON_AddNumberToObject(vera, "dcVideo", video_get_dc_value(0));
    cJSON_AddNumberToObject(vera, "dcHScale", video_get_dc_value(1));
    cJSON_AddNumberToObject(vera, "dcVScale", video_get_dc_value(2));
    cJSON_AddNumberToObject(vera, "dcBorder", video_get_dc_value(3));
    cJSON_AddNumberToObject(vera, "dcHStartL", video_get_dc_value(4));
    cJSON_AddNumberToObject(vera, "dcHStopL", video_get_dc_value(5));
    cJSON_AddNumberToObject(vera, "dcVStartL", video_get_dc_value(6));
    cJSON_AddNumberToObject(vera, "dcVStopL", video_get_dc_value(7));

    // Layer 0 registers live at VERA $0D-$13 (reg_layer[0][0..6]); no side effects.
    cJSON *l0 = cJSON_CreateObject();
    cJSON_AddNumberToObject(l0, "config", video_read(0x0D, true));
    cJSON_AddNumberToObject(l0, "mapBase", video_read(0x0E, true));
    cJSON_AddNumberToObject(l0, "tileBase", video_read(0x0F, true));
    cJSON_AddNumberToObject(l0, "hScroll", video_read(0x10, true) | (video_read(0x11, true) << 8));
    cJSON_AddNumberToObject(l0, "vScroll", video_read(0x12, true) | (video_read(0x13, true) << 8));
    cJSON_AddItemToObject(vera, "l0", l0);

    // Layer 1 registers live at VERA $14-$1A (reg_layer[1][0..6]); no side effects.
    cJSON *l1 = cJSON_CreateObject();
    cJSON_AddNumberToObject(l1, "config", video_read(0x14, true));
    cJSON_AddNumberToObject(l1, "mapBase", video_read(0x15, true));
    cJSON_AddNumberToObject(l1, "tileBase", video_read(0x16, true));
    cJSON_AddNumberToObject(l1, "hScroll", video_read(0x17, true) | (video_read(0x18, true) << 8));
    cJSON_AddNumberToObject(l1, "vScroll", video_read(0x19, true) | (video_read(0x1A, true) << 8));
    cJSON_AddItemToObject(vera, "l1", l1);

    // Audio: AUDIO_CTRL ($1B) runs audio_render() on read, so suppress it
    // (reads back 0); AUDIO_RATE ($1C) is side-effect-free.
    cJSON_AddNumberToObject(vera, "audioCtrl", vera_read_dbg(0x1B));
    cJSON_AddNumberToObject(vera, "audioRate", video_read(0x1C, true));

    cJSON_AddItemToObject(body, "vera", vera);

    send_dap_response(seq, "x16/registers", true, body);
    return 0;
}

// ─── Performance budget (x16/perfStats, x16/perfConfig) ─────────────────────
// Exposes the guest cycle budget accounting from perf_budget.c so that tooling
// -- a VS Code extension, a CI job, a profiling script -- can watch for
// regressions without a human reading a panel.
//
// Statistics are answered while the machine runs free: nothing here stops it,
// and the server is polled from the emulation thread, so the numbers cannot be
// torn by a concurrent update.

static cJSON *perf_stat_json(const perf_stat_t *s) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "min", s->min);
    cJSON_AddNumberToObject(o, "mean", s->mean);
    cJSON_AddNumberToObject(o, "p50", s->p50);
    cJSON_AddNumberToObject(o, "p95", s->p95);
    cJSON_AddNumberToObject(o, "p99", s->p99);
    cJSON_AddNumberToObject(o, "max", s->max);
    return o;
}

static void perf_window_name(float seconds, char *out, size_t outsz) {
    if (seconds <= 0.0f) snprintf(out, outsz, "session");
    else                 snprintf(out, outsz, "%gs", (double)seconds);
}

// Accepts either a number of seconds or a name ("30s", "session"), so a client
// can send back the same window labels it was given.
static float perf_window_seconds(const cJSON *item) {
    if (cJSON_IsNumber(item)) return (float)item->valuedouble;
    if (cJSON_IsString(item) && item->valuestring) {
        if (!strcmp(item->valuestring, "session")) return 0.0f;
        return (float)atof(item->valuestring); // "30s" -> 30
    }
    return 0.0f;
}

static cJSON *perf_budget_json(void) {
    cJSON *b = cJSON_CreateObject();
    cJSON_AddBoolToObject(b, "enabled", perf_budget_is_enabled());
    cJSON_AddNumberToObject(b, "targetFps", perf_budget_get_target_fps());
    cJSON_AddNumberToObject(b, "vsyncHz", perf_budget_vsync_hz());
    cJSON_AddNumberToObject(b, "machineKhz", (double)MHZ * 1000.0);
    cJSON_AddNumberToObject(b, "cyclesPerFrame", perf_budget_cycles_per_frame());
    cJSON_AddNumberToObject(b, "cyclesPerScanline", perf_budget_cycles_per_scanline());
    cJSON_AddNumberToObject(b, "budgetCycles", perf_budget_budget_cycles());
    cJSON_AddNumberToObject(b, "framesPerBudget", perf_budget_frames_per_budget());
    cJSON_AddNumberToObject(b, "historyFrames", perf_budget_capacity());
    cJSON_AddNumberToObject(b, "totalFrames", perf_budget_total_frames());
    const char *mode = "auto";
    switch (perf_budget_get_idle_mode()) {
        case PERF_IDLE_MARKERS: mode = "markers"; break;
        case PERF_IDLE_NONE:    mode = "none";    break;
        default:                mode = "auto";    break;
    }
    cJSON_AddStringToObject(b, "idleMode", mode);
    cJSON_AddBoolToObject(b, "warpMode", warp_mode);
    return b;
}

static cJSON *perf_frame_json(const perf_frame_t *f) {
    cJSON *o = cJSON_CreateObject();
    const uint32_t budget = perf_budget_budget_cycles();
    cJSON_AddNumberToObject(o, "frame", f->frame);
    cJSON_AddNumberToObject(o, "totalCycles", f->total);
    cJSON_AddNumberToObject(o, "workCycles", perf_frame_work(f));
    cJSON_AddNumberToObject(o, "idleCycles", f->idle);
    cJSON_AddNumberToObject(o, "irqCycles", f->irq);
    cJSON_AddNumberToObject(o, "mainCycles",
                            perf_frame_work(f) > f->irq ? perf_frame_work(f) - f->irq : 0);
    cJSON_AddNumberToObject(o, "budgetWorkCycles", f->period_work);
    cJSON_AddNumberToObject(o, "instructions", f->instructions);
    cJSON_AddNumberToObject(o, "hostUs", f->host_us);
    cJSON_AddNumberToObject(o, "utilizationPct",
                            budget ? 100.0 * (double)f->period_work / (double)budget : 0.0);
    cJSON_AddBoolToObject(o, "overBudget", budget && f->period_work > budget);
    return o;
}

// VERA's side of the same question. Deliberately a sub-object of perfStats
// rather than a request of its own: a developer asking "what is eating my
// frame?" should get the CPU answer and the bus answer in one round trip,
// because the interesting cases are the ones where the two disagree.
//
// The headline is peakLineClocks against lineClocks (800), NOT a percentage of
// VERA's total bandwidth. A frame uses maybe a fifth of the chip's 100 MB/s and
// reporting that would be true and useless -- the scanline is the window that
// actually runs out. See src/vera_bandwidth.h.
static cJSON *vera_bandwidth_json(void) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enabled", vera_bandwidth_is_enabled());
    cJSON_AddNumberToObject(o, "lineClocks", VERA_BW_LINE_CLOCKS);
    cJSON_AddNumberToObject(o, "bytesPerAccess", VERA_BW_BYTES_PER_ACCESS);
    cJSON_AddNumberToObject(o, "accessClocks", VERA_BW_ACCESS_CLOCKS);
    cJSON_AddNumberToObject(o, "scanlinesPerFrame", vera_bandwidth_scanlines_per_frame());

    vera_bw_frame_t f;
    if (!vera_bandwidth_last_frame(&f)) {
        return o;
    }

    cJSON_AddNumberToObject(o, "frame", f.frame);
    cJSON_AddNumberToObject(o, "linesActive", f.lines_active);

    cJSON *layers = cJSON_CreateArray();
    for (int i = 0; i < 2; i++) {
        cJSON *l = cJSON_CreateObject();
        cJSON_AddNumberToObject(l, "fetches", f.layer_fetches[i]);
        cJSON_AddNumberToObject(l, "bytes", f.layer_bytes[i]);
        cJSON_AddNumberToObject(l, "peakLine", f.layer_peak_line[i]);
        cJSON_AddNumberToObject(l, "peakFetches", f.layer_peak_fetches[i]);
        cJSON_AddItemToArray(layers, l);
    }
    cJSON_AddItemToObject(o, "layers", layers);

    // Per-scanline pressure: the headline.
    cJSON_AddNumberToObject(o, "peakLine", f.peak_line);
    cJSON_AddNumberToObject(o, "peakLineClocks", f.peak_clocks);
    cJSON_AddNumberToObject(o, "p95LineClocks", f.p95_clocks);
    cJSON_AddNumberToObject(o, "meanLineClocks", f.mean_clocks);
    cJSON_AddNumberToObject(o, "linesOverBudget", f.lines_over);
    cJSON_AddNumberToObject(o, "peakLinePct",
                            100.0 * (double)f.peak_clocks / (double)VERA_BW_LINE_CLOCKS);
    cJSON_AddNumberToObject(o, "totalFetches", f.total_fetches);
    cJSON_AddNumberToObject(o, "totalClocks", f.total_clocks);

    // How much of each scanline was still free when the layers and the CPU had
    // taken their share. Sprites are last on the bus (vram_if.v:142-157), so
    // this is what they were really competing for.
    //
    // Named "bus" deliberately. Every clock figure in this object is BUS
    // OCCUPANCY, which is not the clock the sprite views report: sprite render
    // time is wall clock and runs about ten times higher, so a client that
    // subtracted one from the other would conclude a comfortable line was
    // oversubscribed. Convert sprite width and colour depth to fetches first.
    cJSON_AddNumberToObject(o, "spriteBusHeadroomClocksAtPeak",
                            f.peak_clocks < VERA_BW_LINE_CLOCKS
                                ? VERA_BW_LINE_CLOCKS - f.peak_clocks : 0);
    cJSON_AddStringToObject(o, "clockUnits", "bus");

    cJSON *port = cJSON_CreateObject();
    cJSON_AddNumberToObject(port, "readBytes", f.port_reads);
    cJSON_AddNumberToObject(port, "writeBytes", f.port_writes);
    cJSON_AddNumberToObject(port, "accesses", f.port_accesses);
    cJSON_AddNumberToObject(port, "vramBytes", f.port_vram_bytes);
    cJSON_AddNumberToObject(port, "fxCacheWrites", f.fx_cache_writes);
    cJSON_AddNumberToObject(port, "fxAffineFetches", f.fx_affine_fetches);
    // Bytes that reached VRAM per byte the CPU pushed. Above 1.0 means FX is
    // doing work the CPU did not pay for, which is the point of using it.
    const uint32_t stores = f.port_reads + f.port_writes;
    cJSON_AddNumberToObject(port, "amplification",
                            stores ? (double)f.port_vram_bytes / (double)stores : 0.0);
    cJSON_AddItemToObject(o, "port", port);

    return o;
}

static bool caller_read_path(const char *request, char *out, size_t out_size,
                             char *err, size_t errn);
static bool caller_write_path(const char *request, bool overwrite, char *out, size_t out_size,
                              char *err, size_t errn);

// A capture reads these same counters. An MCP client may not move them under
// one, whoever started it. `names` ends with NULL.
static bool refuse_during_capture(int seq, const char *cmd, const cJSON *args,
                                  const char *const *names) {
    if (current_caller != DBG_CALLER_MCP || !cap_running() || !args) return false;
    for (; *names; names++) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(args, *names);
        if (!item || (!strcmp(*names, "reset") && !cJSON_IsTrue(item))) continue;
        char why[120];
        snprintf(why, sizeof why, "'%s' cannot change while a capture is recording", *names);
        send_dap_error_response(seq, cmd, why);
        return true;
    }
    return false;
}

// ─── Whole-program histogram (x16/perfHist) ─────────────────────────────────
// Arguments, all optional, applied in this order:
//   enabled: bool        arm/disarm (allocates ~21 MB on first arm)
//   caller:  {start,end} the main-loop range whose JSRs open tasks
//   reset:   bool        zero everything
//   dump:    "path"      write the sparse histogram (see perf_hist.h)
//   overwrite: bool      an MCP client must pass this to replace a file
// The body always carries the totals and the task table.
static int handle_x16_perf_hist(int seq, cJSON *args) {
    static const char *const counters[] = {"enabled", "caller", "reset", NULL};
    // Judged first, so that a refused file changes nothing below.
    char dump_path[4200], err[300];
    cJSON *dump = args ? cJSON_GetObjectItemCaseSensitive(args, "dump") : NULL;
    const bool dumping = cJSON_IsString(dump) && dump->valuestring;
    if (dumping &&
        !caller_write_path(dump->valuestring,
                           cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(args, "overwrite")),
                           dump_path, sizeof dump_path, err, sizeof err)) {
        send_dap_error_response(seq, "x16/perfHist", err);
        return 0;
    }
    if (refuse_during_capture(seq, "x16/perfHist", args, counters)) return 0;

    cJSON *item = args ? cJSON_GetObjectItemCaseSensitive(args, "enabled") : NULL;
    if (cJSON_IsBool(item) && !perf_hist_arm(cJSON_IsTrue(item))) {
        send_dap_error_response(seq, "x16/perfHist", "could not allocate the histogram");
        return 0;
    }
    item = args ? cJSON_GetObjectItemCaseSensitive(args, "caller") : NULL;
    if (cJSON_IsObject(item)) {
        cJSON *js = cJSON_GetObjectItemCaseSensitive(item, "start");
        cJSON *je = cJSON_GetObjectItemCaseSensitive(item, "end");
        if (cJSON_IsNumber(js) && cJSON_IsNumber(je))
            perf_hist_set_caller((uint16_t)js->valuedouble, (uint16_t)je->valuedouble);
    }
    item = args ? cJSON_GetObjectItemCaseSensitive(args, "reset") : NULL;
    if (cJSON_IsTrue(item))
        perf_hist_reset();
    long dumped = -2;
    if (dumping)
        dumped = perf_hist_dump(dump_path);

    cJSON *body = cJSON_CreateObject();
    cJSON_AddBoolToObject(body, "enabled", perf_hist_enabled);
    cJSON_AddNumberToObject(body, "frames", perf_hist_frames());
    cJSON_AddNumberToObject(body, "cycles", (double)perf_hist_total_cycles());
    cJSON_AddNumberToObject(body, "irqCycles", (double)perf_hist_irq_cycles());
    cJSON_AddNumberToObject(body, "loopCycles", (double)perf_hist_loop_cycles());
    if (dumped != -2) {
        cJSON_AddNumberToObject(body, "dumped", (double)dumped);
        if (current_caller == DBG_CALLER_MCP && dumped >= 0)
            cJSON_AddStringToObject(body, "dumpPath", dump_path);
    }
    cJSON *tl = cJSON_CreateArray();
    for (int i = 0; i < perf_hist_task_count(); i++) {
        perf_hist_task_t t;
        if (!perf_hist_task_get(i, &t)) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "target", t.target);
        cJSON_AddNumberToObject(o, "ramBank", t.ram_bank);
        cJSON_AddNumberToObject(o, "calls", t.calls);
        cJSON_AddNumberToObject(o, "cycles", (double)t.cycles);
        cJSON_AddNumberToObject(o, "callMax", t.call_max);
        cJSON_AddNumberToObject(o, "frameMax", t.frame_max);
        cJSON_AddItemToArray(tl, o);
    }
    cJSON_AddItemToObject(body, "tasks", tl);
    send_dap_response(seq, "x16/perfHist", true, body);
    return 0;
}

// The capture an MCP client started, by cap_serial(); 0 for none. It is that
// client's to stop, and it ends when the client goes.
static uint32_t mcp_capture_serial = 0;

static bool capture_is_live(void) {
    return cap_state() == CAP_RUNNING || cap_state() == CAP_ARMED;
}

// ─── Capture mode (x16/capture) ─────────────────────────────────────────────
// Record a stretch of running with the debugger quiet, then read where the
// guest's time went, by routine. See profile_capture.h.
// Arguments, all optional, applied in this order:
//   start: {frames, seconds, onMarker, recorders: ["hist","budget","vram"]}
//          begin one (recorders default to ["hist"]); refused while one runs
//   stop:  true   end a running capture, or cancel one armed for a marker
//   load:  path   show a saved run in place of the last capture
//   top:   n      report rows to return (default 50; 0 = none)
//   export: {folded, json, trace, run}   files to write from the report
//   overwrite: bool   an MCP client must pass this to replace a file
// The body always carries the state; once a capture has ended, its report.
static int handle_x16_capture(int seq, cJSON *args) {
    static const char *const export_keys[] = {"folded", "json", "trace", "run"};
    enum { CAP_OUT_FOLDED, CAP_OUT_JSON, CAP_OUT_TRACE, CAP_OUT_RUN, CAP_OUT_KINDS };

    // Every file is judged before anything starts, stops or loads, so that a
    // refused one leaves the capture as it was.
    char   export_path[CAP_OUT_KINDS][4200], load_path[4200], path_err[300];
    bool   exporting[CAP_OUT_KINDS] = {false, false, false, false}, export_any = false;
    cJSON *ex = args ? cJSON_GetObjectItemCaseSensitive(args, "export") : NULL;
    const bool overwrite = args && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(args, "overwrite"));
    for (int i = 0; i < CAP_OUT_KINDS && cJSON_IsObject(ex); i++) {
        cJSON *p = cJSON_GetObjectItemCaseSensitive(ex, export_keys[i]);
        if (!cJSON_IsString(p)) continue;
        if (!caller_write_path(p->valuestring, overwrite, export_path[i], sizeof export_path[i],
                               path_err, sizeof path_err)) {
            send_dap_error_response(seq, "x16/capture", path_err);
            return 0;
        }
        exporting[i] = export_any = true;
    }
    cJSON *ld = args ? cJSON_GetObjectItemCaseSensitive(args, "load") : NULL;
    if (cJSON_IsString(ld) &&
        !caller_read_path(ld->valuestring, load_path, sizeof load_path, path_err, sizeof path_err)) {
        send_dap_error_response(seq, "x16/capture", path_err);
        return 0;
    }
    if (current_caller == DBG_CALLER_MCP && args &&
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(args, "stop")) && capture_is_live() &&
        cap_serial() != mcp_capture_serial) {
        send_dap_error_response(seq, "x16/capture", "that capture was started by someone else");
        return 0;
    }

    cJSON *start = args ? cJSON_GetObjectItemCaseSensitive(args, "start") : NULL;
    if (cJSON_IsObject(start)) {
        cap_config_t c = {0};
        cJSON *v = cJSON_GetObjectItemCaseSensitive(start, "frames");
        if (cJSON_IsNumber(v) && v->valuedouble > 0) c.frames = (uint32_t)v->valuedouble;
        v = cJSON_GetObjectItemCaseSensitive(start, "seconds");
        if (cJSON_IsNumber(v) && v->valuedouble > 0) c.seconds = v->valuedouble;
        v = cJSON_GetObjectItemCaseSensitive(start, "onMarker");
        c.on_marker = cJSON_IsTrue(v);
        cJSON *recs = cJSON_GetObjectItemCaseSensitive(start, "recorders");
        if (cJSON_IsArray(recs)) {
            cJSON *r;
            cJSON_ArrayForEach(r, recs) {
                const uint32_t bit = cJSON_IsString(r) ? profile_session_recorder_bit(r->valuestring) : 0;
                if (!bit) {
                    send_dap_error_response(seq, "x16/capture",
                                            "unknown recorder (expected hist, tree, budget, vram or waterfall)");
                    return 0;
                }
                c.recorders |= bit;
            }
        } else {
            c.recorders = CAP_REC_HIST | CAP_REC_TREE;
        }
        if (cap_state() == CAP_RUNNING || cap_state() == CAP_ARMED) {
            send_dap_error_response(seq, "x16/capture", "a capture is already running or armed");
            return 0;
        }
        if (!cap_start(&c) && cap_state() != CAP_ARMED) {
            send_dap_error_response(seq, "x16/capture", "the recorders could not be armed");
            return 0;
        }
        if (current_caller == DBG_CALLER_MCP) mcp_capture_serial = cap_serial();
    }
    cJSON *stop = args ? cJSON_GetObjectItemCaseSensitive(args, "stop") : NULL;
    if (cJSON_IsTrue(stop))
        cap_stop();
    // load: path -- show a saved run (profile_run.h) in place of the last one.
    char   run_msg[1200] = "";
    if (cJSON_IsString(ld) && profile_session_load_run(load_path, run_msg, sizeof run_msg) != 0) {
        send_dap_error_response(seq, "x16/capture", run_msg);
        return 0;
    }
    if (current_caller == DBG_CALLER_MCP && export_any &&
        !(profile_session_report() && (cap_state() == CAP_DONE || profile_session_run_loaded()))) {
        send_dap_error_response(seq, "x16/capture", "there is no finished capture to export");
        return 0;
    }
    int top = 50;
    cJSON *t = args ? cJSON_GetObjectItemCaseSensitive(args, "top") : NULL;
    if (cJSON_IsNumber(t) && t->valuedouble >= 0)
        top = (int)t->valuedouble;

    static const char *const state_names[] = {"idle", "armed", "running", "done"};
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "state", state_names[cap_state()]);
    cJSON_AddNumberToObject(body, "frames", cap_frames());
    cJSON_AddNumberToObject(body, "elapsedSeconds", cap_elapsed_s());
    if (cap_state() == CAP_DONE)
        cJSON_AddStringToObject(body, "ended", cap_end_name(cap_end_reason()));
    cJSON *recs = cJSON_CreateArray();
    for (uint32_t bit = 1; bit <= CAP_REC_LAST; bit <<= 1)
        if (cap_recorders() & bit)
            cJSON_AddItemToArray(recs, cJSON_CreateString(profile_session_recorder_name(bit)));
    cJSON_AddItemToObject(body, "recorders", recs);

    if (profile_session_run_loaded())
        cJSON_AddStringToObject(body, "loadedRun", profile_session_run_path());
    // A .dbg loaded while the machine was stopped: name the report from it.
    profile_session_sync_names();
    const prof_report_t *r = profile_session_report();
    if (r && (cap_state() == CAP_DONE || profile_session_run_loaded())) {
        cJSON *rep = cJSON_CreateObject();
        const uint64_t total  = r->main_cycles + r->irq_cycles;
        const uint32_t frames = profile_session_report_frames();
        cJSON_AddNumberToObject(rep, "frames", frames);
        cJSON_AddNumberToObject(rep, "mainCycles", (double)r->main_cycles);
        cJSON_AddNumberToObject(rep, "irqCycles", (double)r->irq_cycles);
        cJSON_AddNumberToObject(rep, "instructions", (double)r->instructions);
        cJSON_AddNumberToObject(rep, "rowCount", r->count);
        cJSON_AddNumberToObject(rep, "namedRows", r->named_rows);
        cJSON_AddNumberToObject(rep, "ambiguousRows", r->ambiguous_rows);
        cJSON *rows = cJSON_CreateArray();
        static const char *const regions[] = {"low", "ram", "rom"};
        for (int i = 0; i < r->count && i < top; i++) {
            const prof_row_t *row = &r->rows[i];
            const uint64_t    tc  = row->main_cycles + row->irq_cycles;
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "name", row->name);
            cJSON_AddStringToObject(o, "region", regions[row->region < 3 ? row->region : 0]);
            cJSON_AddNumberToObject(o, "bank", row->bank);
            cJSON_AddNumberToObject(o, "addr", row->addr);
            cJSON_AddBoolToObject(o, "named", row->named);
            cJSON_AddBoolToObject(o, "ambiguous", row->ambiguous);
            cJSON_AddNumberToObject(o, "mainCycles", (double)row->main_cycles);
            cJSON_AddNumberToObject(o, "irqCycles", (double)row->irq_cycles);
            cJSON_AddNumberToObject(o, "instructions", (double)row->instructions);
            cJSON_AddNumberToObject(o, "percent", total ? 100.0 * (double)tc / (double)total : 0.0);
            cJSON_AddNumberToObject(o, "perFrame", frames ? (double)tc / frames : 0.0);
            if (r->node_count > 2) {
                cJSON_AddNumberToObject(o, "inclCycles", (double)row->incl_cycles);
                cJSON_AddNumberToObject(o, "inclPercent",
                                        total ? 100.0 * (double)row->incl_cycles / (double)total : 0.0);
                cJSON_AddNumberToObject(o, "calls", row->calls);
                cJSON_AddNumberToObject(o, "callMax", row->call_max);
                cJSON_AddNumberToObject(o, "frameMax", row->frame_max);
            }
            cJSON_AddItemToArray(rows, o);
        }
        cJSON_AddItemToObject(rep, "rows", rows);
        if (r->node_count > 2) {
            // The call tree, flat: parents before children, so a client can
            // rebuild it from `parent`. Nodes under 0.1% inclusive are left
            // out (with their subtrees), which keeps the reply small.
            cJSON *tree = cJSON_CreateArray();
            int   *out_index = malloc(sizeof *out_index * (size_t)r->node_count);
            int    kept = 0;
            for (int i = 0; out_index && i < r->node_count; i++) {
                const prof_node_t *n = &r->nodes[i];
                out_index[i] = -1;
                const bool root = n->parent < 0;
                if (!root && (out_index[n->parent] < 0 || n->incl * 1000 < total))
                    continue;
                char name[PROF_NAME_MAX + 16];
                profile_session_node_name(r, i, name, sizeof name);
                cJSON *o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "name", name);
                cJSON_AddNumberToObject(o, "parent", root ? -1 : out_index[n->parent]);
                cJSON_AddNumberToObject(o, "self", (double)n->self);
                cJSON_AddNumberToObject(o, "incl", (double)n->incl);
                cJSON_AddNumberToObject(o, "calls", n->calls);
                cJSON_AddNumberToObject(o, "callMax", n->call_max);
                cJSON_AddNumberToObject(o, "frameMax", n->frame_max);
                cJSON_AddItemToArray(tree, o);
                out_index[i] = kept++;
            }
            free(out_index);
            cJSON_AddItemToObject(rep, "tree", tree);
            cJSON_AddBoolToObject(rep, "treeConsistent", profile_session_tree_consistent());
        }
        if (profile_session_has_waterfall()) {
            // Writes per (routine, lane) over the whole capture, heaviest first.
            int                  na;
            const prof_writes_t *ag = profile_session_writes_by_routine(&na);
            cJSON *dw = cJSON_CreateArray();
            for (int i = 0; i < na && i < top; i++) {
                const int row = ag[i].row;
                cJSON *o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "routine", row >= 0 ? r->rows[row].name : "?");
                cJSON_AddStringToObject(o, "lane", wf_lane_name(ag[i].lane));
                cJSON_AddNumberToObject(o, "writes", ag[i].writes);
                cJSON_AddItemToArray(dw, o);
            }
            cJSON_AddItemToObject(rep, "deviceWrites", dw);
            cJSON_AddNumberToObject(rep, "waterfallFrames", wf_frame_count());
            cJSON_AddNumberToObject(rep, "waterfallDetailFrames", wf_detail_count());
        }
        cJSON_AddItemToObject(body, "report", rep);

        // export: {folded: path, json: path} -- files written by the emulator.
        if (cJSON_IsObject(ex)) {
            cJSON *done = cJSON_CreateObject();
            if (exporting[CAP_OUT_FOLDED])
                cJSON_AddNumberToObject(done, "folded", profile_session_export_folded(export_path[CAP_OUT_FOLDED]));
            if (exporting[CAP_OUT_JSON])
                cJSON_AddNumberToObject(done, "json", profile_session_export_json(export_path[CAP_OUT_JSON]));
            if (exporting[CAP_OUT_TRACE])
                cJSON_AddNumberToObject(done, "trace", profile_session_export_trace(export_path[CAP_OUT_TRACE]));
            // run: a saved run, to open again later (0 written, -1 failed).
            if (exporting[CAP_OUT_RUN]) {
                char m[1200];
                cJSON_AddNumberToObject(done, "run", profile_session_save_run(export_path[CAP_OUT_RUN], m, sizeof m));
                cJSON_AddStringToObject(done, "runMessage", m);
            }
            if (current_caller == DBG_CALLER_MCP) {
                cJSON *where = cJSON_CreateObject();
                for (int i = 0; i < CAP_OUT_KINDS; i++) {
                    if (exporting[i])
                        cJSON_AddStringToObject(where, export_keys[i], export_path[i]);
                }
                cJSON_AddItemToObject(done, "paths", where);
            }
            cJSON_AddItemToObject(body, "exported", done);
        }
    }
    send_dap_response(seq, "x16/capture", true, body);
    return 0;
}

// ─── Host time (x16/hostPerf) ───────────────────────────────────────────────
// Where the HOST's time goes per emulated frame: emulation, input, present,
// the debugger UI, sleep, DAP -- plus each debugger panel. Guest cycle numbers
// (x16/perfStats, x16/perfHist) are unaffected by any of this; these are the
// numbers that say whether the machine keeps real speed. See host_prof.h.
// Arguments, optional:
//   reset: bool   zero the totals first (then sample again later)
static int handle_x16_host_perf(int seq, cJSON *args) {
    cJSON *item = args ? cJSON_GetObjectItemCaseSensitive(args, "reset") : NULL;
    if (cJSON_IsTrue(item))
        host_prof_reset();

    host_prof_summary_t s;
    host_prof_summary(&s);
    uint32_t last[HP_SECTIONS], last_wall = 0;
    host_prof_last_frame(last, &last_wall);

    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "frames", s.frames);
    cJSON_AddNumberToObject(body, "uiFrames", s.ui_frames);
    cJSON_AddNumberToObject(body, "wallSeconds", s.wall_s);
    cJSON_AddNumberToObject(body, "emulatedMHz", s.emu_mhz);
    cJSON_AddNumberToObject(body, "fps", s.fps);
    cJSON_AddNumberToObject(body, "uiHz", s.ui_hz);
    cJSON_AddNumberToObject(body, "busyP50Us", s.busy_p50_us);
    cJSON_AddNumberToObject(body, "busyP95Us", s.busy_p95_us);
    cJSON_AddNumberToObject(body, "busyMaxUs", s.busy_max_us);
    cJSON_AddNumberToObject(body, "lastFrameWallUs", last_wall);

    cJSON *sections = cJSON_CreateObject();
    for (int i = 0; i < HP_SECTIONS; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "msPerSecond", s.ms_per_s[i]);
        cJSON_AddNumberToObject(o, "usPerFrame", s.us_per_frame[i]);
        cJSON_AddNumberToObject(o, "lastFrameUs", last[i]);
        cJSON_AddItemToObject(sections, host_prof_section_name((hp_section_t)i), o);
    }
    cJSON_AddItemToObject(body, "sections", sections);

    cJSON *panels = cJSON_CreateArray();
    for (int i = 0; i < host_prof_panel_count(); i++) {
        host_prof_panel_t p;
        if (!host_prof_panel_get(i, &p)) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", p.name);
        cJSON_AddNumberToObject(o, "renders", p.calls);
        cJSON_AddNumberToObject(o, "avgUs", p.avg_us);
        cJSON_AddNumberToObject(o, "recentUs", p.recent_us);
        cJSON_AddNumberToObject(o, "maxUs", p.max_us);
        cJSON_AddNumberToObject(o, "msPerSecond", p.ms_per_s);
        cJSON_AddItemToArray(panels, o);
    }
    cJSON_AddItemToObject(body, "panels", panels);

    // Spikes: frames whose busy time exceeded their real-time budget, with
    // the section that dominated each, and the worst frames in full.
    cJSON *sp = cJSON_CreateObject();
    cJSON_AddNumberToObject(sp, "thresholdUs", host_prof_spike_us());
    cJSON_AddNumberToObject(sp, "count", host_prof_spikes());
    cJSON *by = cJSON_CreateObject();
    for (int i = 0; i < HP_SECTIONS; i++)
        cJSON_AddNumberToObject(by, host_prof_section_name((hp_section_t)i),
                                host_prof_spikes_by((hp_section_t)i));
    cJSON_AddItemToObject(sp, "dominatedBy", by);
    cJSON_AddItemToObject(body, "spikes", sp);

    host_prof_frame_t w[HP_WORST_FRAMES];
    const int nw = host_prof_worst(w, HP_WORST_FRAMES);
    cJSON *worst = cJSON_CreateArray();
    for (int k = 0; k < nw; k++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "frame", w[k].frame);
        cJSON_AddNumberToObject(o, "busyUs", w[k].busy_us);
        cJSON_AddNumberToObject(o, "wallUs", w[k].wall_us);
        cJSON_AddNumberToObject(o, "cycles", w[k].cycles);
        cJSON_AddNumberToObject(o, "uiFrames", w[k].ui_frames);
        cJSON_AddStringToObject(o, "dominant", host_prof_section_name(host_prof_dominant(&w[k])));
        cJSON *us = cJSON_CreateObject();
        for (int i = 0; i < HP_SECTIONS; i++)
            cJSON_AddNumberToObject(us, host_prof_section_name((hp_section_t)i), w[k].us[i]);
        cJSON_AddItemToObject(o, "sectionsUs", us);
        cJSON_AddItemToArray(worst, o);
    }
    cJSON_AddItemToObject(body, "worstFrames", worst);
    send_dap_response(seq, "x16/hostPerf", true, body);
    return 0;
}

static int handle_x16_perf_stats(int seq, cJSON *args) {
    // A client asking for statistics is a client that wants them collected.
    // Arming here rather than making it a separate round trip means the common
    // case -- attach, poll, read -- just works.
    if (!perf_budget_owner_wants(caller_perf_owner()) && !perf_budget_is_enabled()) {
        perf_budget_arm(caller_perf_owner(), true);
    }
    if (!vera_bandwidth_owner_wants(caller_bw_owner()) && !vera_bandwidth_is_enabled()) {
        vera_bandwidth_arm(caller_bw_owner(), true);
    }

    cJSON *body = cJSON_CreateObject();
    cJSON_AddItemToObject(body, "budget", perf_budget_json());
    cJSON_AddItemToObject(body, "bandwidth", vera_bandwidth_json());

    perf_frame_t last;
    if (perf_budget_last_frame(&last)) {
        cJSON_AddItemToObject(body, "current", perf_frame_json(&last));
    }

    // Windows: whatever the client asked for, else a useful default spread.
    static const float defaults[] = {1.0f, 10.0f, 30.0f, 60.0f, 0.0f};
    cJSON *req = args ? cJSON_GetObjectItemCaseSensitive(args, "windows") : NULL;
    cJSON *windows = cJSON_CreateArray();
    const int nreq = (req && cJSON_IsArray(req)) ? cJSON_GetArraySize(req) : 0;
    const int nwin = nreq > 0 ? nreq : (int)(sizeof defaults / sizeof defaults[0]);
    for (int i = 0; i < nwin; i++) {
        const float secs = nreq > 0 ? perf_window_seconds(cJSON_GetArrayItem(req, i))
                                    : defaults[i];
        perf_window_t w;
        if (!perf_budget_window(secs, &w)) continue;

        char name[32];
        perf_window_name(secs, name, sizeof name);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", name);
        cJSON_AddNumberToObject(o, "seconds", secs);
        cJSON_AddNumberToObject(o, "frames", w.frames);
        cJSON_AddNumberToObject(o, "budgetCycles", w.budget_cycles);
        cJSON_AddItemToObject(o, "work", perf_stat_json(&w.work));
        cJSON_AddItemToObject(o, "budgetWork", perf_stat_json(&w.budget_work));
        cJSON_AddItemToObject(o, "total", perf_stat_json(&w.total));
        cJSON_AddItemToObject(o, "irq", perf_stat_json(&w.irq));
        cJSON_AddItemToObject(o, "hostUs", perf_stat_json(&w.host_us));
        cJSON_AddNumberToObject(o, "overruns", w.overruns);
        cJSON_AddNumberToObject(o, "overrunPct", w.overrun_pct);
        cJSON_AddNumberToObject(o, "worstOverrunCycles", w.worst_overrun);
        cJSON_AddNumberToObject(o, "worstFrame", w.worst_frame);
        cJSON_AddNumberToObject(o, "meanUtilizationPct", w.mean_utilization);
        cJSON_AddNumberToObject(o, "p95UtilizationPct", w.p95_utilization);
        cJSON_AddNumberToObject(o, "maxUtilizationPct", w.max_utilization);
        cJSON_AddItemToArray(windows, o);
    }
    cJSON_AddItemToObject(body, "windows", windows);

    cJSON *incz = args ? cJSON_GetObjectItemCaseSensitive(args, "includeZones") : NULL;
    if (!incz || !cJSON_IsFalse(incz)) {
        cJSON *zones = cJSON_CreateArray();
        const int nz = perf_budget_zone_count();
        for (int i = 0; i < nz; i++) {
            perf_zone_t z;
            if (!perf_budget_zone_get(i, &z)) continue;
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "id", i);
            cJSON_AddStringToObject(o, "name", z.name);
            cJSON_AddBoolToObject(o, "isMarker", z.is_marker);
            cJSON_AddBoolToObject(o, "isIdle", z.is_idle);
            if (z.is_marker) {
                cJSON_AddNumberToObject(o, "markerId", z.marker_id);
            } else {
                cJSON_AddNumberToObject(o, "start", z.start);
                cJSON_AddNumberToObject(o, "end", z.end);
                cJSON_AddNumberToObject(o, "bank", z.bank);
            }
            if (i < PERF_RING_ZONES) {
                perf_frame_t lf;
                if (perf_budget_last_frame(&lf))
                    cJSON_AddNumberToObject(o, "current", lf.zone[i]);
            }
            perf_stat_t st;
            if (perf_budget_zone_window(i, 30.0f, &st))
                cJSON_AddItemToObject(o, "window30s", perf_stat_json(&st));
            cJSON_AddItemToArray(zones, o);
        }
        cJSON_AddItemToObject(body, "zones", zones);
    }

    cJSON *incf = args ? cJSON_GetObjectItemCaseSensitive(args, "includeFrames") : NULL;
    if (incf && cJSON_IsNumber(incf) && incf->valuedouble > 0) {
        int want = (int)incf->valuedouble;
        const int have = perf_budget_frame_count();
        if (want > have) want = have;
        cJSON *frames = cJSON_CreateArray();
        // Oldest first, so a client can append them to a series it is keeping.
        for (int i = want - 1; i >= 0; i--) {
            perf_frame_t f;
            if (perf_budget_frame_at(i, &f))
                cJSON_AddItemToArray(frames, perf_frame_json(&f));
        }
        cJSON_AddItemToObject(body, "frames", frames);
    }

    send_dap_response(seq, "x16/perfStats", true, body);
    return 0;
}

// Whether the client wants pushed overrun notifications, and when the last one
// went out. Coalescing is the point: a program that misses its budget usually
// misses it for many frames running, and one event per frame would be a flood
// that says nothing the first one did not.
static bool     perf_overrun_events = false;
static uint32_t perf_overrun_last_ms = 0;
#define PERF_OVERRUN_EVENT_INTERVAL_MS 1000

static int handle_x16_perf_config(int seq, cJSON *args) {
    if (!args) {
        send_dap_error_response(seq, "x16/perfConfig", "missing arguments");
        return 0;
    }
    static const char *const measured[] = {"targetFps", "idleMode", "historyFrames", "zones",
                                           "reset", NULL};
    if (refuse_during_capture(seq, "x16/perfConfig", args, measured)) return 0;

    cJSON *item = cJSON_GetObjectItemCaseSensitive(args, "enabled");
    if (cJSON_IsBool(item)) {
        const bool want = cJSON_IsTrue(item);
        if (!perf_budget_arm(caller_perf_owner(), want)) {
            send_dap_error_response(seq, "x16/perfConfig",
                                    "could not allocate the history ring");
            return 0;
        }
        // Turning profiling on is as clear a statement of intent as there is,
        // so overrun events follow it unless the client says otherwise below.
        // They go to the DAP client, so only that client's switch moves them.
        if (current_caller == DBG_CALLER_DAP) perf_overrun_events = want;
        // The bandwidth accounting follows the same switch. Two requests to
        // answer one question ("profile this machine") would be a surface
        // nobody wants to remember.
        vera_bandwidth_arm(caller_bw_owner(), want);
    }

    item = cJSON_GetObjectItemCaseSensitive(args, "targetFps");
    if (cJSON_IsNumber(item) && item->valuedouble > 0)
        perf_budget_set_target_fps((float)item->valuedouble);

    item = cJSON_GetObjectItemCaseSensitive(args, "idleMode");
    if (cJSON_IsString(item) && item->valuestring) {
        if (!strcmp(item->valuestring, "auto"))         perf_budget_set_idle_mode(PERF_IDLE_AUTO);
        else if (!strcmp(item->valuestring, "markers")) perf_budget_set_idle_mode(PERF_IDLE_MARKERS);
        else if (!strcmp(item->valuestring, "none"))    perf_budget_set_idle_mode(PERF_IDLE_NONE);
    }

    item = cJSON_GetObjectItemCaseSensitive(args, "historyFrames");
    if (cJSON_IsNumber(item) && item->valuedouble >= 2)
        perf_budget_set_capacity((int)item->valuedouble);

    item = cJSON_GetObjectItemCaseSensitive(args, "overrunEvents");
    if (cJSON_IsBool(item) && current_caller == DBG_CALLER_DAP)
        perf_overrun_events = cJSON_IsTrue(item);

    // Zones, when given, REPLACE the address zones wholesale: a client that
    // owns the zone list wants to declare it, not diff it. Marker regions the
    // guest opened are not the client's to remove, but zone_clear takes them
    // too -- they are re-created by the next marker write.
    item = cJSON_GetObjectItemCaseSensitive(args, "zones");
    if (cJSON_IsArray(item)) {
        perf_budget_zone_clear();
        const int n = cJSON_GetArraySize(item);
        for (int i = 0; i < n; i++) {
            cJSON *z = cJSON_GetArrayItem(item, i);
            if (!cJSON_IsObject(z)) continue;
            cJSON *jn = cJSON_GetObjectItemCaseSensitive(z, "name");
            cJSON *js = cJSON_GetObjectItemCaseSensitive(z, "start");
            cJSON *je = cJSON_GetObjectItemCaseSensitive(z, "end");
            cJSON *jb = cJSON_GetObjectItemCaseSensitive(z, "bank");
            cJSON *ji = cJSON_GetObjectItemCaseSensitive(z, "idle");
            if (!cJSON_IsNumber(js) || !cJSON_IsNumber(je)) continue;
            perf_budget_zone_add(cJSON_IsString(jn) ? jn->valuestring : "zone",
                                 (uint16_t)js->valuedouble, (uint16_t)je->valuedouble,
                                 cJSON_IsNumber(jb) ? (int16_t)jb->valuedouble : (int16_t)-1,
                                 cJSON_IsTrue(ji));
        }
    }

    item = cJSON_GetObjectItemCaseSensitive(args, "reset");
    if (cJSON_IsTrue(item)) {
        perf_budget_reset();
        vera_bandwidth_reset();
    }

    cJSON *body = cJSON_CreateObject();
    cJSON_AddItemToObject(body, "budget", perf_budget_json());
    cJSON_AddItemToObject(body, "bandwidth", vera_bandwidth_json());
    if (current_caller == DBG_CALLER_DAP)
        cJSON_AddBoolToObject(body, "overrunEvents", perf_overrun_events);
    send_dap_response(seq, "x16/perfConfig", true, body);
    return 0;
}

// Push accumulated budget overruns to the client. Called once per poll, which
// is once per emulated frame.
static void perf_poll_overruns(void) {
    if (!perf_overrun_events || client_sock == SOCKET_INVALID) return;
    if (!perf_budget_is_enabled()) return;

    const uint32_t now = SDL_GetTicks();
    if (now - perf_overrun_last_ms < PERF_OVERRUN_EVENT_INTERVAL_MS) return;

    perf_overrun_report_t r;
    if (!perf_budget_drain_overruns(&r)) {
        // Nothing to report. The clock is still advanced so that a run of clean
        // seconds does not make the first bad one fire instantly.
        perf_overrun_last_ms = now;
        return;
    }
    perf_overrun_last_ms = now;

    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "sinceLastEventFrames", r.frames);
    cJSON_AddNumberToObject(body, "overruns", r.overruns);
    cJSON_AddNumberToObject(body, "worstOverrunCycles", r.worst_overrun);
    cJSON_AddNumberToObject(body, "worstFrame", r.worst_frame);
    cJSON_AddNumberToObject(body, "worstUtilizationPct", r.worst_utilization);
    cJSON_AddNumberToObject(body, "budgetCycles", perf_budget_budget_cycles());
    send_dap_event("x16/perfBudgetOverrun", body);
}

// ─── x16/listBreakpoints: every armed breakpoint/watchpoint, any owner ──────
//
// setBreakpoints and its siblings only tell a client about breakpoints it
// itself set. This is the read-back for the whole picture -- what -bp armed,
// what the ImGui panel has, what a DAP request of its own already set -- in
// one poll. A request/response poll rather than a "breakpoint" event: see the
// KNOWN LIMITATION comment in debug_server.h (change events don't fit entries
// the client did not itself set; VS Code ignores/misuses them).

// Values are part of the debugger's external interface (debug_core.h); do not
// renumber BPOPERAND_*/BPCMP_* without updating these to match.
static const char *bp_operand_names[] = { "a", "x", "y", "sp", "p", "byte", "word" };
static const char *bp_cmp_names[]     = { "eq", "ne", "lt", "le", "gt", "ge" };

static const char *bp_operand_name(int operand) {
    return (operand >= 0 && operand < (int)(sizeof bp_operand_names / sizeof bp_operand_names[0]))
        ? bp_operand_names[operand] : "unknown";
}

static const char *bp_cmp_name(int op) {
    return (op >= 0 && op < (int)(sizeof bp_cmp_names / sizeof bp_cmp_names[0]))
        ? bp_cmp_names[op] : "unknown";
}

// Shared by breakpoints and watchpoints -- debug_owner_t bits mean the same
// thing in both tables. debug_core.h says the owner list "can be extended
// freely", so an unrecognised bit still gets an entry rather than being
// silently dropped.
static const char *owner_name(int owner, char *buf, size_t n) {
    static const char *names[] = {
        "cli", "ui", "dapSource", "dapFunction", "dapInstruction", "dapConsole", "step", "mcp"
    };
    if (owner >= 0 && owner < (int)(sizeof names / sizeof names[0])) return names[owner];
    snprintf(buf, n, "owner%d", owner);
    return buf;
}

static cJSON *owners_bitmask_to_json(uint16_t owners) {
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < DEBUG_OWNER_COUNT; i++) {
        if (!(owners & (1u << i))) continue;
        char buf[16];
        cJSON_AddItemToArray(arr, cJSON_CreateString(owner_name(i, buf, sizeof buf)));
    }
    return arr;
}

static int handle_x16_list_breakpoints(int seq, cJSON *args) {
    (void)args;
    cJSON *body = cJSON_CreateObject();

    cJSON *bps = cJSON_CreateArray();
    const int nbp = debug_bp_count();
    for (int i = 0; i < nbp; i++) {
        const struct breakpoint *bp = debug_bp_at(i);
        if (!bp) continue;

        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "pc", bp->pc);
        cJSON_AddNumberToObject(o, "bank", bp->bank);
        cJSON_AddNumberToObject(o, "x16Bank", bp->x16Bank);
        cJSON_AddBoolToObject(o, "enabled", bp->enabled);
        cJSON_AddItemToObject(o, "owners", owners_bitmask_to_json(bp->owners));

        int has_cond = 0, operand = 0, op = 0;
        uint16_t operand_addr = 0;
        uint32_t value = 0, ignore = 0;
        debug_bp_get_condition(bp->pc, bp->bank, bp->x16Bank, &has_cond, &operand,
                               &operand_addr, &op, &value, &ignore);
        if (has_cond) {
            cJSON *cond = cJSON_CreateObject();
            cJSON_AddStringToObject(cond, "operand", bp_operand_name(operand));
            cJSON_AddNumberToObject(cond, "operandAddr", operand_addr);
            cJSON_AddStringToObject(cond, "op", bp_cmp_name(op));
            cJSON_AddNumberToObject(cond, "value", value);
            cJSON_AddItemToObject(o, "condition", cond);
        } else {
            cJSON_AddNullToObject(o, "condition");
        }
        cJSON_AddNumberToObject(o, "ignoreCount", ignore);
        cJSON_AddNumberToObject(o, "hits", debug_bp_get_hits(bp->pc, bp->bank, bp->x16Bank));

        debug_owner_t dump_owner = DEBUG_OWNER_UI;
        uint32_t dump_writes = 0;
        bool dump_full = false;
        if (debug_bp_dump_info(bp->pc, bp->bank, bp->x16Bank, &dump_owner, &dump_writes, &dump_full)) {
            cJSON *d = cJSON_AddObjectToObject(o, "dump");
            cJSON_AddItemToObject(d, "owner", owners_bitmask_to_json((uint16_t)(1u << dump_owner)));
            cJSON_AddNumberToObject(d, "writes", dump_writes);
            cJSON_AddBoolToObject(d, "full", dump_full);
        }

        cJSON *logs = NULL;
        for (int k = 0; k < DEBUG_OWNER_COUNT; k++) {
            const char *message = logpoint_message(bp->pc, bp->bank, bp->x16Bank, (debug_owner_t)k);
            if (!message) continue;
            if (!logs) logs = cJSON_AddArrayToObject(o, "logs");
            char   name[16];
            cJSON *l = cJSON_CreateObject();
            cJSON_AddStringToObject(l, "owner", owner_name(k, name, sizeof name));
            cJSON_AddStringToObject(l, "message", message);
            cJSON_AddItemToArray(logs, l);
        }

        // DAP-visible identity, when this address was set through
        // setBreakpoints. Owner-gated first to disambiguate an address another
        // owner also happens to hold; first match wins, since a live entry
        // should never have more than one.
        if (debug_bp_has_owner(bp->pc, bp->bank, bp->x16Bank, DEBUG_OWNER_DAP_SOURCE)) {
            for (int j = 0; j < num_dap_bps; j++) {
                if (dap_bps[j].addr == (uint16_t)bp->pc && dap_bps[j].bank == bp->bank
                    && dap_bps[j].x16Bank == bp->x16Bank) {
                    cJSON_AddNumberToObject(o, "dapId", dap_bps[j].dap_id);
                    cJSON_AddStringToObject(o, "file", dap_bps[j].file);
                    cJSON_AddNumberToObject(o, "line", caller_line_out(dap_bps[j].line));
                    break;
                }
            }
        }

        cJSON_AddItemToArray(bps, o);
    }
    cJSON_AddItemToObject(body, "breakpoints", bps);

    cJSON *wps = cJSON_CreateArray();
    const int nwp = debug_wp_count();
    for (int i = 0; i < nwp; i++) {
        const struct watchpoint *wp = debug_wp_at(i);
        if (!wp) continue;

        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "space", wp->space == DEBUG_WP_SPACE_VRAM ? "vram" : "cpu");
        cJSON_AddNumberToObject(o, "addr", wp->addr);
        cJSON_AddNumberToObject(o, "len", wp->len);
        cJSON_AddNumberToObject(o, "x16Bank", wp->x16Bank);
        cJSON_AddBoolToObject(o, "active", wp->active);
        cJSON_AddNumberToObject(o, "hits", wp->hits);
        cJSON_AddNumberToObject(o, "ignoreCount", wp->ignore);
        cJSON_AddStringToObject(o, "access", dap_wp_access_name(wp->access));
        cJSON_AddBoolToObject(o, "excludeFetch", wp->exclude_fetch);
        cJSON_AddItemToObject(o, "owners", owners_bitmask_to_json(wp->owners));
        if (wp->has_value) {
            cJSON *filter = cJSON_CreateObject();
            cJSON_AddStringToObject(filter, "op", bp_cmp_name(wp->op));
            cJSON_AddNumberToObject(filter, "value", wp->value);
            cJSON_AddItemToObject(o, "valueFilter", filter);
        } else {
            cJSON_AddNullToObject(o, "valueFilter");
        }
        cJSON_AddItemToArray(wps, o);
    }
    cJSON_AddItemToObject(body, "watchpoints", wps);

    send_dap_response(seq, "x16/listBreakpoints", true, body);
    return 0;
}

// ─── x16/readLog: what logpoints have printed ───────────────────────────────
//
// Changes nothing: the position is the caller's, so any number of clients can
// read the same lines. Pass the returned `cursor` back to get what came after.
// `lost` counts the lines between the given cursor and the oldest still held.
//   cursor: where to read from (default: the oldest line held)
//   limit:  most lines to return, 1-256 (default 100)
static int handle_x16_read_log(int seq, cJSON *args) {
    static const char cmd[] = "x16/readLog";
    const dbg_log_ring_t *ring = logpoint_ring();
    const uint64_t oldest = dbg_log_ring_oldest(ring);
    uint64_t cursor = oldest;
    int limit = 100;

    cJSON *c = args ? cJSON_GetObjectItemCaseSensitive(args, "cursor") : NULL;
    if (c) {
        if (!cJSON_IsNumber(c) || c->valuedouble < 0 || c->valuedouble > 9007199254740992.0 ||
            c->valuedouble != (double)(uint64_t)c->valuedouble) {
            send_dap_error_response(seq, cmd, "'cursor' must be a whole number from an earlier result");
            return 0;
        }
        cursor = (uint64_t)c->valuedouble;
        if (cursor > ring->next) {
            send_dap_error_response(seq, cmd, "'cursor' is past the newest line");
            return 0;
        }
    }
    cJSON *l = args ? cJSON_GetObjectItemCaseSensitive(args, "limit") : NULL;
    if (l) {
        if (!cJSON_IsNumber(l) || l->valuedouble < 1 || l->valuedouble > 256 ||
            l->valuedouble != (int)l->valuedouble) {
            send_dap_error_response(seq, cmd, "'limit' must be a whole number from 1 to 256");
            return 0;
        }
        limit = (int)l->valuedouble;
    }

    const uint64_t lost = cursor < oldest ? oldest - cursor : 0;
    uint64_t at = cursor < oldest ? oldest : cursor;
    cJSON *body = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(body, "entries");
    for (int n = 0; n < limit && at < ring->next; n++, at++) {
        const dbg_log_entry_t *e = dbg_log_ring_at(ring, at);
        char where[24], name[16];
        dap_fmt_bp(where, sizeof where, (int)e->pc, e->bank, e->x16_bank);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "seq", (double)e->seq);
        cJSON_AddStringToObject(o, "address", where);
        cJSON_AddStringToObject(o, "owner", owner_name(e->owner, name, sizeof name));
        cJSON_AddStringToObject(o, "text", e->text);
        cJSON_AddItemToArray(list, o);
    }
    cJSON_AddNumberToObject(body, "cursor", (double)at);
    cJSON_AddNumberToObject(body, "lost", (double)lost);
    cJSON_AddBoolToObject(body, "more", at < ring->next);
    send_dap_response(seq, cmd, true, body);
    return 0;
}

// ─── x16/sdcard: card state, filesystem index and block history ─────────────
//
// What the I/O panel's SD Card tab and Files > SD image show, as JSON.
// Arguments (all optional):
//   files:   "touched" (default) | "all" | "none" -- which index entries to list
//   history: max history rows, newest first (default: all held)
//   reindex: true to re-read the filesystem first (per-file counts are kept)

static const char *sd_region_name(sdcard_fat_region_t r) {
    switch (r) {
        case SDCARD_FAT_REGION_MBR:      return "mbr";
        case SDCARD_FAT_REGION_RESERVED: return "reserved";
        case SDCARD_FAT_REGION_FAT:      return "fat";
        case SDCARD_FAT_REGION_ROOTDIR:  return "rootdir";
        case SDCARD_FAT_REGION_FILE:     return "file";
        case SDCARD_FAT_REGION_DIR:      return "dir";
        case SDCARD_FAT_REGION_FREE:     return "free";
        default:                         return "unknown";
    }
}

static int handle_x16_sdcard(int seq, cJSON *args) {
    const char *files_mode = "touched";
    int         hist_max   = SDCARD_HISTORY_SIZE;
    if (args) {
        cJSON *f = cJSON_GetObjectItemCaseSensitive(args, "files");
        if (cJSON_IsString(f)) {
            if (strcmp(f->valuestring, "touched") && strcmp(f->valuestring, "all") &&
                strcmp(f->valuestring, "none")) {
                send_dap_error_response(seq, "x16/sdcard",
                                        "'files' must be \"touched\", \"all\" or \"none\"");
                return 0;
            }
            files_mode = f->valuestring;
        }
        cJSON *h = cJSON_GetObjectItemCaseSensitive(args, "history");
        if (cJSON_IsNumber(h)) {
            hist_max = h->valueint < 0 ? 0 : h->valueint;
        }
        cJSON *r = cJSON_GetObjectItemCaseSensitive(args, "reindex");
        if (cJSON_IsTrue(r)) {
            sdcard_reindex();
        }
    }
    sdcard_fat_refresh();

    sdcard_debug_state_t sd;
    sdcard_debug_get_state(&sd);

    cJSON *body = cJSON_CreateObject();
    cJSON_AddBoolToObject(body, "attached", sd.attached);
    if (!sd.attached) {
        send_dap_response(seq, "x16/sdcard", true, body);
        return 0;
    }

    cJSON *card = cJSON_CreateObject();
    cJSON_AddStringToObject(card, "image", sd.image_path ? sd.image_path : "");
    cJSON_AddNumberToObject(card, "imageSize", (double)sd.image_size);
    cJSON_AddBoolToObject(card, "selected", sd.selected);
    cJSON_AddNumberToObject(card, "lastCommand", sd.last_cmd);
    cJSON_AddBoolToObject(card, "lastCommandIsAcmd", sd.last_cmd_is_acmd);
    cJSON_AddNumberToObject(card, "lastLba", sd.last_lba);
    cJSON_AddBoolToObject(card, "idle", sd.is_idle);
    cJSON_AddBoolToObject(card, "initialized", sd.is_initialized);
    cJSON_AddBoolToObject(card, "multiblockRead", sd.ongoing_multiblock_read);
    cJSON_AddNumberToObject(card, "responseLength", sd.response_length);
    cJSON_AddNumberToObject(card, "responseCounter", sd.response_counter);
    cJSON_AddNumberToObject(card, "commands", sd.commands);
    cJSON_AddNumberToObject(card, "blocksRead", (double)sd.blocks_read);
    cJSON_AddNumberToObject(card, "blocksWritten", (double)sd.blocks_written);
    cJSON_AddNumberToObject(card, "bytesRead", (double)sd.bytes_read);
    cJSON_AddNumberToObject(card, "bytesWritten", (double)sd.bytes_written);
    {
        const sdcard_fat_file_t *lf  = NULL;
        uint64_t                 off = 0;
        sdcard_fat_region_t      reg = sdcard_fat_lookup(sd.last_lba, &lf, &off);
        cJSON_AddStringToObject(card, "lastRegion", sd_region_name(reg));
        if (lf != NULL) {
            cJSON_AddStringToObject(card, "lastPath", lf->path);
            cJSON_AddNumberToObject(card, "lastOffset", (double)off);
        }
    }
    cJSON_AddItemToObject(body, "card", card);

    cJSON *index = cJSON_CreateObject();
    cJSON_AddBoolToObject(index, "ready", sdcard_fat_ready());
    if (sdcard_fat_ready()) {
        cJSON_AddStringToObject(index, "type", sdcard_fat_type_name());
        cJSON_AddNumberToObject(index, "bytesPerCluster", sdcard_fat_bytes_per_cluster());
        cJSON_AddNumberToObject(index, "entries", sdcard_fat_file_count());
        cJSON_AddBoolToObject(index, "stale", sdcard_fat_is_stale());
        cJSON_AddBoolToObject(index, "truncated", sdcard_fat_truncated());
        if (strcmp(files_mode, "none") != 0) {
            const bool touched_only = strcmp(files_mode, "touched") == 0;
            cJSON     *files        = cJSON_CreateArray();
            for (int i = 0; i < sdcard_fat_file_count(); i++) {
                const sdcard_fat_file_t *f = sdcard_fat_file_at(i);
                if (f == NULL || f->path == NULL) {
                    continue;
                }
                if (touched_only && f->bytes_read == 0 && f->bytes_written == 0) {
                    continue;
                }
                cJSON *o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "path", f->path);
                cJSON_AddBoolToObject(o, "dir", f->is_dir);
                cJSON_AddNumberToObject(o, "size", f->size);
                cJSON_AddNumberToObject(o, "bytesRead", (double)f->bytes_read);
                cJSON_AddNumberToObject(o, "bytesWritten", (double)f->bytes_written);
                cJSON_AddItemToArray(files, o);
            }
            cJSON_AddItemToObject(index, "files", files);
        }
    }
    cJSON_AddItemToObject(body, "index", index);

    cJSON *hist = cJSON_CreateArray();
    const int hn = sdcard_history_count();
    for (int i = 0; i < hn && i < hist_max; i++) {
        const sdcard_history_entry_t *h = sdcard_history_at(i);
        if (h == NULL) {
            break;
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "op", h->is_write ? "write" : "read");
        cJSON_AddNumberToObject(o, "command", h->command);
        cJSON_AddNumberToObject(o, "firstLba", h->first_lba);
        cJSON_AddNumberToObject(o, "blocks", h->blocks);
        cJSON_AddNumberToObject(o, "bytes", (double)h->blocks * 512.0);
        cJSON_AddStringToObject(o, "region", sd_region_name((sdcard_fat_region_t)h->region));
        if (h->path[0] != '\0') {
            cJSON_AddStringToObject(o, "path", h->path);
            cJSON_AddNumberToObject(o, "offset", (double)h->offset);
        } else if (h->region == SDCARD_FAT_REGION_FREE) {
            // Written before its file was indexed; name the block's owner now.
            const sdcard_fat_file_t *f = NULL;
            if (sdcard_fat_lookup(h->first_lba, &f, NULL) == SDCARD_FAT_REGION_FILE && f != NULL) {
                cJSON_AddStringToObject(o, "ownerNow", f->path);
            }
        }
        cJSON_AddItemToArray(hist, o);
    }
    cJSON_AddItemToObject(body, "history", hist);

    send_dap_response(seq, "x16/sdcard", true, body);
    return 0;
}

// ─── x16/spriteMuxStats, x16/spriteMuxConfig: per-frame sprite multiplexing ─
//
// Structural twin of x16/perfStats/x16/perfConfig above -- same self-arming
// poll, same explicit config request -- so a client already speaking one pair
// finds the other unsurprising.

static cJSON *sprite_frame_summary_json(const sprite_frame_summary_t *s) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "frame", s->frame);
    cJSON_AddNumberToObject(o, "cpuCycles", s->cpu_cycles);
    cJSON_AddNumberToObject(o, "genCount", s->gen_count);
    cJSON_AddNumberToObject(o, "effectiveSprites", s->effective_sprites);
    cJSON_AddNumberToObject(o, "phantomWrites", s->phantom_writes);
    cJSON_AddNumberToObject(o, "lateWrites", s->late_writes);
    cJSON_AddNumberToObject(o, "slotsUsed", s->slots_used);
    cJSON_AddNumberToObject(o, "slotsMultiplexed", s->slots_multiplexed);
    cJSON_AddNumberToObject(o, "maxGensInSlot", s->max_gens_in_slot);
    cJSON_AddNumberToObject(o, "mutations", s->mutations);
    cJSON_AddNumberToObject(o, "mutationsOnscreen", s->mutations_onscreen);
    cJSON_AddNumberToObject(o, "mutationBursts", s->mutation_bursts);
    cJSON_AddNumberToObject(o, "mutationCycles", s->mutation_cycles);
    cJSON_AddNumberToObject(o, "burstGap", s->burst_gap);
    cJSON_AddNumberToObject(o, "linesExhausted", s->lines_exhausted);
    cJSON_AddNumberToObject(o, "spritesDropped", s->sprites_dropped);
    cJSON_AddNumberToObject(o, "budgetUsedTotal", s->budget_used_total);
    cJSON_AddNumberToObject(o, "peakLine", s->peak_line);
    cJSON_AddNumberToObject(o, "peakBudget", s->peak_budget);
    cJSON_AddNumberToObject(o, "peakDemand", s->peak_demand);
    cJSON_AddBoolToObject(o, "multiplexing", s->multiplexing);
    return o;
}

static int handle_x16_sprite_mux_stats(int seq, cJSON *args) {
    (void)args;
    // Same rationale as handle_x16_perf_stats: asking for statistics is
    // asking for them to be collected, so attach-poll-read just works.
    if (!sprite_trace_owner_wants(caller_sprite_owner()) && !sprite_trace_active) {
        sprite_trace_arm(caller_sprite_owner(), true);
    }

    cJSON *body = cJSON_CreateObject();
    cJSON_AddBoolToObject(body, "enabled", sprite_trace_active);
    cJSON_AddNumberToObject(body, "burstGap", sprite_trace_get_burst_gap());
    cJSON_AddNumberToObject(body, "memoryBytes", (double)sprite_trace_memory_usage());

    // NULL until a frame completes after tracing was (re)enabled -- enabling
    // resets the recording -- and it never becomes non-NULL while the machine
    // is halted, since tracing only advances on a completed frame.
    const sprite_trace_frame_t *f = sprite_trace_last_frame();
    if (f) {
        cJSON_AddItemToObject(body, "frame", sprite_frame_summary_json(&f->summary));
    }

    send_dap_response(seq, "x16/spriteMuxStats", true, body);
    return 0;
}

static int handle_x16_sprite_mux_config(int seq, cJSON *args) {
    if (!args) {
        send_dap_error_response(seq, "x16/spriteMuxConfig", "missing arguments");
        return 0;
    }

    cJSON *item = cJSON_GetObjectItemCaseSensitive(args, "enabled");
    if (cJSON_IsBool(item)) {
        if (!sprite_trace_arm(caller_sprite_owner(), cJSON_IsTrue(item))) {
            send_dap_error_response(seq, "x16/spriteMuxConfig",
                                    "could not allocate the trace buffers");
            return 0;
        }
    }

    item = cJSON_GetObjectItemCaseSensitive(args, "burstGap");
    if (cJSON_IsNumber(item) && item->valuedouble > 0)
        sprite_trace_set_burst_gap((uint32_t)item->valuedouble);

    item = cJSON_GetObjectItemCaseSensitive(args, "reset");
    if (cJSON_IsTrue(item))
        sprite_trace_reset();

    cJSON *body = cJSON_CreateObject();
    cJSON_AddBoolToObject(body, "enabled", sprite_trace_active);
    cJSON_AddNumberToObject(body, "burstGap", sprite_trace_get_burst_gap());
    send_dap_response(seq, "x16/spriteMuxConfig", true, body);
    return 0;
}

// The most rows one disassemble request may ask for. Past it the request fails,
// because a shorter reply would break "exactly this number".
#define DAP_DISASM_MAX 65536

static void dap_add_invalid_instruction(cJSON *arr, uint8_t bank, uint16_t at) {
    char addr_str[16];
    if (bank != 0 || is_gen2)
        snprintf(addr_str, sizeof(addr_str), "0x%06X", ((uint32_t)bank << 16) | at);
    else
        snprintf(addr_str, sizeof(addr_str), "0x%04X", at);
    cJSON *inst = cJSON_CreateObject();
    cJSON_AddStringToObject(inst, "address", addr_str);
    cJSON_AddStringToObject(inst, "instruction", "??");
    cJSON_AddStringToObject(inst, "presentationHint", "invalid");
    cJSON_AddItemToArray(arr, inst);
}

static void dap_add_instruction(cJSON *arr, uint8_t bank, const code_map_line_t *ln) {
    cJSON *inst = cJSON_CreateObject();
    char addr_str[16];
    if (bank != 0 || is_gen2)
        snprintf(addr_str, sizeof(addr_str), "0x%06X", ((uint32_t)bank << 16) | ln->addr);
    else
        snprintf(addr_str, sizeof(addr_str), "0x%04X", ln->addr);
    cJSON_AddStringToObject(inst, "address", addr_str);
    cJSON_AddStringToObject(inst, "instruction", ln->text);

    const char *symbol = NULL;
    if (bank == 0 &&
        dbg_info_addr_to_label_in_bank(ln->addr, debug_current_x16_bank(ln->addr, 0), &symbol) &&
        symbol)
        cJSON_AddStringToObject(inst, "symbol", symbol);

    // Build instructionBytes hex string
    char bytes_str[32] = "";
    int blen = 0;
    for (int b = 0; b < ln->size && b < 4; b++) {
        blen += snprintf(bytes_str + blen, sizeof(bytes_str) - blen, "%02X ", ln->bytes[b]);
    }
    if (blen > 0) bytes_str[blen - 1] = '\0'; // trim trailing space
    cJSON_AddStringToObject(inst, "instructionBytes", bytes_str);

    // Add source location if dbg info is available
    const char *src_file = NULL;
    int src_line = 0;
    if (dbg_info_addr_to_source(ln->addr, &src_file, &src_line)) {
        char uri[1024];
        cJSON *source = cJSON_CreateObject();
        cJSON_AddStringToObject(source, "path", caller_path_out(src_file, uri, sizeof uri));
        cJSON_AddItemToObject(inst, "location", source);
        cJSON_AddNumberToObject(inst, "line", caller_line_out(src_line));
    }
    cJSON_AddItemToArray(arr, inst);
}

static int handle_dap_disassemble(int seq, cJSON *args) {
    if (!args) {
        send_dap_error_response(seq, "disassemble", "missing arguments");
        return 0;
    }
    cJSON *ref_item = cJSON_GetObjectItemCaseSensitive(args, "memoryReference");
    cJSON *count_item = cJSON_GetObjectItemCaseSensitive(args, "instructionCount");
    cJSON *offset_item = cJSON_GetObjectItemCaseSensitive(args, "offset");
    cJSON *insn_item = cJSON_GetObjectItemCaseSensitive(args, "instructionOffset");

    if (!ref_item || !cJSON_IsString(ref_item)) {
        send_dap_error_response(seq, "disassemble", "missing memoryReference");
        return 0;
    }
    uint32_t ref = 0;
    bool is_vram = false;
    if (!dap_parse_mem_ref(ref_item->valuestring, &is_vram, &ref) || is_vram) {
        send_dap_error_response(seq, "disassemble",
                                "unparseable memoryReference (expected a CPU address like 0xC000)");
        return 0;
    }
    const double asked = (count_item && cJSON_IsNumber(count_item)) ? count_item->valuedouble : 10;
    if (asked > DAP_DISASM_MAX) {
        send_dap_error_response(seq, "disassemble", "instructionCount is too large (limit 65536)");
        return 0;
    }
    const int count = asked < 0 ? 0 : (int)asked;
    int offset = (offset_item && cJSON_IsNumber(offset_item)) ? (int)offset_item->valuedouble : 0;
    double insn_off = (insn_item && cJSON_IsNumber(insn_item)) ? insn_item->valuedouble : 0;
    if (insn_off < -0x20000) insn_off = -0x20000;
    if (insn_off >  0x20000) insn_off =  0x20000;
    uint32_t ref24 = (uint32_t)(((int)ref + offset)) & 0xFFFFFF;
    uint16_t addr = (uint16_t)(ref24 & 0xFFFF);
    uint8_t  bank = (uint8_t)(ref24 >> 16); // 65C816 program bank

    // Decode via the anchored code map so operand widths follow the recorded
    // 65C816 M/X/E flag state (the naive path assumed a fixed width and drifted).
    uint8_t rambank = memory_get_ram_bank();
    uint8_t rombank = memory_get_rom_bank();
    static code_map_line_t lines[200];

    // Find the first row. An offset that runs off either end of the bank leaves
    // rows with nothing to show, which the protocol wants as invalid filler.
    int  pad_front = 0;
    bool past_end  = false;
    uint16_t cur   = addr;
    if (insn_off < 0) {
        for (int back = (int)-insn_off; back > 0; back--) {
            if (cur == 0) { pad_front = back; break; }
            const uint16_t prev = code_map_prev_instruction(cur, bank, rambank, rombank);
            if (prev >= cur) { pad_front = back; break; }
            cur = prev;
        }
    } else {
        for (int fwd = (int)insn_off; fwd > 0 && !past_end; ) {
            uint16_t next = cur;
            const int got = code_map_disasm_forward(cur, bank, rambank, rombank,
                                                    fwd > 200 ? 200 : fwd, lines, 200, &next);
            if (got <= 0 || next <= lines[got - 1].addr) past_end = true;
            fwd -= got > 0 ? got : fwd;
            cur = next;
        }
    }

    cJSON *instructions = cJSON_CreateArray();
    int emitted = 0;
    for (; emitted < count && emitted < pad_front; emitted++)
        dap_add_invalid_instruction(instructions, bank, 0);
    while (emitted < count && !past_end) {
        uint16_t next = cur;
        const int want = count - emitted > 200 ? 200 : count - emitted;
        const int got = code_map_disasm_forward(cur, bank, rambank, rombank, want, lines, 200, &next);
        if (got <= 0) break;
        for (int i = 0; i < got && emitted < count; i++, emitted++) {
            if (i > 0 && lines[i].addr <= lines[i - 1].addr) { past_end = true; break; }
            dap_add_instruction(instructions, bank, &lines[i]);
        }
        if (next <= lines[got - 1].addr) past_end = true;   // ran off the top
        cur = next;
    }
    for (; emitted < count; emitted++)
        dap_add_invalid_instruction(instructions, bank, 0xFFFF);

    cJSON *body = cJSON_CreateObject();
    cJSON_AddItemToObject(body, "instructions", instructions);
    send_dap_response(seq, "disassemble", true, body);
    return 0;
}

// ─── DAP command dispatch ───────────────────────────────────────────

// ─── setVariable ─────────────────────────────────────────────────────

// $hex, 0xhex or decimal, and nothing else: anything unreadable used to store 0
// and report success. Also takes a value as the variables view shows it, since
// that is the text an editor offers for editing: "$12 (18)" or "$35 [nv-BdIzC]".
// If the two halves of the first form disagree, there is no telling which was
// meant, so it is refused.
static bool dap_parse_value(const char *s, uint32_t *out) {
    while (isspace((unsigned char)*s)) s++;
    int base = 10;
    if (*s == '$') {
        base = 16;
        s++;
    } else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }
    if (base == 16 ? !isxdigit((unsigned char)*s) : !isdigit((unsigned char)*s)) return false;
    if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return false;

    char *end;
    const unsigned long long v = strtoull(s, &end, base);
    if (v > 0xFFFFFFFFULL) return false;      // an overflow reads as the maximum
    while (isspace((unsigned char)*end)) end++;
    if (*end == '(') {
        if (!isdigit((unsigned char)end[1])) return false;
        char *close;
        const unsigned long long shown = strtoull(end + 1, &close, 10);
        if (*close != ')' || shown != v) return false;
        end = close + 1;
    } else if (*end == '[') {
        end = strchr(end, ']');
        if (!end) return false;
        end++;
    }
    while (isspace((unsigned char)*end)) end++;
    if (*end) return false;
    *out = (uint32_t)v;
    return true;
}

// Stores to a register by its name in the variables view. Reports what it holds
// afterwards, since a value wider than the register is cut down to fit.
static bool dap_set_register(const char *name, uint32_t v, uint32_t *stored, int *digits) {
    const bool a16 = regs.is65c816 && !regs.e && !(regs.status & FLAG_MEMORY_WIDTH);
    const bool x16 = regs.is65c816 && !regs.e && !(regs.status & FLAG_INDEX_WIDTH);
    *digits = 2;
    if      (!strcmp(name, "PC")) { regs.pc = (uint16_t)v; *stored = regs.pc; *digits = 4; }
    else if (!strcmp(name, "A"))  {
        if (a16) { regs.c = (uint16_t)v; *stored = regs.c; *digits = 4; }
        else     { regs.a = (uint8_t)v;  *stored = regs.a; }
    }
    else if (!strcmp(name, "X"))  {
        if (x16) { regs.x = (uint16_t)v; *stored = regs.x; *digits = 4; }
        else     { regs.xl = (uint8_t)v; *stored = regs.xl; }
    }
    else if (!strcmp(name, "Y"))  {
        if (x16) { regs.y = (uint16_t)v; *stored = regs.y; *digits = 4; }
        else     { regs.yl = (uint8_t)v; *stored = regs.yl; }
    }
    else if (!strcmp(name, "SP")) {
        regs.sp = regs.is65c816 ? (uint16_t)v : (uint16_t)((regs.sp & 0xFF00) | (v & 0xFF));
        if (regs.is65c816 && !regs.e) { *stored = regs.sp; *digits = 4; }
        else                            *stored = (uint8_t)regs.sp;
    }
    else if (!strcmp(name, "D"))   { regs.dp = (uint16_t)v; *stored = regs.dp; *digits = 4; }
    else if (!strcmp(name, "DBR")) { regs.db = (uint8_t)v; *stored = regs.db; }
    else if (!strcmp(name, "PBR")) { regs.k = (uint8_t)v; *stored = regs.k; }
    else if (name[0] == 'P')       { regs.status = (uint8_t)v; *stored = regs.status; } // "P (Status)"
    else if (!strcmp(name, "RAM Bank")) { memory_set_ram_bank((uint8_t)v); *stored = memory_get_ram_bank(); }
    else if (!strcmp(name, "ROM Bank")) { memory_set_rom_bank((uint8_t)v); *stored = memory_get_rom_bank(); }
    else return false;
    return true;
}

// Stores virtual register Rn and returns what it now holds.
static uint32_t dap_set_vreg(int n, uint32_t v) {
    const uint16_t a = dbg_machine_dp_add((uint16_t)(0x02 + 2 * n));
    debug_write6502(a, 0, (uint8_t)(v & 0xFF));
    debug_write6502((uint16_t)(a + 1), 0, (uint8_t)((v >> 8) & 0xFF));
    return (uint32_t)debug_read6502(a, 0, USE_CURRENT_X16_BANK) |
           ((uint32_t)debug_read6502((uint16_t)(a + 1), 0, USE_CURRENT_X16_BANK) << 8);
}

static int handle_dap_set_variable(int seq, cJSON *args) {
    if (!args) { send_dap_error_response(seq, "setVariable", "missing arguments"); return 0; }
    cJSON *ref_i  = cJSON_GetObjectItemCaseSensitive(args, "variablesReference");
    cJSON *name_i = cJSON_GetObjectItemCaseSensitive(args, "name");
    cJSON *val_i  = cJSON_GetObjectItemCaseSensitive(args, "value");
    int ref = (ref_i && cJSON_IsNumber(ref_i)) ? (int)ref_i->valuedouble : 0;
    const char *name   = (name_i && cJSON_IsString(name_i)) ? name_i->valuestring : "";
    const char *valstr = (val_i && cJSON_IsString(val_i)) ? val_i->valuestring : "";
    uint32_t v;
    if (!dap_parse_value(valstr, &v)) {
        send_dap_error_response(seq, "setVariable", "not a number (use $hex, 0xhex or decimal)");
        return 0;
    }
    bool ok = true;
    // What the target actually holds afterwards, which is what gets reported: a
    // value wider than the register is cut down to fit.
    uint32_t stored = 0;
    int digits = 2;

    if (ref == VARREF_REGISTERS) {
        ok = dap_set_register(name, v, &stored, &digits);
    } else if (ref == VARREF_STACK) {
        const uint16_t a = (uint16_t)dap_parse_num(name); // name is "$XXXX"
        debug_write6502(a, 0, (uint8_t)v);
        stored = debug_read6502(a, 0, USE_CURRENT_X16_BANK);
    } else if (ref == VARREF_VREGS) {
        // name is "Rn" (n = 0..15) → 16-bit little-endian at direct-page + $02 + 2*n.
        if ((name[0] == 'R' || name[0] == 'r') && name[1]) {
            int n = atoi(name + 1);
            if (n >= 0 && n <= 15) {
                stored = dap_set_vreg(n, v);
                digits = 4;
            } else ok = false;
        } else ok = false;
    } else {
        ok = false; // zero-page rows are 16-byte groups — not individually settable
    }

    if (!ok) { send_dap_error_response(seq, "setVariable", "cannot set this variable"); return 0; }
    char result[40];
    snprintf(result, sizeof result, "$%0*X (%u)", digits, stored, stored);
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "value", result);
    cJSON_AddNumberToObject(body, "variablesReference", 0);
    send_dap_response(seq, "setVariable", true, body);
    return 0;
}

// ─── setExpression ───────────────────────────────────────────────────

// The next address of a word, as a word read takes it: within the 16-bit bank.
static uint32_t dap_expr_next(uint32_t addr) {
    return (addr & 0xFF0000) | ((addr + 1) & 0xFFFF);
}

// A CPU write has no way to name a bank, so a target in one that is not mapped
// would land in whichever is.
static bool dap_expr_bank_mapped(uint32_t addr, int bank, char *err, size_t errn) {
    if (bank < 0 || addr > 0xFFFF) return true;
    const int now = debug_current_x16_bank((int)addr, 0);
    if (now == DEBUG_BANK_ANY || now == bank) return true;
    snprintf(err, errn, "bank %d is not mapped at $%04X (bank %d is); select it first",
             bank, (unsigned)addr, now);
    return false;
}

static void dap_expr_poke(uint32_t addr, uint8_t value) {
    debug_write6502((uint16_t)(addr & 0xFFFF), (uint8_t)(addr >> 16), value);
}

static uint8_t dap_expr_peek(uint32_t addr) {
    return debug_read6502((uint16_t)(addr & 0xFFFF), (uint8_t)(addr >> 16), USE_CURRENT_X16_BANK);
}

static bool dap_expr_store(const dbg_expr_target_t *t, uint32_t v, uint32_t *stored, int *digits,
                           char *err, size_t errn) {
    *digits = 2;
    switch (t->kind) {
        case DBG_EXPR_TARGET_REG:
            if (t->reg >= DBG_EXPR_REG_R0 && t->reg <= DBG_EXPR_REG_R15) {
                *stored = dap_set_vreg((int)(t->reg - DBG_EXPR_REG_R0), v);
                *digits = 4;
                return true;
            }
            {
                const char *name = t->reg == DBG_EXPR_REG_P       ? "P (Status)"
                                 : t->reg == DBG_EXPR_REG_RAMBANK ? "RAM Bank"
                                 : t->reg == DBG_EXPR_REG_ROMBANK ? "ROM Bank"
                                 : dbg_expr_reg_name(t->reg);
                if (dap_set_register(name, v, stored, digits)) return true;
            }
            snprintf(err, errn, "cannot set this register");
            return false;
        case DBG_EXPR_TARGET_BYTE:
            if (!dap_expr_bank_mapped(t->addr, t->bank, err, errn)) return false;
            dap_expr_poke(t->addr, (uint8_t)v);
            *stored = dap_expr_peek(t->addr);
            return true;
        case DBG_EXPR_TARGET_WORD: {
            const uint32_t next = dap_expr_next(t->addr);
            if (!dap_expr_bank_mapped(t->addr, t->bank, err, errn) ||
                !dap_expr_bank_mapped(next, t->bank, err, errn))
                return false;
            dap_expr_poke(t->addr, (uint8_t)(v & 0xFF));
            dap_expr_poke(next, (uint8_t)((v >> 8) & 0xFF));
            *stored = (uint32_t)dap_expr_peek(t->addr) | ((uint32_t)dap_expr_peek(next) << 8);
            *digits = 4;
            return true;
        }
        case DBG_EXPR_TARGET_VBYTE:
            video_space_write(t->addr, (uint8_t)v);
            *stored = video_space_read(t->addr);
            return true;
    }
    snprintf(err, errn, "cannot set this");
    return false;
}

static int handle_dap_set_expression(int seq, cJSON *args) {
    static const char cmd[] = "setExpression";
    cJSON *ex = args ? cJSON_GetObjectItemCaseSensitive(args, "expression") : NULL;
    cJSON *vi = args ? cJSON_GetObjectItemCaseSensitive(args, "value") : NULL;
    if (!ex || !cJSON_IsString(ex) || !vi || !cJSON_IsString(vi)) {
        send_dap_error_response(seq, cmd, "'expression' and 'value' are required");
        return 0;
    }

    char err[220];
    dbg_expr_t        target_expr, value_expr;
    dbg_expr_target_t target;
    if (!dbg_expr_compile(ex->valuestring, dbg_machine_env(), &target_expr, err, sizeof err) ||
        !dbg_expr_target(&target_expr, dbg_machine_env(), &target, err, sizeof err)) {
        send_dap_error_response(seq, cmd, err);
        return 0;
    }

    // An expression, or a value as this server displays it, which is the text an
    // editor offers for editing: "$12 (18)" or "$35 [nv-BdIzC]".
    uint32_t v = 0;
    dbg_expr_value_t value;
    if (dbg_expr_compile(vi->valuestring, dbg_machine_env(), &value_expr, err, sizeof err) &&
        dbg_expr_eval(&value_expr, dbg_machine_env(), &value, err, sizeof err)) {
        v = value.value;
    } else if (!strpbrk(vi->valuestring, "([") || !dap_parse_value(vi->valuestring, &v)) {
        send_dap_error_response(seq, cmd, err);
        return 0;
    }

    uint32_t stored = 0;
    int digits = 2;
    if (!dap_expr_store(&target, v, &stored, &digits, err, sizeof err)) {
        send_dap_error_response(seq, cmd, err);
        return 0;
    }
    char result[40];
    snprintf(result, sizeof result, "$%0*X (%u)", digits, stored, stored);
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "value", result);
    cJSON_AddNumberToObject(body, "variablesReference", 0);
    send_dap_response(seq, cmd, true, body);
    return 0;
}

// ─── completions ─────────────────────────────────────────────────────

#define DAP_COMPLETIONS_MAX 100

static bool dap_is_name_char(char c) {
    return isalnum((unsigned char)c) || c == '_' || c == '.' || c == '@';
}

// UTF-16 code units in one UTF-8 sequence, given its first byte.
static int dap_utf16_units(unsigned char lead) {
    return lead >= 0xF0 ? 2 : 1;
}

static const char *dap_utf8_next(const char *s) {
    s++;
    while (((unsigned char)*s & 0xC0) == 0x80) s++;
    return s;
}

static int dap_utf16_len(const char *s, const char *end) {
    int units = 0;
    while (s < end) {
        units += dap_utf16_units((unsigned char)*s);
        s = dap_utf8_next(s);
    }
    return units;
}

static void dap_add_completion(cJSON *targets, const char *label, const char *type,
                               int start, int length) {
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "label", label);
    cJSON_AddNumberToObject(item, "start", start);
    cJSON_AddNumberToObject(item, "length", length);
    cJSON_AddStringToObject(item, "type", type);
    cJSON_AddItemToArray(targets, item);
}

static bool dap_completion_listed(cJSON *targets, const char *label) {
    cJSON *t;
    cJSON_ArrayForEach(t, targets) {
        if (!SDL_strcasecmp(cJSON_GetObjectItemCaseSensitive(t, "label")->valuestring, label))
            return true;
    }
    return false;
}

static int handle_dap_completions(int seq, cJSON *args) {
    static const char cmd[] = "completions";
    cJSON *ti = args ? cJSON_GetObjectItemCaseSensitive(args, "text") : NULL;
    cJSON *ci = args ? cJSON_GetObjectItemCaseSensitive(args, "column") : NULL;
    cJSON *li = args ? cJSON_GetObjectItemCaseSensitive(args, "line") : NULL;
    if (!ti || !cJSON_IsString(ti) || !ci || !cJSON_IsNumber(ci)) {
        send_dap_error_response(seq, cmd, "'text' and 'column' are required");
        return 0;
    }
    const char *text = ti->valuestring;
    cJSON *targets = cJSON_CreateArray();

    // The line asked for, or none if the text is shorter than that.
    const char *line = text;
    if (li && cJSON_IsNumber(li)) {
        const double index = li->valuedouble - dap_line_base;
        if (index < 0) line = NULL;
        for (double n = index; line && n > 0; n--) {
            const char *nl = strchr(line, '\n');
            line = nl ? nl + 1 : NULL;
        }
    }

    if (line) {
        const char *end = line + strcspn(line, "\r\n");
        const char *cursor = line;
        for (double left = ci->valuedouble - dap_col_base; left > 0 && cursor < end; ) {
            left -= dap_utf16_units((unsigned char)*cursor);
            cursor = dap_utf8_next(cursor);
        }
        const char *word = cursor;
        while (word > line && dap_is_name_char(word[-1])) word--;
        const size_t n = (size_t)(cursor - word);
        const bool number = isdigit((unsigned char)*word) && n > 0;
        const bool hex    = word > line && word[-1] == '$';

        if (!number && !hex && n <= DBG_EXPR_MAX_NAME) {
            char prefix[DBG_EXPR_MAX_NAME + 1];
            for (size_t i = 0; i < n; i++) prefix[i] = (char)tolower((unsigned char)word[i]);
            prefix[n] = '\0';
            const int start  = dap_col_base + dap_utf16_len(text, word);
            const int length = (int)n;

            int listed = 0;
            for (int r = 0; r < DBG_EXPR_REG_COUNT; r++) {
                const char *name = dbg_expr_reg_name((dbg_expr_reg_t)r);
                if (dap_ci_prefix(name, prefix)) {
                    dap_add_completion(targets, name, "property", start, length);
                    listed++;
                }
            }
            const int symbols = dbg_info_symbol_count();
            for (int i = 0; i < symbols && listed < DAP_COMPLETIONS_MAX; i++) {
                const char *name = NULL;
                dbg_addr_t  addr;
                if (!dbg_info_symbol_at(i, &name, &addr) || !name ||
                    !dap_ci_prefix(name, prefix) || dap_completion_listed(targets, name))
                    continue;
                dap_add_completion(targets, name, "reference", start, length);
                listed++;
            }
        }
    }

    cJSON *body = cJSON_CreateObject();
    cJSON_AddItemToObject(body, "targets", targets);
    send_dap_response(seq, cmd, true, body);
    return 0;
}

// The MCP evaluate tool. Only the engine: the console commands in evaluate can
// clear every client's breakpoints or reset the machine.
// Replaces this session's filters and nothing else: an editor sends this on every
// session, usually empty, and that must not touch the debugger window's setting.
static int handle_dap_set_exception_breakpoints(int seq, cJSON *args) {
    cJSON *list = args ? cJSON_GetObjectItemCaseSensitive(args, "filters") : NULL;
    if (list && !cJSON_IsArray(list)) {
        send_dap_error_response(seq, "setExceptionBreakpoints", "'filters' must be an array");
        return 0;
    }
    uint16_t mask = 0;
    cJSON *results = cJSON_CreateArray();
    cJSON *item;
    cJSON_ArrayForEach(item, list) {
        const uint16_t bit = cJSON_IsString(item) ? stop_filter_bit(item->valuestring) : 0;
        cJSON *r = cJSON_CreateObject();
        const bool ok = bit && !dap_no_debug;
        cJSON_AddBoolToObject(r, "verified", ok);
        if (!bit)               cJSON_AddStringToObject(r, "message", "unknown filter");
        else if (dap_no_debug)  cJSON_AddStringToObject(r, "message", "launched with noDebug");
        if (ok) mask |= bit;
        cJSON_AddItemToArray(results, r);
    }
    debug_stop_filter_set(DEBUG_OWNER_DAP_SOURCE, mask);
    cJSON *body = cJSON_CreateObject();
    cJSON_AddItemToObject(body, "breakpoints", results);
    send_dap_response(seq, "setExceptionBreakpoints", true, body);
    return 0;
}

static int handle_dap_exception_info(int seq, cJSON *args) {
    (void)args;
    debug_stop_exception_t ex;
    if (!DEBUGGetStopException(&ex)) {
        send_dap_error_response(seq, "exceptionInfo", "the machine is not stopped on an exception");
        return 0;
    }
    const int i = stop_filter_index(ex.kind);
    char text[64];
    exception_text(&ex, text, sizeof text);
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "exceptionId", stop_filter_table[i].filter);
    cJSON_AddStringToObject(body, "description", text);
    cJSON_AddStringToObject(body, "breakMode", "always");
    cJSON *details = cJSON_AddObjectToObject(body, "details");
    cJSON_AddStringToObject(details, "message", text);
    cJSON_AddStringToObject(details, "typeName", stop_filter_table[i].label);
    if (ex.has_vector) cJSON_AddNumberToObject(body, "vector", ex.vector);
    else               cJSON_AddNullToObject(body, "vector");
    cJSON_AddNumberToObject(body, "address", ex.address);
    cJSON_AddNumberToObject(body, "programBank", ex.bank);
    send_dap_response(seq, "exceptionInfo", true, body);
    return 0;
}

// With no 'filters', reports. A bad name changes nothing.
static int handle_mcp_set_stop_on(int seq, cJSON *args) {
    static const char *const cmd = "mcp/setStopOn";
    cJSON *list = cJSON_GetObjectItemCaseSensitive(args, "filters");
    if (list) {
        if (!cJSON_IsArray(list)) {
            send_dap_error_response(seq, cmd, "'filters' must be an array of irq, nmi, brk, stp");
            return 0;
        }
        uint16_t mask = 0;
        cJSON *item;
        cJSON_ArrayForEach(item, list) {
            const uint16_t bit = cJSON_IsString(item) ? stop_filter_bit(item->valuestring) : 0;
            if (!bit) {
                send_dap_error_response(seq, cmd, "unknown filter: use irq, nmi, brk or stp");
                return 0;
            }
            mask |= bit;
        }
        debug_stop_filter_set(DEBUG_OWNER_MCP, mask);
    }
    cJSON *body = cJSON_CreateObject();
    cJSON_AddItemToObject(body, "filters", stop_filter_names(debug_stop_filter_get(DEBUG_OWNER_MCP)));
    cJSON_AddItemToObject(body, "armed", stop_filter_names(debug_stop_filters()));
    send_dap_response(seq, cmd, true, body);
    return 0;
}

static int handle_mcp_evaluate(int seq, cJSON *args) {
    static const char cmd[] = "mcp/evaluate";
    cJSON *ex = cJSON_GetObjectItemCaseSensitive(args, "expression");
    if (!ex || !cJSON_IsString(ex)) {
        send_dap_error_response(seq, cmd, "'expression' is required");
        return 0;
    }
    char err[220], text[40];
    dbg_expr_t       expr;
    dbg_expr_value_t value;
    if (!dbg_expr_compile(ex->valuestring, dbg_machine_env(), &expr, err, sizeof err) ||
        !dbg_expr_eval(&expr, dbg_machine_env(), &value, err, sizeof err)) {
        send_dap_error_response(seq, cmd, err);
        return 0;
    }
    dap_fmt_expr_value(text, sizeof text, &value);
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "value", value.value);
    cJSON_AddStringToObject(body, "text", text);
    if (value.bank >= 0) cJSON_AddNumberToObject(body, "bank", value.bank);
    send_dap_response(seq, cmd, true, body);
    return 0;
}

// ─── terminate (Stop: ends a launched machine, detaches from any other) ──
static int handle_dap_terminate(int seq, cJSON *args) {
    (void)args;
    // A machine another front end is still using outlives this session.
    const bool launched = dap_launched && !frontend_holds_run_state();
    send_dap_response(seq, "terminate", true, NULL);
    if (launched) {
        // The shutdown path sends exited and terminated.
        dap_quit_emulator();
        return 0;
    }
    // Sending can drop the client, and dropping it has already done the rest.
    if (dap_reply_lost()) return 1;

    dap_session_active = false;
    // Released before resuming, for the same reason as the disconnect request:
    // running on with this client's breakpoints still armed halts the machine
    // again moments later, and by then the session is marked inactive so the
    // socket cleanup will not resume it.
    dap_release_session_state();
    // Same rule as a client disconnecting: only take the machine out of a stop
    // when there is no local debugger whose user owns the run state. Otherwise
    // "Stop Debugging" in the editor runs the emulator out from under someone
    // who is halted in the SDL debug window inspecting it.
    if (currentMode == DMODE_STOP && !dap_other_owner_of_run_state()) {
        currentMode = DMODE_RUN;
    }
    send_dap_event("terminated", NULL);
    return 1;
}

// ─── attach (the emulator is already running; mirror launch) ─────────
static int handle_dap_attach(int seq, cJSON *args) {
    if (args) {
        cJSON *dbg_file = cJSON_GetObjectItemCaseSensitive(args, "dbgFile");
        if (dbg_file && cJSON_IsString(dbg_file)) dbg_info_load(dbg_file->valuestring);
    }
    dap_launched = false;
    send_dap_response(seq, "attach", true, NULL);
    send_dap_event_thread_started();
    return 0;
}

// ─── setFunctionBreakpoints (label breakpoints via .dbg) ─────────────
static int handle_dap_set_function_breakpoints(int seq, cJSON *args) {
    // "Here is the complete list" -- so let go of everything this kind of
    // breakpoint asked for, and start again. Anything another owner also wants
    // stays armed for them; nothing here has to know who they are. The keys are
    // remembered so conditions nobody asks for again can be dropped below,
    // rather than left for a later breakpoint at that address to inherit.
    struct { int pc; uint8_t bank; int x16Bank; } old[128];
    const int n_old = num_func_bps;
    for (int i = 0; i < n_old; i++) {
        old[i].pc = func_bp_addrs[i];
        old[i].bank = func_bp_banks[i];
        old[i].x16Bank = func_bp_x16[i];
    }
    debug_bp_clear_owner(DEBUG_OWNER_DAP_FUNCTION);
    num_func_bps = 0;

    cJSON *body = cJSON_CreateObject();
    cJSON *arr  = cJSON_CreateArray();
    cJSON *bps  = args ? cJSON_GetObjectItemCaseSensitive(args, "breakpoints") : NULL;
    int    n    = (bps && cJSON_IsArray(bps)) ? cJSON_GetArraySize(bps) : 0;
    for (int i = 0; i < n; i++) {
        cJSON *fb = cJSON_GetArrayItem(bps, i);
        cJSON *nm = cJSON_GetObjectItemCaseSensitive(fb, "name");
        cJSON *ci = cJSON_GetObjectItemCaseSensitive(fb, "condition");
        cJSON *hi = cJSON_GetObjectItemCaseSensitive(fb, "hitCondition");
        const char *cond_str = (ci && cJSON_IsString(ci)) ? ci->valuestring : "";
        const char *hit_str  = (hi && cJSON_IsString(hi)) ? hi->valuestring : "";
        cJSON *rb = cJSON_CreateObject();
        dbg_addr_t addr = 0;
        bool verified = false;
        int seg_bank = -1;
        char why[200] = "out of memory; breakpoint not armed";
        if (!nm || !cJSON_IsString(nm)) {
            snprintf(why, sizeof why, "breakpoint has no name");
        } else if (strlen(nm->valuestring) >= sizeof func_bp_names[0]) {
            snprintf(why, sizeof why, "name is too long");
        } else if (num_func_bps >= 128) {
            snprintf(why, sizeof why, "too many function breakpoints (limit 128)");
        } else if (!dbg_info_label_to_addr_bank(nm->valuestring, &addr, &seg_bank)) {
            snprintf(why, sizeof why, "no label named \"%.60s\" in the loaded debug info", nm->valuestring);
        } else if (dap_bp_can_arm(addr, seg_bank, cond_str, hit_str, why, sizeof why)) {
            // Pinned to the bank the label's segment was loaded into, unless the
            // condition names one. Armed for any bank, a label at $A000 also
            // stopped in every other bank's code at $A000.
            const int pc  = (int)(addr & 0xFFFF);
            const uint8_t pb = (uint8_t)(addr >> 16);
            const bool existed = debug_bp_find(pc, pb, dap_cond_bank_pin(addr, cond_str, seg_bank)) >= 0;
            const int x16 = dap_apply_bp_condition(addr, cond_str, hit_str, seg_bank);
            struct breakpoint bp = { pc, pb, x16, 0, false };
            if (debug_bp_add_for(bp, DEBUG_OWNER_DAP_FUNCTION) != DEBUG_ADD_FULL) {
                const int id = next_dap_bp_id++;
                func_bp_banks[num_func_bps] = pb;
                func_bp_addrs[num_func_bps] = (uint16_t)pc;
                func_bp_x16[num_func_bps]   = x16;
                func_bp_ids[num_func_bps]   = id;
                snprintf(func_bp_names[num_func_bps], sizeof func_bp_names[0], "%s", nm->valuestring);
                snprintf(func_bp_cond[num_func_bps], sizeof func_bp_cond[0], "%s", cond_str);
                snprintf(func_bp_hit[num_func_bps], sizeof func_bp_hit[0], "%s", hit_str);
                num_func_bps++;
                char ref[16];
                if (pb != 0 || is_gen2)
                    snprintf(ref, sizeof ref, "0x%06X", ((uint32_t)pb << 16) | (uint32_t)pc);
                else
                    snprintf(ref, sizeof ref, "0x%04X", (unsigned)pc);
                cJSON_AddNumberToObject(rb, "id", id);
                cJSON_AddStringToObject(rb, "instructionReference", ref);
                dap_add_bank_message(rb, x16);
                verified = true;
            } else if (!existed) {
                debug_bp_forget(pc, pb, x16);
                snprintf(why, sizeof why, "out of memory; breakpoint not armed");
            }
        }
        cJSON_AddBoolToObject(rb, "verified", verified);
        if (!verified) cJSON_AddStringToObject(rb, "message", why);
        cJSON_AddItemToArray(arr, rb);
    }
    for (int i = 0; i < n_old; i++) {
        if (debug_bp_find(old[i].pc, old[i].bank, old[i].x16Bank) < 0)
            debug_bp_forget(old[i].pc, old[i].bank, old[i].x16Bank);
    }
    cJSON_AddItemToObject(body, "breakpoints", arr);
    send_dap_response(seq, "setFunctionBreakpoints", true, body);
    return 0;
}

// ─── setInstructionBreakpoints ───────────────────────────────────────

// One hex number, with or without a "$" or "0x" prefix. Bare digits are hex,
// as -bp reads them. Returns the character after it, or NULL if there was none.
static const char *dap_scan_hex(const char *s, unsigned long *out) {
    while (*s == ' ') s++;
    if (s[0] == '$')
        s++;
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    if (!isxdigit((unsigned char)*s))
        return NULL;
    char *end;
    *out = strtoul(s, &end, 16);
    return end;
}

// Resolve an instructionReference into a core breakpoint, or fail.
//
// The X16 bank was the dimension this path did not have. A reference was read as
// `pc | program_bank << 16` with x16Bank left at ANY, but read6502 forces the
// program bank to zero off a Gen2, so any non-zero top byte built an entry whose
// bank could never equal the running one: it was rejected on every instruction
// while the client had been told "verified": true. Breakpoints in banked RAM --
// exactly the ones worth setting from an editor -- silently did nothing.
//
// Which field a bank byte belongs in is debug_bp_from_ref()'s to decide, so that
// this and -bp cannot drift apart on it. Only the syntax is here: "bb:aaaa" is
// accepted and means what -bp means by it.
//
// Anything that is not entirely one of those forms is refused. Bare digits used
// to go through strtoul's base-0 rule, so "A000" armed $0000 and "0401" (octal)
// armed $0101, both reported verified.
static bool dap_parse_instruction_ref(const char *s, long offset, struct breakpoint *out) {
    if (!s) return false;
    if (dap_has_prefix_i(s, "sym:")) {
        dbg_resolved_t r;
        char why[160];
        if (!dbg_info_resolve(s + 4, &r, why, sizeof why)) return false;
        return debug_bp_from_ref((long)r.addr, r.bank >= 0 ? r.bank : DEBUG_BANK_ANY, offset, out);
    }
    unsigned long first, a;
    const char *p = dap_scan_hex(s, &first);
    if (!p) return false;
    if (*p == ':') {
        p = dap_scan_hex(p + 1, &a);
        if (!p) return false;
        while (*p == ' ') p++;
        if (*p || first > 0xFF || a > 0xFFFF) return false;
        return debug_bp_from_ref((long)a, (int)first, offset, out);
    }
    while (*p == ' ') p++;
    if (*p || first > 0xFFFFFF) return false;
    return debug_bp_from_ref((long)first, DEBUG_BANK_ANY, offset, out);
}

// A breakpoint address in the form dap_parse_instruction_ref() reads back.
static void dap_fmt_bp(char *out, size_t n, int pc, uint8_t bank, int x16Bank) {
    if (x16Bank != DEBUG_BANK_ANY)
        snprintf(out, n, "%02X:%04X", (unsigned)x16Bank, (unsigned)pc);
    else if (bank)
        snprintf(out, n, "$%02X%04X", (unsigned)bank, (unsigned)pc);
    else
        snprintf(out, n, "$%04X", (unsigned)pc);
}

// Check a breakpoint condition is one this server can apply: "&&"-joined
// terms, each a bank pin ("bank == 70", "bank == any") or a single value test
// ("A == $05", "byte[$1234] != 0"). *bank_out gets the bank term: -1 for none,
// else as dap_parse_bank_term() returns it. On failure, says why in err.
static bool dap_check_condition(const char *cond, int *bank_out, char *err, size_t errn) {
    *bank_out = -1;
    if (!cond || !cond[0]) return true;
    char buf[192];
    if (strlen(cond) >= sizeof buf) {
        snprintf(err, errn, "condition is too long");
        return false;
    }
    strcpy(buf, cond);
    int values = 0;
    for (char *t = buf; t; ) {
        char *amp = strstr(t, "&&");
        if (amp) *amp = '\0';
        while (*t == ' ') t++;
        const int b = dap_parse_bank_term(t);
        if (b == DAP_BANK_TERM_BAD) {
            snprintf(err, errn, "cannot read the bank in \"%s\"", t);
            return false;
        }
        if (b != -1) {
            if (*bank_out != -1 && *bank_out != b) {
                snprintf(err, errn, "condition names two different banks");
                return false;
            }
            *bank_out = b;
        } else {
            int operand, op; uint16_t oaddr; uint32_t val;
            if (!dap_parse_value_term(t, &operand, &oaddr, &op, &val)) {
                snprintf(err, errn,
                         "cannot parse condition term \"%s\" (expected e.g. A == $05, "
                         "byte[$1234] != 0, bank == 70)", t);
                return false;
            }
            if (++values > 1) {
                snprintf(err, errn, "only one value test per condition is supported");
                return false;
            }
        }
        t = amp ? amp + 2 : NULL;
    }
    return true;
}

// A hitCondition this server can apply: N, or >= N, meaning stop from the Nth
// hit on.
static bool dap_check_hit_condition(const char *h) {
    uint32_t n;
    return strlen(h) < sizeof func_bp_hit[0] && dap_parse_hit(h, &n) && n > 0;
}

// x16Dump on an instruction breakpoint makes it a DUMP breakpoint: it does not
// stop; each firing appends memory to a file (debug_core.c).
//   "x16Dump": {"file": path, "regions": [{"addr": n, "len": n, "bank": n?}]}
// addr is a 24-bit CPU address, or a memoryReference string as readMemory takes
// it ("0xC000", "vram:1B000"); bank pins an x16 RAM bank for $A000-$BFFF and
// means nothing for VRAM.  A VRAM region wraps at the top of the 17-bit space.
static uint8_t dap_dump_read(uint32_t addr, int x16Bank) {
    if (x16Bank == DEBUG_DUMP_VRAM)
        return video_space_read(addr & 0x1FFFF);
    return debug_read6502((uint16_t)(addr & 0xFFFF), (uint8_t)(addr >> 16), x16Bank);
}

#define DAP_DUMP_REGIONS 16

static bool dap_dump_regions(cJSON *regs, uint32_t *addr, uint32_t *len, int *bank, int *count) {
    int    n = 0;
    cJSON *r;
    cJSON_ArrayForEach(r, regs) {
        cJSON *a = cJSON_GetObjectItemCaseSensitive(r, "addr");
        cJSON *l = cJSON_GetObjectItemCaseSensitive(r, "len");
        cJSON *b = cJSON_GetObjectItemCaseSensitive(r, "bank");
        if (n >= DAP_DUMP_REGIONS || !a || !l || !cJSON_IsNumber(l))
            return false;
        bool is_vram = false;
        if (cJSON_IsNumber(a))
            addr[n] = (uint32_t)a->valuedouble;
        else if (!cJSON_IsString(a) || !dap_parse_mem_ref(a->valuestring, &is_vram, &addr[n]))
            return false;
        if (is_vram && b)
            return false;   // a RAM bank pin on VRAM is a mistake, not a no-op
        len[n]  = (uint32_t)l->valuedouble;
        bank[n] = is_vram ? DEBUG_DUMP_VRAM
                : (b && cJSON_IsNumber(b)) ? (int)b->valuedouble : USE_CURRENT_X16_BANK;
        n++;
    }
    *count = n;
    return true;
}

static bool dap_apply_dump(const struct breakpoint *bp, cJSON *dump) {
    cJSON *file = cJSON_GetObjectItemCaseSensitive(dump, "file");
    cJSON *regs = cJSON_GetObjectItemCaseSensitive(dump, "regions");
    if (!file || !cJSON_IsString(file) || !regs || !cJSON_IsArray(regs))
        return false;
    uint32_t addr[DAP_DUMP_REGIONS], len[DAP_DUMP_REGIONS];
    int      bank[DAP_DUMP_REGIONS], n = 0;
    if (!dap_dump_regions(regs, addr, len, bank, &n))
        return false;
    debug_bp_dump_read = dap_dump_read;
    return debug_bp_set_dump(bp->pc, bp->bank, bp->x16Bank, DEBUG_OWNER_DAP_INSTRUCTION,
                             file->valuestring, n, addr, len, bank);
}

static int handle_dap_set_instruction_breakpoints(int seq, cJSON *args) {
    // The request REPLACES the set, and clearing the owner closes its dumps.
    // Remember the keys, so the conditions of the ones not asked for again can
    // be dropped below rather than left for a later breakpoint there.
    struct { int pc; uint8_t bank; int x16Bank; } old[128];
    int n_old = 0;
    for (int i = 0; i < debug_bp_count(); i++) {
        const struct breakpoint *b = debug_bp_at(i);
        if ((b->owners & (1u << DEBUG_OWNER_DAP_INSTRUCTION)) && n_old < 128) {
            old[n_old].pc = b->pc;
            old[n_old].bank = b->bank;
            old[n_old].x16Bank = b->x16Bank;
            n_old++;
        }
    }
    debug_bp_clear_owner(DEBUG_OWNER_DAP_INSTRUCTION);
    num_instr_bps = 0;

    cJSON *body = cJSON_CreateObject();
    cJSON *arr  = cJSON_CreateArray();
    cJSON *bps  = args ? cJSON_GetObjectItemCaseSensitive(args, "breakpoints") : NULL;
    int    n    = (bps && cJSON_IsArray(bps)) ? cJSON_GetArraySize(bps) : 0;
    for (int i = 0; i < n; i++) {
        cJSON *ib  = cJSON_GetArrayItem(bps, i);
        cJSON *ir  = cJSON_GetObjectItemCaseSensitive(ib, "instructionReference");
        cJSON *off = cJSON_GetObjectItemCaseSensitive(ib, "offset");
        cJSON *rb  = cJSON_CreateObject();
        long   offset = (off && cJSON_IsNumber(off)) ? (long)off->valuedouble : 0;
        cJSON *ci  = cJSON_GetObjectItemCaseSensitive(ib, "condition");
        cJSON *hi  = cJSON_GetObjectItemCaseSensitive(ib, "hitCondition");
        cJSON *xb  = cJSON_GetObjectItemCaseSensitive(ib, "x16Bank");
        const char *cond_str = (ci && cJSON_IsString(ci)) ? ci->valuestring : "";
        const char *hit_str  = (hi && cJSON_IsString(hi)) ? hi->valuestring : "";
        struct breakpoint bp;
        bool verified = false;
        char why[200] = "address could not be resolved to an armable breakpoint";
        int  cond_bank = -1;

        // The condition and hit count used to be ignored outright, so a
        // conditional breakpoint stopped on every pass. Anything that cannot be
        // applied is now refused rather than armed without it.
        bool ok = ir && cJSON_IsString(ir)
                  && dap_parse_instruction_ref(ir->valuestring, offset, &bp)
                  && num_instr_bps < 128;
        if (ok && !dap_check_condition(cond_str, &cond_bank, why, sizeof why))
            ok = false;
        if (ok && hit_str[0] && !dap_check_hit_condition(hit_str)) {
            snprintf(why, sizeof why, "cannot parse hitCondition \"%s\" (expected N: stop from the Nth hit on)",
                     hit_str);
            ok = false;
        }

        // The bank can come from the reference ("46:BDA8"), an x16Bank field (the
        // name x16/listBreakpoints reports it under), or a bank term in the
        // condition. Any of them will do; two that disagree are refused.
        if (ok) {
            int want = bp.x16Bank;
            if (xb) {
                if (!cJSON_IsNumber(xb) || xb->valuedouble < 0 || xb->valuedouble > 255) {
                    snprintf(why, sizeof why, "x16Bank must be a bank number 0-255");
                    ok = false;
                } else if (want != DEBUG_BANK_ANY && want != (int)xb->valuedouble) {
                    snprintf(why, sizeof why, "x16Bank disagrees with the bank in instructionReference");
                    ok = false;
                } else {
                    want = (int)xb->valuedouble;
                }
            }
            if (ok && cond_bank != -1) {
                const int cb = (cond_bank == DAP_BANK_TERM_ANY) ? DEBUG_BANK_ANY : cond_bank;
                if (want != DEBUG_BANK_ANY && cb != want) {
                    snprintf(why, sizeof why, "the condition's bank disagrees with the breakpoint's bank");
                    ok = false;
                } else {
                    want = cb;
                }
            }
            if (ok && want != DEBUG_BANK_ANY
                && debug_normalise_bank(want, bp.pc, bp.bank) == DEBUG_BANK_ANY) {
                snprintf(why, sizeof why, "$%04X is not in a banked window, so a bank cannot apply",
                         (unsigned)bp.pc);
                ok = false;
            }
            bp.x16Bank = want;
        }

        if (ok) {
            const dbg_addr_t a24 = ((dbg_addr_t)bp.bank << 16) | (dbg_addr_t)bp.pc;
            if (dap_bp_cond_conflicts(a24, dap_cond_bank_pin(a24, cond_str, bp.x16Bank),
                                      cond_str, hit_str)) {
                snprintf(why, sizeof why,
                         "another breakpoint at $%04X has a different condition or hit count",
                         (unsigned)bp.pc);
                ok = false;
            }
        }

        if (ok) {
            const dbg_addr_t a24 = ((dbg_addr_t)bp.bank << 16) | (dbg_addr_t)bp.pc;
            dap_apply_bp_condition(a24, cond_str, hit_str, bp.x16Bank);
            if (debug_bp_add_for(bp, DEBUG_OWNER_DAP_INSTRUCTION) != DEBUG_ADD_FULL) {
                const int id  = next_dap_bp_id++;
                const int idx = num_instr_bps++;
                instr_bp_banks[idx] = bp.bank;
                instr_bp_addrs[idx] = (uint16_t)bp.pc;
                instr_bp_x16[idx]   = debug_normalise_bank(bp.x16Bank, bp.pc, bp.bank);
                instr_bp_ids[idx]   = id;
                cJSON_AddNumberToObject(rb, "id", id);
                verified = true;
                cJSON *dump = cJSON_GetObjectItemCaseSensitive(ib, "x16Dump");
                if (dump) {
                    instr_bp_ids[idx] = 0;   // writes to a file and never stops
                    debug_owner_t holder = DEBUG_OWNER_DAP_INSTRUCTION;
                    debug_bp_dump_info(bp.pc, bp.bank, bp.x16Bank, &holder, NULL, NULL);
                    const bool taken = holder != DEBUG_OWNER_DAP_INSTRUCTION;
                    if (!taken && dap_apply_dump(&bp, dump))
                        cJSON_AddBoolToObject(rb, "x16Dump", true);
                    else {
                        debug_bp_remove_for(bp.pc, bp.bank, bp.x16Bank, DEBUG_OWNER_DAP_INSTRUCTION);
                        if (debug_bp_find(bp.pc, bp.bank, bp.x16Bank) < 0)
                            debug_bp_forget(bp.pc, bp.bank, bp.x16Bank);
                        num_instr_bps--;
                        verified = false;
                        if (taken)
                            snprintf(why, sizeof why, "another client has a dump breakpoint at this address");
                        else
                            snprintf(why, sizeof why,
                                     "x16Dump needs {file, regions: [{addr, len, bank?}]} (addr a number "
                                     "or a reference like \"vram:1B000\", no bank on VRAM), at most 16 "
                                     "regions, and a file that can be opened");
                    }
                } else {
                    debug_bp_clear_dump_for(bp.pc, bp.bank, bp.x16Bank, DEBUG_OWNER_DAP_INSTRUCTION);
                }
            } else {
                snprintf(why, sizeof why, "out of memory; breakpoint not armed");
            }
        }
        cJSON_AddBoolToObject(rb, "verified", verified);
        // Verified used to be the only thing said about a breakpoint, which made
        // "armed" and "accepted but inert" look identical from the client side.
        if (!verified)
            cJSON_AddStringToObject(rb, "message", why);
        else
            dap_add_bank_message(rb, bp.x16Bank);
        cJSON_AddItemToArray(arr, rb);
    }
    for (int i = 0; i < n_old; i++) {
        if (debug_bp_find(old[i].pc, old[i].bank, old[i].x16Bank) < 0)
            debug_bp_forget(old[i].pc, old[i].bank, old[i].x16Bank);
    }
    cJSON_AddItemToObject(body, "breakpoints", arr);
    send_dap_response(seq, "setInstructionBreakpoints", true, body);
    return 0;
}

// ─── dataBreakpointInfo (memory write watchpoint on an address) ──────

// "R0" to "R15", either case.
static bool dap_vreg_number(const char *s, int *n) {
    if (s[0] != 'R' && s[0] != 'r') return false;
    if (!isdigit((unsigned char)s[1])) return false;
    int v = s[1] - '0';
    if (s[2]) {
        if (s[1] == '0' || !isdigit((unsigned char)s[2]) || s[3]) return false;
        v = v * 10 + (s[2] - '0');
    }
    if (v > 15) return false;
    *n = v;
    return true;
}

// What a variable or expression names in memory. A CPU register has no address,
// and a zero-page row is sixteen unrelated bytes, so neither can be watched: the
// old code read every such name as the number 0 and watched $0000.
static bool dap_data_target(const char *name, int ref, uint16_t *addr, uint16_t *len) {
    uint32_t a;
    int vreg;
    switch (ref) {
    case VARREF_VREGS:
        if (!dap_vreg_number(name, &vreg)) return false;
        *addr = dbg_machine_dp_add((uint16_t)(0x02 + 2 * vreg));
        *len  = 2;
        return *addr < 0xFFFF;   // a watch is one contiguous range
    case VARREF_STACK:
        if (!dap_strict_num(name, name + strlen(name), &a) || a < 0x0100 || a > 0x01FF) return false;
        *addr = (uint16_t)a;
        *len  = 1;
        return true;
    case 0:   // an expression rather than a row of a scope
        if (dap_vreg_number(name, &vreg)) return dap_data_target(name, VARREF_VREGS, addr, len);
        if (!dap_strict_num(name, name + strlen(name), &a) || a > 0xFFFF) return false;
        *addr = (uint16_t)a;
        *len  = 1;
        return true;
    default:
        return false;
    }
}

// A VRAM target: "vram:<hex address>", with ":<decimal length>" for a range.
static bool dap_parse_vram_target(const char *s, uint32_t *addr, uint32_t *len) {
    if (!dap_has_prefix_i(s, "vram:")) return false;
    const char *colon = strchr(s + 5, ':');
    const size_t n = colon ? (size_t)(colon - s) : strlen(s);
    char head[24];
    if (n >= sizeof head) return false;
    memcpy(head, s, n);
    head[n] = '\0';

    bool is_vram = false;
    uint32_t a = 0, l = 1;
    if (!dap_parse_mem_ref(head, &is_vram, &a) || !is_vram || a > 0x1FFFF) return false;
    if (colon && (!dap_strict_num(colon + 1, colon + strlen(colon), &l) || l == 0 || l > 0x20000 - a))
        return false;
    *addr = a;
    *len  = l;
    return true;
}

static int handle_dap_data_breakpoint_info(int seq, cJSON *args) {
    cJSON *name_i = args ? cJSON_GetObjectItemCaseSensitive(args, "name") : NULL;
    cJSON *ref_i  = args ? cJSON_GetObjectItemCaseSensitive(args, "variablesReference") : NULL;
    const int ref = (ref_i && cJSON_IsNumber(ref_i)) ? (int)ref_i->valuedouble : 0;
    cJSON *as_i = args ? cJSON_GetObjectItemCaseSensitive(args, "asAddress") : NULL;
    cJSON *by_i = args ? cJSON_GetObjectItemCaseSensitive(args, "bytes") : NULL;
    const bool as_address = as_i && cJSON_IsTrue(as_i);
    const bool bytes_ok = !by_i || (cJSON_IsNumber(by_i) && by_i->valuedouble >= 1 &&
                                    by_i->valuedouble <= 0x20000 &&
                                    by_i->valuedouble == (uint32_t)by_i->valuedouble);
    const uint32_t bytes = (by_i && bytes_ok) ? (uint32_t)by_i->valuedouble : 0;
    cJSON *body = cJSON_CreateObject();
    char dataId[32], desc[64];
    bool known = false, vram = false;
    if (name_i && cJSON_IsString(name_i) && bytes_ok) {
        const char *name = name_i->valuestring;
        uint32_t vaddr = 0, vlen = 1, addr = 0, len = 1;
        uint16_t addr16 = 0, len16 = 1;
        if (dap_parse_vram_target(name, &vaddr, &vlen)) {
            if (bytes) vlen = bytes;
            known = vlen <= 0x20000 - vaddr;
            vram  = true;
            if (vlen > 1) {
                snprintf(dataId, sizeof dataId, "vram:%05X:%u", (unsigned)vaddr, (unsigned)vlen);
                snprintf(desc, sizeof desc, "VRAM $%05X-$%05X", (unsigned)vaddr,
                         (unsigned)(vaddr + vlen - 1));
            } else {
                snprintf(dataId, sizeof dataId, "vram:%05X", (unsigned)vaddr);
                snprintf(desc, sizeof desc, "VRAM $%05X", (unsigned)vaddr);
            }
        } else if (as_address ? (dap_strict_num(name, name + strlen(name), &addr) && addr <= 0xFFFF)
                              : dap_data_target(name, ref, &addr16, &len16)) {
            if (!as_address) {
                addr = addr16;
                len  = len16;
            }
            if (bytes) len = bytes;
            known = len <= 0x10000 - addr;
            // A CPU watch applies whichever bank is mapped.
            const char *banks = addr + len > 0xA000 ? " in any bank" : "";
            if (len > 1) {
                snprintf(dataId, sizeof dataId, "%u:%u", (unsigned)addr, (unsigned)len);
                snprintf(desc, sizeof desc, "$%04X-$%04X%s", (unsigned)addr,
                         (unsigned)(addr + len - 1), banks);
            } else {
                snprintf(dataId, sizeof dataId, "%u", (unsigned)addr);
                snprintf(desc, sizeof desc, "$%04X%s", (unsigned)addr, banks);
            }
        }
    }
    if (known) {
        cJSON_AddStringToObject(body, "dataId", dataId);
        cJSON_AddStringToObject(body, "description", desc);
        cJSON *at = cJSON_CreateArray();
        cJSON_AddItemToArray(at, cJSON_CreateString("write"));
        if (!vram) {
            cJSON_AddItemToArray(at, cJSON_CreateString("read"));
            cJSON_AddItemToArray(at, cJSON_CreateString("readWrite"));
        }
        cJSON_AddItemToObject(body, "accessTypes", at);
        cJSON_AddBoolToObject(body, "canPersist", false);
    } else {
        cJSON_AddNullToObject(body, "dataId");
        cJSON_AddStringToObject(body, "description", "no data breakpoint is available for this");
    }
    send_dap_response(seq, "dataBreakpointInfo", true, body);
    return 0;
}

// A dataId as dataBreakpointInfo issues it: "addr", "addr:len", or either of
// those after "vram:".
static bool dap_parse_data_id(const char *s, uint8_t *space, uint32_t *addr, uint32_t *len) {
    if (dap_has_prefix_i(s, "vram:")) {
        *space = DEBUG_WP_SPACE_VRAM;
        return dap_parse_vram_target(s, addr, len);
    }
    const char *colon = strchr(s, ':');
    uint32_t a, l = 1;
    if (!dap_strict_num(s, colon ? colon : s + strlen(s), &a) || a > 0xFFFF) return false;
    if (colon && (!dap_strict_num(colon + 1, colon + strlen(colon), &l) || l == 0 || l > 0x10000 - a))
        return false;
    *space = DEBUG_WP_SPACE_CPU;
    *addr  = a;
    *len   = l;
    return true;
}

// Claim a watch, or say why not. A shared entry keeps its first owner's length
// and filter, so one that differs from this request would be reported armed
// while doing something else, and changing it would alter that owner's watch.
static const char *dap_arm_watch(uint8_t space, uint8_t access, bool exclude_fetch, uint32_t addr,
                                 uint32_t len, bool has_value, int op, uint8_t value,
                                 uint32_t ignore, debug_owner_t owner) {
    const debug_wp_spec_t spec = { space, addr, len, DEBUG_BANK_ANY, has_value, op, value,
                                   access, exclude_fetch, ignore };
    switch (debug_wp_add_ex(&spec, owner)) {
    case DEBUG_ADD_FULL:
        return "the watchpoint table is full";
    case DEBUG_ADD_CONFLICT:
        return "another watch at this address has a different range, filter, hit count or state";
    default:
        return NULL;
    }
}

// ─── setDataBreakpoints (maps to emulator write watchpoints) ─────────
static int handle_dap_set_data_breakpoints(int seq, cJSON *args) {
    // The client's complete list, so let go of the previous one. Watches the
    // console made are owned separately and are left alone, as are any from -wp.
    // An entry sent again unchanged keeps its count, as a code breakpoint does.
    struct watchpoint previous[MAX_WATCHPOINTS];
    int num_previous = 0;
    for (int i = 0; i < debug_wp_count(); i++) {
        const struct watchpoint *w = debug_wp_at(i);
        if (w->owners & (1u << DEBUG_OWNER_DAP_SOURCE)) previous[num_previous++] = *w;
    }
    debug_wp_clear_owner(DEBUG_OWNER_DAP_SOURCE);
    num_data_bps = 0;

    cJSON *body = cJSON_CreateObject();
    cJSON *arr  = cJSON_CreateArray();
    cJSON *bps  = args ? cJSON_GetObjectItemCaseSensitive(args, "breakpoints") : NULL;
    int    n    = (bps && cJSON_IsArray(bps)) ? cJSON_GetArraySize(bps) : 0;
    for (int i = 0; i < n; i++) {
        cJSON *db = cJSON_GetArrayItem(bps, i);
        cJSON *id = cJSON_GetObjectItemCaseSensitive(db, "dataId");
        cJSON *ac = cJSON_GetObjectItemCaseSensitive(db, "accessType");
        cJSON *hc = cJSON_GetObjectItemCaseSensitive(db, "hitCondition");
        cJSON *c  = cJSON_GetObjectItemCaseSensitive(db, "condition");
        cJSON *rb = cJSON_CreateObject();
        const char *why = NULL;
        uint8_t  space = DEBUG_WP_SPACE_CPU;
        uint8_t  access = DEBUG_WP_ACCESS_WRITE;
        uint32_t addr = 0, len = 1;
        bool has_value = false;
        int op = 0;
        uint32_t val = 0;

        if (ac && cJSON_IsString(ac)) {
            if (!strcmp(ac->valuestring, "read")) access = DEBUG_WP_ACCESS_READ;
            else if (!strcmp(ac->valuestring, "readWrite")) access = DEBUG_WP_ACCESS_BOTH;
            else if (strcmp(ac->valuestring, "write")) access = 0;
        } else if (ac) {
            access = 0;
        }

        if (!id || !cJSON_IsString(id) || !dap_parse_data_id(id->valuestring, &space, &addr, &len))
            why = "not a dataId from dataBreakpointInfo";
        else if (!access)
            why = "accessType must be read, write or readWrite";
        else if (space == DEBUG_WP_SPACE_VRAM && access != DEBUG_WP_ACCESS_WRITE)
            why = "a VRAM watch catches writes only";
        else if (hc && !cJSON_IsString(hc))
            why = "hitCondition must be a string";
        else if (hc && hc->valuestring[0] && !dap_check_hit_condition(hc->valuestring))
            why = "cannot parse hitCondition (expected N: stop from the Nth hit on)";
        else if (c && !cJSON_IsString(c))
            why = "condition must be a string";
        else if (c && c->valuestring[0]) {
            // A written-value filter (e.g. "== $05", "!=0", "$FF"): only break
            // when the byte written compares true.
            has_value = true;
            if (len > 1)
                why = "a value condition applies to a single byte";
            else if (!dap_parse_simple_cmp(c->valuestring, &op, &val) || val > 0xFF)
                why = "cannot parse condition (expected a byte comparison, e.g. == $05)";
        }
        if (!why && num_data_bps >= MAX_WATCHPOINTS)
            why = "too many data breakpoints";
        uint32_t hit_at = 1;
        if (!why && hc && hc->valuestring[0]) dap_parse_hit(hc->valuestring, &hit_at);
        if (!why)
            // A data breakpoint is on data, so bytes fetched as code do not count.
            why = dap_arm_watch(space, access, true, addr, len, has_value, op, (uint8_t)val,
                                hit_at - 1, DEBUG_OWNER_DAP_SOURCE);

        if (!why) {
            const int at = debug_wp_find(space, addr, DEBUG_BANK_ANY);
            const struct watchpoint *w = debug_wp_at(at);
            for (int k = 0; w && k < num_previous; k++) {
                const struct watchpoint *p = &previous[k];
                if (p->space == w->space && p->addr == w->addr && p->len == w->len &&
                    p->access == w->access && p->has_value == w->has_value &&
                    (!p->has_value || (p->op == w->op && p->value == w->value)) &&
                    p->hits > w->hits) {
                    debug_wp_set_hits_at(at, p->hits);
                    break;
                }
            }
            const int bp_id = next_dap_bp_id++;
            data_bps[num_data_bps].id    = bp_id;
            data_bps[num_data_bps].space = space;
            data_bps[num_data_bps].addr  = addr;
            num_data_bps++;
            cJSON_AddNumberToObject(rb, "id", bp_id);
        }
        cJSON_AddBoolToObject(rb, "verified", why == NULL);
        if (why) cJSON_AddStringToObject(rb, "message", why);
        cJSON_AddItemToArray(arr, rb);
    }
    cJSON_AddItemToObject(body, "breakpoints", arr);
    send_dap_response(seq, "setDataBreakpoints", true, body);
    return 0;
}

// ─── loadedSources (files from the loaded .dbg) ──────────────────────
static int handle_dap_loaded_sources(int seq, cJSON *args) {
    (void)args;
    cJSON *body = cJSON_CreateObject();
    cJSON *arr  = cJSON_CreateArray();
    // Which generated files are left out depends on what is on disk, and the
    // names come from a .dbg an MCP client may have written.
    const bool all = current_caller == DBG_CALLER_MCP;
    int n = all ? dbg_info_all_file_count() : dbg_info_file_count();
    for (int i = 0; i < n; i++) {
        const char *nm = NULL;
        if ((all ? dbg_info_all_file_at(i, &nm) : dbg_info_file_at(i, &nm)) && nm) {
            const char *base = nm;
            for (const char *p = nm; *p; ++p) if (*p == '/' || *p == '\\') base = p + 1;
            char uri[1024];
            cJSON *src = cJSON_CreateObject();
            cJSON_AddStringToObject(src, "name", base);
            cJSON_AddStringToObject(src, "path", caller_path_out(nm, uri, sizeof uri));
            cJSON_AddItemToArray(arr, src);
        }
    }
    cJSON_AddItemToObject(body, "sources", arr);
    send_dap_response(seq, "loadedSources", true, body);
    return 0;
}

// ─── source (return file content by path) ────────────────────────────
static int handle_dap_source(int seq, cJSON *args) {
    const char *path = NULL;
    char local[1024];
    if (args) {
        cJSON *src = cJSON_GetObjectItemCaseSensitive(args, "source");
        if (src) {
            cJSON *p = cJSON_GetObjectItemCaseSensitive(src, "path");
            if (!p) p = cJSON_GetObjectItemCaseSensitive(src, "name");
            if (p && cJSON_IsString(p)) {
                dap_path_in(p->valuestring, local, sizeof local);
                path = local;
            }
        }
    }
    const source_file_t *sf = path ? source_view_get(path) : NULL;
    if (sf && sf->found) {
        size_t total = 1;
        for (int i = 0; i < sf->count; i++) total += strlen(sf->lines[i]) + 1;
        char *buf = (char *)malloc(total);
        cJSON *body = cJSON_CreateObject();
        if (buf) {
            size_t o = 0;
            for (int i = 0; i < sf->count; i++) {
                size_t l = strlen(sf->lines[i]);
                memcpy(buf + o, sf->lines[i], l); o += l;
                buf[o++] = '\n';
            }
            buf[o] = '\0';
            cJSON_AddStringToObject(body, "content", buf);
            free(buf);
        } else {
            cJSON_AddStringToObject(body, "content", "");
        }
        send_dap_response(seq, "source", true, body);
    } else {
        send_dap_error_response(seq, "source", "source not available");
    }
    return 0;
}

// ─── breakpointLocations (which lines in a range map to code) ────────
static int handle_dap_breakpoint_locations(int seq, cJSON *args) {
    const char *path = NULL; int line = 0, endLine = 0;
    char local[1024];
    if (args) {
        cJSON *src = cJSON_GetObjectItemCaseSensitive(args, "source");
        if (src) {
            cJSON *p = cJSON_GetObjectItemCaseSensitive(src, "path");
            if (p && cJSON_IsString(p)) {
                dap_path_in(p->valuestring, local, sizeof local);
                path = local;
            }
        }
        cJSON *l  = cJSON_GetObjectItemCaseSensitive(args, "line");    if (l && cJSON_IsNumber(l))  line = dap_line_in((int)l->valuedouble);
        cJSON *el = cJSON_GetObjectItemCaseSensitive(args, "endLine"); if (el && cJSON_IsNumber(el)) endLine = dap_line_in((int)el->valuedouble);
    }
    if (endLine < line) endLine = line;
    cJSON *body = cJSON_CreateObject();
    cJSON *arr  = cJSON_CreateArray();
    for (int ln = line; path && ln <= endLine; ln++) {
        dbg_addr_t addr;
        if (dbg_info_source_to_addr(path, ln, &addr)) {
            cJSON *loc = cJSON_CreateObject();
            cJSON_AddNumberToObject(loc, "line", dap_line_out(ln));
            cJSON_AddItemToArray(arr, loc);
        }
    }
    cJSON_AddItemToObject(body, "breakpoints", arr);
    send_dap_response(seq, "breakpointLocations", true, body);
    return 0;
}

// ─── x16/sourceLocation (an address's line, or a line's addresses) ───

#define SOURCE_NAMES_MAX 8
#define SOURCE_HITS_MAX  32

// A file name that fits more than one file in the debug info.
static void source_ambiguous(char *out, size_t n, const char *const *names, int total) {
    size_t o = (size_t)snprintf(out, n, "that could be any of:");
    const int shown = total < SOURCE_NAMES_MAX ? total : SOURCE_NAMES_MAX;
    for (int i = 0; i < shown && o < n; i++)
        o += (size_t)snprintf(out + o, n - o, "%s %s", i ? "," : "", names[i]);
    if (total > shown && o < n)
        snprintf(out + o, n - o, " and %d more", total - shown);
}

static cJSON *source_location_item(uint32_t addr, int bank, const char *file, int line) {
    char uri[1024];
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "address", addr);
    if (bank >= 0) cJSON_AddNumberToObject(o, "bank", bank);
    else           cJSON_AddNullToObject(o, "bank");
    cJSON_AddStringToObject(o, "file", caller_path_out(file, uri, sizeof uri));
    cJSON_AddNumberToObject(o, "line", caller_line_out(line));
    return o;
}

// Where a line generated code, as locations into `arr`. False if the file name
// fits more than one file, with the candidates in `why`.
static bool source_line_places(const char *file, int client_line, int bank, cJSON *arr,
                               bool *truncated, char *why, size_t whyn) {
    char local[1024];
    caller_path_in(file, local, sizeof local);
    const int line = caller_line_in(client_line);
    *truncated = false;

    const char *names[SOURCE_NAMES_MAX];
    const int matched = dbg_info_find_source(local, names, SOURCE_NAMES_MAX);
    if (matched > 1) {
        source_ambiguous(why, whyn, names, matched);
        return false;
    }
    if (matched == 1) {
        dbg_line_hit_t hits[SOURCE_HITS_MAX];
        const int total = dbg_info_line_addrs(names[0], line, hits, SOURCE_HITS_MAX);
        const int have  = total < SOURCE_HITS_MAX ? total : SOURCE_HITS_MAX;
        // A bank picks the segments known to be there, or failing that the
        // ones not known to be anywhere. Those then name one place each.
        bool exact = false;
        for (int i = 0; i < have; i++)
            exact = exact || (bank >= 0 && hits[i].ram_bank == bank);
        for (int i = 0; i < have; i++) {
            if (bank >= 0 && hits[i].ram_bank != (exact ? bank : -1)) continue;
            bool repeated = false;
            for (int j = 0; bank >= 0 && j < i && !repeated; j++)
                repeated = hits[j].addr == hits[i].addr &&
                           hits[j].ram_bank == (exact ? bank : -1);
            if (repeated) continue;
            cJSON *o = source_location_item(hits[i].addr, hits[i].ram_bank, hits[i].file, line);
            if (hits[i].segment) cJSON_AddStringToObject(o, "segment", hits[i].segment);
            cJSON_AddItemToArray(arr, o);
        }
        *truncated = total > SOURCE_HITS_MAX;
    }
    return true;
}

static int handle_x16_source_location(int seq, cJSON *args) {
    static const char cmd[] = "x16/sourceLocation";
    cJSON *ai = args ? cJSON_GetObjectItemCaseSensitive(args, "address") : NULL;
    cJSON *fi = args ? cJSON_GetObjectItemCaseSensitive(args, "file") : NULL;
    cJSON *li = args ? cJSON_GetObjectItemCaseSensitive(args, "line") : NULL;
    cJSON *bi = args ? cJSON_GetObjectItemCaseSensitive(args, "bank") : NULL;
    if ((ai != NULL) == (fi != NULL || li != NULL)) {
        send_dap_error_response(seq, cmd, "give 'address', or 'file' and 'line'");
        return 0;
    }
    int bank = -1;
    if (bi && !cJSON_IsNull(bi)) {
        if (!cJSON_IsNumber(bi) || bi->valuedouble < 0 || bi->valuedouble > 255 ||
            bi->valuedouble != (int)bi->valuedouble) {
            send_dap_error_response(seq, cmd, "'bank' must be a whole number from 0 to 255");
            return 0;
        }
        bank = (int)bi->valuedouble;
    }

    cJSON *body = cJSON_CreateObject();
    cJSON *arr  = cJSON_CreateArray();
    cJSON_AddItemToObject(body, "locations", arr);

    if (ai) {
        uint32_t addr = 0;
        bool is_vram = false, ok;
        if (cJSON_IsNumber(ai)) {
            ok = ai->valuedouble >= 0 && ai->valuedouble <= 0xFFFF &&
                 ai->valuedouble == (int)ai->valuedouble;
            if (ok) addr = (uint32_t)ai->valuedouble;
        } else {
            ok = cJSON_IsString(ai) && dap_parse_mem_ref(ai->valuestring, &is_vram, &addr) &&
                 !is_vram && addr <= 0xFFFF;
        }
        if (!ok) {
            cJSON_Delete(body);
            send_dap_error_response(seq, cmd, "'address' must be a CPU address from $0000 to $FFFF");
            return 0;
        }
        const int ram_bank = bank >= 0 ? bank : (int)memory_get_ram_bank();
        const char *file = NULL;
        int line = 0;
        const dbg_bank_result_t how = dbg_info_addr_to_source_banked_ex(addr, ram_bank, &file, &line);
        if (how != DBG_BANK_NO_MATCH && file) {
            const bool banked = addr >= 0xA000 && addr < 0xC000;
            cJSON *o = source_location_item(addr, banked ? ram_bank : -1, file, line);
            cJSON_AddBoolToObject(o, "bankKnown", how == DBG_BANK_RESOLVED);
            cJSON_AddItemToArray(arr, o);
        }
    } else {
        if (!fi || !cJSON_IsString(fi) || !fi->valuestring[0] || !li || !cJSON_IsNumber(li) ||
            li->valuedouble < 0 || li->valuedouble > 0x7FFFFFF0 ||
            li->valuedouble != (int)li->valuedouble) {
            cJSON_Delete(body);
            send_dap_error_response(seq, cmd, "'file' and 'line' go together");
            return 0;
        }
        char why[256];
        bool truncated = false;
        if (!source_line_places(fi->valuestring, (int)li->valuedouble, bank, arr, &truncated,
                                why, sizeof why)) {
            cJSON_Delete(body);
            send_dap_error_response(seq, cmd, why);
            return 0;
        }
        if (truncated) cJSON_AddBoolToObject(body, "truncated", true);
    }
    send_dap_response(seq, cmd, true, body);
    return 0;
}

// ─── x16/runTo ──────────────────────────────────────────────────────

// The one place a source line names. Refused when it has none or several, since
// a run-to has a single target.
static bool run_to_line_target(cJSON *fi, cJSON *li, int bank, struct breakpoint *bp,
                               char *why, size_t whyn) {
    if (!fi || !cJSON_IsString(fi) || !fi->valuestring[0] || !li || !cJSON_IsNumber(li) ||
        li->valuedouble < 0 || li->valuedouble > 0x7FFFFFF0 ||
        li->valuedouble != (int)li->valuedouble) {
        snprintf(why, whyn, "'file' and 'line' go together");
        return false;
    }
    cJSON *places = cJSON_CreateArray();
    bool truncated = false;
    bool ok = source_line_places(fi->valuestring, (int)li->valuedouble, bank, places, &truncated,
                                 why, whyn);
    const int n = cJSON_GetArraySize(places);
    if (ok && n == 0) {
        snprintf(why, whyn, "no code was generated for that line");
        ok = false;
    } else if (ok && (n > 1 || truncated)) {
        size_t o = (size_t)snprintf(why, whyn, "that line has code in %s%d places:",
                                    truncated ? "over " : "", n);
        for (int i = 0; i < n && i < 4 && o < whyn; i++) {
            cJSON *p = cJSON_GetArrayItem(places, i);
            cJSON *b = cJSON_GetObjectItemCaseSensitive(p, "bank");
            cJSON *g = cJSON_GetObjectItemCaseSensitive(p, "segment");
            const unsigned a = (unsigned)cJSON_GetObjectItemCaseSensitive(p, "address")->valuedouble;
            char at[24];
            if (cJSON_IsNumber(b)) snprintf(at, sizeof at, "%02X:%04X", (unsigned)b->valuedouble, a);
            else                   snprintf(at, sizeof at, "$%04X", a);
            o += (size_t)snprintf(why + o, whyn - o, " %s%s%.40s", at, cJSON_IsString(g) ? " in " : "",
                                  cJSON_IsString(g) ? g->valuestring : "");
        }
        if (o < whyn) snprintf(why + o, whyn - o, "; give an address, or a 'bank' for one of them");
        ok = false;
    } else if (ok) {
        cJSON *p = cJSON_GetArrayItem(places, 0);
        cJSON *b = cJSON_GetObjectItemCaseSensitive(p, "bank");
        ok = debug_bp_from_ref((long)cJSON_GetObjectItemCaseSensitive(p, "address")->valuedouble,
                               cJSON_IsNumber(b) ? (int)b->valuedouble : DEBUG_BANK_ANY, 0, bp);
        if (!ok) snprintf(why, whyn, "that line's address cannot be run to");
    }
    cJSON_Delete(places);
    return ok;
}

// Runs a paused machine to one address, in one bank if it names one, and answers
// when it stops for any reason.
static int handle_x16_run_to(int seq, cJSON *args) {
    static const char cmd[] = "x16/runTo";
    const debug_owner_t owner = caller_step_owner();
    if (DEBUGStepHeldByOther(owner)) {
        send_dap_error_response(seq, cmd, "another client's step or run-to is in progress");
        return 0;
    }
    if (step.pending || run_to.pending) {
        send_dap_error_response(seq, cmd, step.pending ? "a step is already in progress"
                                                       : "a run-to is already in progress");
        return 0;
    }
    if (!DEBUGIsPaused()) {
        send_dap_error_response(seq, cmd, "the machine must be paused");
        return 0;
    }

    cJSON *ai = args ? cJSON_GetObjectItemCaseSensitive(args, "address") : NULL;
    cJSON *fi = args ? cJSON_GetObjectItemCaseSensitive(args, "file") : NULL;
    cJSON *li = args ? cJSON_GetObjectItemCaseSensitive(args, "line") : NULL;
    cJSON *bi = args ? cJSON_GetObjectItemCaseSensitive(args, "bank") : NULL;
    cJSON *ti = args ? cJSON_GetObjectItemCaseSensitive(args, "timeoutMs") : NULL;
    if ((ai != NULL) == (fi != NULL || li != NULL)) {
        send_dap_error_response(seq, cmd, "give 'address', or 'file' and 'line'");
        return 0;
    }
    int bank = -1;
    if (bi && !cJSON_IsNull(bi)) {
        if (!cJSON_IsNumber(bi) || bi->valuedouble < 0 || bi->valuedouble > 255 ||
            bi->valuedouble != (int)bi->valuedouble) {
            send_dap_error_response(seq, cmd, "'bank' must be a whole number from 0 to 255");
            return 0;
        }
        bank = (int)bi->valuedouble;
    }
    int timeout = RUN_TO_WAIT_DEFAULT;
    if (ti) {
        if (!cJSON_IsNumber(ti) || ti->valuedouble < 0 || ti->valuedouble > RUN_TO_WAIT_LIMIT ||
            ti->valuedouble != (int)ti->valuedouble) {
            send_dap_error_response(seq, cmd, "'timeoutMs' must be a whole number from 0 to 30000");
            return 0;
        }
        timeout = (int)ti->valuedouble;
    }

    struct breakpoint bp;
    char why[320];
    if (ai) {
        bool ok;
        if (cJSON_IsNumber(ai))
            ok = ai->valuedouble >= 0 && ai->valuedouble <= 0xFFFFFF &&
                 ai->valuedouble == (int)ai->valuedouble &&
                 debug_bp_from_ref((long)ai->valuedouble, DEBUG_BANK_ANY, 0, &bp);
        else
            ok = cJSON_IsString(ai) && dap_parse_instruction_ref(ai->valuestring, 0, &bp);
        if (!ok) {
            send_dap_error_response(seq, cmd,
                                    "unreadable 'address' (expected e.g. $C000, 04:C000 or sym:main)");
            return 0;
        }
    } else if (!run_to_line_target(fi, li, bank, &bp, why, sizeof why)) {
        send_dap_error_response(seq, cmd, why);
        return 0;
    }

    int want = bp.x16Bank;
    if (bank >= 0) {
        if (want != DEBUG_BANK_ANY && want != bank) {
            send_dap_error_response(seq, cmd, "'bank' disagrees with the bank in the address");
            return 0;
        }
        want = bank;
    }
    if (want != DEBUG_BANK_ANY && debug_normalise_bank(want, bp.pc, bp.bank) == DEBUG_BANK_ANY) {
        snprintf(why, sizeof why, "$%04X is not in a banked window, so a bank cannot apply",
                 (unsigned)bp.pc);
        send_dap_error_response(seq, cmd, why);
        return 0;
    }
    const dbg_run_target_t target = { (uint16_t)bp.pc, bp.bank,
                                      debug_normalise_bank(want, bp.pc, bp.bank) };

    if (current_caller == DBG_CALLER_DAP) {
        run_to.pending  = true;
        run_to.seq      = seq;
        run_to.target   = target;
        run_to.deadline = SDL_GetTicks() + (Uint32)timeout;
        // The resume hook tells the client it is running. The reply waits.
        DEBUGRunToBank(target.pc, target.bank, target.x16Bank, owner);
        return 1;
    }
    DEBUGRunToBank(target.pc, target.bank, target.x16Bank, owner);
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "pc", target.pc);
    cJSON_AddNumberToObject(body, "pbr", target.bank);
    cJSON_AddNumberToObject(body, "x16Bank", target.x16Bank);
    send_dap_response(seq, cmd, true, body);
    return 1;
}

// ─── stepInTargets (the call destination at a JSR/JSL) ───────────────
static int handle_dap_step_in_targets(int seq, cJSON *args) {
    (void)args;
    cJSON *body = cJSON_CreateObject();
    cJSON *arr  = cJSON_CreateArray();
    uint8_t opc = debug_read6502(regs.pc, regs.k, USE_CURRENT_X16_BANK);
    if (opc == 0x20 || opc == 0x22) { // JSR abs / JSL
        uint16_t target = debug_read6502((uint16_t)(regs.pc + 1), regs.k, USE_CURRENT_X16_BANK) |
                          (debug_read6502((uint16_t)(regs.pc + 2), regs.k, USE_CURRENT_X16_BANK) << 8);
        const char *label = NULL;
        char lbl[64];
        if (dbg_info_addr_to_label(target, &label) && label)
            snprintf(lbl, sizeof lbl, "%s ($%04X)", label, target);
        else
            snprintf(lbl, sizeof lbl, "$%04X", target);
        cJSON *t = cJSON_CreateObject();
        cJSON_AddNumberToObject(t, "id", 1);
        cJSON_AddStringToObject(t, "label", lbl);
        cJSON_AddItemToArray(arr, t);
    }
    cJSON_AddItemToObject(body, "targets", arr);
    send_dap_response(seq, "stepInTargets", true, body);
    return 0;
}

// ─── Breakpoints one at a time, for MCP ─────────────────────────────
//
// DAP's breakpoint requests each replace a whole list that belongs to the client,
// so they are not callable. These add and remove a single claim under MCP's own
// owner, leaving anything another owner holds at the same address armed.

// The core key a request names. A condition can pin the bank too.
static bool mcp_bp_key(cJSON *args, const char *cond, struct breakpoint *bp,
                       char *why, size_t whyn) {
    cJSON *ad = cJSON_GetObjectItemCaseSensitive(args, "address");
    cJSON *xb = cJSON_GetObjectItemCaseSensitive(args, "bank");
    int cond_bank = -1;
    if (!ad || !cJSON_IsString(ad) || !dap_parse_instruction_ref(ad->valuestring, 0, bp)) {
        snprintf(why, whyn, "unreadable address (expected e.g. $C000, or 04:C000 for a bank)");
        return false;
    }
    if (!dap_check_condition(cond, &cond_bank, why, whyn)) return false;

    int want = bp->x16Bank;
    if (xb) {
        const int b = (int)xb->valuedouble;
        if (want != DEBUG_BANK_ANY && want != b) {
            snprintf(why, whyn, "'bank' disagrees with the bank in the address");
            return false;
        }
        want = b;
    }
    if (cond_bank != -1) {
        const int cb = (cond_bank == DAP_BANK_TERM_ANY) ? DEBUG_BANK_ANY : cond_bank;
        if (want != DEBUG_BANK_ANY && cb != want) {
            snprintf(why, whyn, "the condition's bank disagrees with the breakpoint's bank");
            return false;
        }
        want = cb;
    }
    if (want != DEBUG_BANK_ANY && debug_normalise_bank(want, bp->pc, bp->bank) == DEBUG_BANK_ANY) {
        snprintf(why, whyn, "$%04X is not in a banked window, so a bank cannot apply",
                 (unsigned)bp->pc);
        return false;
    }
    bp->x16Bank = debug_normalise_bank(want, bp->pc, bp->bank);
    return true;
}

// A watch covers one CPU address range in every bank, or one range of VRAM.
static bool mcp_watch_range(cJSON *args, uint8_t *space, uint32_t *addr, uint32_t *len,
                            char *why, size_t whyn) {
    cJSON *ad = cJSON_GetObjectItemCaseSensitive(args, "address");
    cJSON *ln = cJSON_GetObjectItemCaseSensitive(args, "length");
    if (ad && cJSON_IsString(ad) && dap_has_prefix_i(ad->valuestring, "vram:")) {
        bool is_vram = false;
        uint32_t a = 0;
        if (!dap_parse_mem_ref(ad->valuestring, &is_vram, &a) || a > 0x1FFFF) {
            snprintf(why, whyn, "a VRAM watch takes an address from $00000 to $1FFFF");
            return false;
        }
        if (cJSON_HasObjectItem(args, "bank")) {
            snprintf(why, whyn, "VRAM has no banks, so a VRAM watch takes no 'bank'");
            return false;
        }
        const int vl = ln ? (int)ln->valuedouble : 1;
        if (vl < 1 || (uint32_t)vl > 0x20000 - a) {
            snprintf(why, whyn, "the watched range runs past $1FFFF");
            return false;
        }
        *space = DEBUG_WP_SPACE_VRAM;
        *addr  = a;
        *len   = (uint32_t)vl;
        return true;
    }
    struct breakpoint bp;
    if (!ad || !cJSON_IsString(ad) || !dap_parse_instruction_ref(ad->valuestring, 0, &bp) ||
        bp.bank != 0 || bp.x16Bank != DEBUG_BANK_ANY) {
        snprintf(why, whyn, "a write watch takes a 16-bit CPU address, e.g. $0400");
        return false;
    }
    if (cJSON_HasObjectItem(args, "bank")) {
        snprintf(why, whyn, "a write watch covers every bank, so it takes no 'bank'");
        return false;
    }
    const int l = ln ? (int)ln->valuedouble : 1;
    if (l < 1 || bp.pc + l > 0x10000) {
        snprintf(why, whyn, "the watched range runs past $FFFF");
        return false;
    }
    *space = DEBUG_WP_SPACE_CPU;
    *addr  = bp.pc;
    *len   = (uint32_t)l;
    return true;
}

// The access a watch kind asks for, or 0 for an exec breakpoint.
static uint8_t mcp_watch_access(cJSON *args) {
    cJSON *k = cJSON_GetObjectItemCaseSensitive(args, "kind");
    if (!k || !cJSON_IsString(k)) return 0;
    if (!strcmp(k->valuestring, "write")) return DEBUG_WP_ACCESS_WRITE;
    if (!strcmp(k->valuestring, "read")) return DEBUG_WP_ACCESS_READ;
    if (!strcmp(k->valuestring, "access")) return DEBUG_WP_ACCESS_BOTH;
    return 0;
}

static const char *client_write_root(char *err, size_t errn);

#define MCP_DUMP_REGION_MAX 4096u
#define MCP_DUMP_FILE_MAX   (256ull * 1024 * 1024)

// What a client's dump asks for, and the new file it may write. Nothing is
// created here.
static bool mcp_dump_request(cJSON *dump, uint32_t *addr, uint32_t *len, int *bank, int *n,
                             char *target, size_t target_n, char *why, size_t whyn) {
    cJSON *file = cJSON_GetObjectItemCaseSensitive(dump, "file");
    cJSON *regs = cJSON_GetObjectItemCaseSensitive(dump, "regions");
    if (!file || !cJSON_IsString(file) || !file->valuestring[0] || !regs || !cJSON_IsArray(regs) ||
        !dap_dump_regions(regs, addr, len, bank, n) || *n < 1) {
        snprintf(why, whyn, "'dump' needs a file and 1 to %d regions", DAP_DUMP_REGIONS);
        return false;
    }
    for (int i = 0; i < *n; i++) {
        if (len[i] < 1 || len[i] > MCP_DUMP_REGION_MAX) {
            snprintf(why, whyn, "a dump region is 1 to %u bytes", MCP_DUMP_REGION_MAX);
            return false;
        }
    }
    const char *folder = client_write_root(why, whyn);
    if (!folder) return false;
    bool exists = false;
    if (!host_write_path(folder, file->valuestring, target, target_n, &exists, why, whyn))
        return false;
    if (exists) {
        snprintf(why, whyn, "that file exists; a dump breakpoint writes a new file");
        return false;
    }
    return true;
}

static int handle_mcp_set_breakpoint(int seq, cJSON *args) {
    static const char cmd[] = "mcp/setBreakpoint";
    cJSON *ci = cJSON_GetObjectItemCaseSensitive(args, "condition");
    cJSON *hi = cJSON_GetObjectItemCaseSensitive(args, "hitCondition");
    cJSON *dump = cJSON_GetObjectItemCaseSensitive(args, "dump");
    const char *cond = (ci && cJSON_IsString(ci)) ? ci->valuestring : "";
    const char *hit  = (hi && cJSON_IsString(hi)) ? hi->valuestring : "";
    char why[420], where[24];
    cJSON *body;

    const uint8_t access = mcp_watch_access(args);
    if (access && dump) {
        send_dap_error_response(seq, cmd, "'dump' applies to an exec breakpoint");
        return 0;
    }
    if (access) {
        uint8_t  space = DEBUG_WP_SPACE_CPU;
        uint32_t addr = 0, len = 1;
        uint32_t val = 0;
        int op = 0;
        const bool has_value = cond[0] != '\0';
        if (!mcp_watch_range(args, &space, &addr, &len, why, sizeof why)) {
            send_dap_error_response(seq, cmd, why);
            return 0;
        }
        if (hit[0]) {
            send_dap_error_response(seq, cmd, "a watch takes no hit count");
            return 0;
        }
        if (has_value && len > 1) {
            send_dap_error_response(seq, cmd, "a value condition applies to a single byte");
            return 0;
        }
        if (has_value && (!dap_parse_simple_cmp(cond, &op, &val) || val > 0xFF)) {
            send_dap_error_response(seq, cmd, "cannot parse condition (expected a byte comparison, e.g. == $05)");
            return 0;
        }
        // Set again with the same values is fine; a different range or filter
        // would change what an earlier claim, even this client's, is watching.
        cJSON *xf = cJSON_GetObjectItemCaseSensitive(args, "excludeFetch");
        // Setting a watch this client alone holds and has disabled arms it again,
        // as it does for an exec breakpoint.
        const int held = debug_wp_find(space, addr, DEBUG_BANK_ANY);
        const bool rearm = held >= 0 && !debug_wp_at(held)->active &&
                           debug_wp_at(held)->owners == (uint16_t)(1u << DEBUG_OWNER_MCP);
        if (rearm) debug_wp_set_active_at(held, true);
        const char *refused = dap_arm_watch(space, access, !(xf && cJSON_IsFalse(xf)), addr, len,
                                            has_value, op, (uint8_t)val, 0, DEBUG_OWNER_MCP);
        if (refused) {
            if (rearm) debug_wp_set_active_at(held, false);
            send_dap_error_response(seq, cmd, refused);
            return 0;
        }
        const bool vram = space == DEBUG_WP_SPACE_VRAM;
        snprintf(where, sizeof where, vram ? "$%05X" : "$%04X", (unsigned)addr);
        body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "kind", cJSON_GetObjectItemCaseSensitive(args, "kind")->valuestring);
        cJSON_AddStringToObject(body, "space", vram ? "vram" : "cpu");
        cJSON_AddStringToObject(body, "address", where);
        cJSON_AddNumberToObject(body, "length", len);
        send_dap_response(seq, cmd, true, body);
        return 0;
    }

    struct breakpoint bp;
    if (!mcp_bp_key(args, cond, &bp, why, sizeof why)) {
        send_dap_error_response(seq, cmd, why);
        return 0;
    }
    if (hit[0] && !dap_check_hit_condition(hit)) {
        send_dap_error_response(seq, cmd, "cannot parse hitCondition (expected N: stop from the Nth hit on)");
        return 0;
    }
    cJSON *log_item = cJSON_GetObjectItemCaseSensitive(args, "log");
    const char *log_text = log_item && cJSON_IsString(log_item) ? log_item->valuestring : "";
    if (log_text[0] && !dbg_log_check(log_text, why, sizeof why)) {
        send_dap_error_response(seq, cmd, why);
        return 0;
    }
    const dbg_addr_t a24 = ((dbg_addr_t)bp.bank << 16) | (dbg_addr_t)bp.pc;
    const uint16_t mine = (uint16_t)(1u << DEBUG_OWNER_MCP);
    const int at = debug_bp_find(bp.pc, bp.bank, bp.x16Bank);
    const uint16_t owners = at >= 0 ? debug_bp_at(at)->owners : 0;
    const bool had = (owners & mine) != 0;
    const bool others = (owners & (uint16_t)~mine) != 0;
    const bool was_enabled = at >= 0 && debug_bp_at(at)->enabled;
    // This client's own condition may change, so only someone else's can conflict.
    if (others && dap_bp_cond_conflicts(a24, bp.x16Bank, cond, hit)) {
        snprintf(why, sizeof why, "another breakpoint at $%04X has a different condition or hit count",
                 (unsigned)bp.pc);
        send_dap_error_response(seq, cmd, why);
        return 0;
    }

    uint32_t dump_addr[DAP_DUMP_REGIONS], dump_len[DAP_DUMP_REGIONS];
    int      dump_bank[DAP_DUMP_REGIONS], dump_n = 0;
    char     target[4200];
    if (dump) {
        debug_owner_t holder = DEBUG_OWNER_MCP;
        if (debug_bp_dump_info(bp.pc, bp.bank, bp.x16Bank, &holder, NULL, NULL)) {
            send_dap_error_response(seq, cmd, holder == DEBUG_OWNER_MCP
                ? "this client already has a dump breakpoint here; remove it first"
                : "another client has a dump breakpoint at this address");
            return 0;
        }
        if (!mcp_dump_request(dump, dump_addr, dump_len, dump_bank, &dump_n, target, sizeof target,
                              why, sizeof why)) {
            send_dap_error_response(seq, cmd, why);
            return 0;
        }
    }

    if (debug_bp_add_for(bp, DEBUG_OWNER_MCP) == DEBUG_ADD_FULL) {
        send_dap_error_response(seq, cmd, "the breakpoint table is full");
        return 0;
    }
    if (dump) {
        FILE *f = host_create_new(target);
        if (!f || !debug_bp_set_dump_file(bp.pc, bp.bank, bp.x16Bank, DEBUG_OWNER_MCP, f,
                                          MCP_DUMP_FILE_MAX, dump_n, dump_addr, dump_len, dump_bank)) {
            if (f) fclose(f);
            if (!had) debug_bp_remove_for(bp.pc, bp.bank, bp.x16Bank, DEBUG_OWNER_MCP);
            if (debug_bp_find(bp.pc, bp.bank, bp.x16Bank) < 0)
                debug_bp_forget(bp.pc, bp.bank, bp.x16Bank);
            else if (!was_enabled)
                debug_bp_set_enabled(bp.pc, bp.bank, bp.x16Bank, false);
            send_dap_error_response(seq, cmd, "cannot create that file");
            return 0;
        }
        debug_bp_dump_read = dap_dump_read;
    } else {
        debug_bp_clear_dump_for(bp.pc, bp.bank, bp.x16Bank, DEBUG_OWNER_MCP);
    }
    dap_apply_bp_condition(a24, cond, hit, bp.x16Bank);
    if (!log_text[0]) {
        logpoint_clear(bp.pc, bp.bank, bp.x16Bank, DEBUG_OWNER_MCP);
    } else if (!logpoint_set(bp.pc, bp.bank, bp.x16Bank, DEBUG_OWNER_MCP, log_text, why, sizeof why)) {
        // Only out of memory: checked above. It stays a plain breakpoint if this
        // client already held one, and goes if this call created it.
        if (!had) debug_bp_remove_for(bp.pc, bp.bank, bp.x16Bank, DEBUG_OWNER_MCP);
        if (debug_bp_find(bp.pc, bp.bank, bp.x16Bank) < 0)
            debug_bp_forget(bp.pc, bp.bank, bp.x16Bank);
        else if (!was_enabled)
            debug_bp_set_enabled(bp.pc, bp.bank, bp.x16Bank, false);
        send_dap_error_response(seq, cmd, why);
        return 0;
    }

    dap_fmt_bp(where, sizeof where, bp.pc, bp.bank, bp.x16Bank);
    body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "kind", "exec");
    cJSON_AddStringToObject(body, "address", where);
    if (dump) cJSON_AddStringToObject(body, "dumpFile", target);
    if (log_text[0]) cJSON_AddStringToObject(body, "log", log_text);
    if (dump || log_text[0]) {
        const int now = debug_bp_find(bp.pc, bp.bank, bp.x16Bank);
        const uint16_t held = now >= 0 ? debug_bp_at(now)->owners : 0;
        const uint16_t quiet = debug_bp_quiet_owners(bp.pc, bp.bank, bp.x16Bank);
        cJSON_AddBoolToObject(body, "alsoStops", (held & (uint16_t)~quiet) != 0);
    }
    send_dap_response(seq, cmd, true, body);
    return 0;
}

// Enabled is one flag on an entry, so it is refused where anyone else holds it.
static int handle_mcp_enable_breakpoint(int seq, cJSON *args) {
    static const char cmd[] = "mcp/enableBreakpoint";
    static const char shared[] =
        "another client holds a breakpoint at this address, and enabling applies to all of them";
    cJSON *en = cJSON_GetObjectItemCaseSensitive(args, "enabled");
    if (!en || !cJSON_IsBool(en)) {
        send_dap_error_response(seq, cmd, "'enabled' must be true or false");
        return 0;
    }
    const bool on = cJSON_IsTrue(en);
    const uint16_t mine = (uint16_t)(1u << DEBUG_OWNER_MCP);
    char why[200], where[24];
    cJSON *body = cJSON_CreateObject();

    const uint8_t access = mcp_watch_access(args);
    if (access) {
        uint8_t  space = DEBUG_WP_SPACE_CPU;
        uint32_t addr = 0, len = 1;
        const char *refused = NULL;
        if (!mcp_watch_range(args, &space, &addr, &len, why, sizeof why)) refused = why;
        const int idx = refused ? -1 : debug_wp_find(space, addr, DEBUG_BANK_ANY);
        const struct watchpoint *w = idx >= 0 ? debug_wp_at(idx) : NULL;
        if (!refused && (!w || !(w->owners & mine))) refused = "MCP has no watch at this address";
        if (!refused && w->access != access) refused = "the watch at this address is of a different kind";
        if (!refused && (w->owners & (uint16_t)~mine)) refused = shared;
        if (refused) {
            cJSON_Delete(body);
            send_dap_error_response(seq, cmd, refused);
            return 0;
        }
        debug_wp_set_active_at(idx, on);
        const bool vram = space == DEBUG_WP_SPACE_VRAM;
        snprintf(where, sizeof where, vram ? "$%05X" : "$%04X", (unsigned)addr);
        cJSON_AddStringToObject(body, "kind", cJSON_GetObjectItemCaseSensitive(args, "kind")->valuestring);
        cJSON_AddStringToObject(body, "space", vram ? "vram" : "cpu");
    } else {
        struct breakpoint bp;
        const char *refused = NULL;
        if (!mcp_bp_key(args, "", &bp, why, sizeof why)) refused = why;
        const int idx = refused ? -1 : debug_bp_find(bp.pc, bp.bank, bp.x16Bank);
        const uint16_t owners = idx >= 0 ? debug_bp_at(idx)->owners : 0;
        if (!refused && !(owners & mine)) refused = "MCP has no breakpoint at this address";
        if (!refused && (owners & (uint16_t)~mine)) refused = shared;
        if (refused) {
            cJSON_Delete(body);
            send_dap_error_response(seq, cmd, refused);
            return 0;
        }
        debug_bp_set_enabled(bp.pc, bp.bank, bp.x16Bank, on);
        dap_fmt_bp(where, sizeof where, bp.pc, bp.bank, bp.x16Bank);
        cJSON_AddStringToObject(body, "kind", "exec");
    }
    cJSON_AddStringToObject(body, "address", where);
    cJSON_AddBoolToObject(body, "enabled", on);
    send_dap_response(seq, cmd, true, body);
    return 0;
}

static int handle_mcp_remove_breakpoint(int seq, cJSON *args) {
    static const char cmd[] = "mcp/removeBreakpoint";
    char why[200];
    bool still_armed;

    const uint8_t access = mcp_watch_access(args);
    if (access) {
        uint8_t  space = DEBUG_WP_SPACE_CPU;
        uint32_t addr = 0, len = 1;
        if (!mcp_watch_range(args, &space, &addr, &len, why, sizeof why)) {
            send_dap_error_response(seq, cmd, why);
            return 0;
        }
        if (!debug_wp_has_owner_ex(space, addr, DEBUG_BANK_ANY, DEBUG_OWNER_MCP)) {
            send_dap_error_response(seq, cmd, "MCP has no watch at this address");
            return 0;
        }
        if (debug_wp_at(debug_wp_find(space, addr, DEBUG_BANK_ANY))->access != access) {
            send_dap_error_response(seq, cmd, "the watch at this address is of a different kind");
            return 0;
        }
        debug_wp_remove_ex(space, addr, DEBUG_BANK_ANY, DEBUG_OWNER_MCP);
        still_armed = debug_wp_find(space, addr, DEBUG_BANK_ANY) >= 0;
    } else {
        struct breakpoint bp;
        if (!mcp_bp_key(args, "", &bp, why, sizeof why)) {
            send_dap_error_response(seq, cmd, why);
            return 0;
        }
        if (!debug_bp_has_owner(bp.pc, bp.bank, bp.x16Bank, DEBUG_OWNER_MCP)) {
            send_dap_error_response(seq, cmd, "MCP has no breakpoint at this address");
            return 0;
        }
        debug_bp_remove_for(bp.pc, bp.bank, bp.x16Bank, DEBUG_OWNER_MCP);
        still_armed = debug_bp_find(bp.pc, bp.bank, bp.x16Bank) >= 0;
        if (!still_armed) debug_bp_forget(bp.pc, bp.bank, bp.x16Bank);
    }
    cJSON *body = cJSON_CreateObject();
    cJSON_AddBoolToObject(body, "removed", true);
    // Someone else's claim at the same address keeps it armed.
    cJSON_AddBoolToObject(body, "stillArmed", still_armed);
    send_dap_response(seq, cmd, true, body);
    return 0;
}

// ─── Symbols ─────────────────────────────────────────────────────────

static void dap_add_bank_field(cJSON *o, int bank) {
    if (bank >= 0) cJSON_AddNumberToObject(o, "bank", bank);
    else           cJSON_AddNullToObject(o, "bank");
}

extern uint8_t *fsroot_path;
extern bool     fsroot_given;

// A host file a client named, to be read. A DAP client is the user's editor and
// keeps naming what it likes. An MCP client reads only what the machine itself
// can see (-fsroot, or the working folder without one), the dump folder it can
// write to, and the folder of the program the emulator was started with.
static bool caller_read_path(const char *request, char *out, size_t out_size,
                             char *err, size_t errn) {
    if (current_caller == DBG_CALLER_DAP) {
        const int n = snprintf(out, out_size, "%s", request);
        if (n < 0 || (size_t)n >= out_size) {
            snprintf(err, errn, "that path is too long");
            return false;
        }
        return true;
    }

    const char *roots[3];
    int count = 0;
    roots[count++] = (fsroot_given && fsroot_path) ? (const char *)fsroot_path : ".";
    if (!fsroot_given) {
        const char *d = export_dir(EXPORT_DUMP);
        if (d[0]) roots[count++] = d;
    }
    char prg_folder[4200];
    const char *prg = dbg_load_prg_path();
    if (prg[0]) {
        snprintf(prg_folder, sizeof prg_folder, "%s", prg);
        char *cut = strrchr(prg_folder, '/');
#ifdef _WIN32
        char *back = strrchr(prg_folder, '\\');
        if (back && (!cut || back > cut)) cut = back;
#endif
        if (!cut)
            snprintf(prg_folder, sizeof prg_folder, ".");
        else if (cut == prg_folder || cut[-1] == ':')
            cut[1] = '\0';   // the root itself, which the rule then refuses
        else
            *cut = '\0';
        roots[count++] = prg_folder;
    }
    return host_read_path(roots, count, request, out, out_size, err, errn);
}

// The folder a client's written files stay inside: -fsroot if one was chosen,
// else the dump folder ("" is the working one). NULL, with `err`, if it cannot
// be made.
static const char *client_write_root(char *err, size_t errn) {
    if (fsroot_given && fsroot_path) return (const char *)fsroot_path;
    const char *d = export_dir(EXPORT_DUMP);
    if (d[0] && !export_make_dir(d)) {
        snprintf(err, errn, "the dump folder cannot be created");
        return NULL;
    }
    return d[0] ? d : ".";
}

// A host file a client named, to be written. A DAP client is the user's editor
// and keeps naming what it likes. An MCP client writes only inside the folder
// above, and replaces a file only when it says so.
static bool caller_write_path(const char *request, bool overwrite, char *out, size_t out_size,
                              char *err, size_t errn) {
    if (current_caller == DBG_CALLER_DAP) {
        const int n = snprintf(out, out_size, "%s", request);
        if (n < 0 || (size_t)n >= out_size) {
            snprintf(err, errn, "that path is too long");
            return false;
        }
        return true;
    }
    const char *folder = client_write_root(err, errn);
    if (!folder) return false;
    bool exists = false;
    if (!host_write_path(folder, request, out, out_size, &exists, err, errn)) return false;
    if (exists && !overwrite) {
        snprintf(err, errn, "that file exists; pass overwrite to replace it");
        return false;
    }
    return true;
}

// ─── read_source ────────────────────────────────────────────────────
//
// The name must be one the debug info holds, but that does not make it safe: a
// client can write a .dbg with dump_memory. So each place the Source window would
// look is judged by the read rule, and only one inside it is opened.

#define MCP_SOURCE_MAX (4u * 1024 * 1024)

static int handle_mcp_read_source(int seq, cJSON *args) {
    static const char cmd[] = "mcp/readSource";
    // The same words for missing and refused, so that this cannot be used to
    // learn what exists outside the folders a client may read.
    static const char unavailable[] = "that source file is not in the folders a client may read";

    cJSON *fi = cJSON_GetObjectItemCaseSensitive(args, "file");
    cJSON *si = cJSON_GetObjectItemCaseSensitive(args, "startLine");
    cJSON *ci = cJSON_GetObjectItemCaseSensitive(args, "lineCount");
    if (!fi || !cJSON_IsString(fi) || !fi->valuestring[0]) {
        send_dap_error_response(seq, cmd, "'file' is required");
        return 0;
    }
    const int start = cJSON_IsNumber(si) ? (int)si->valuedouble : 1;
    const int count = cJSON_IsNumber(ci) ? (int)ci->valuedouble : 200;

    const char *names[SOURCE_NAMES_MAX];
    const int matched = dbg_info_find_source(fi->valuestring, names, SOURCE_NAMES_MAX);
    if (!matched) {
        send_dap_error_response(seq, cmd, "that file is not in the loaded debug info");
        return 0;
    }
    if (matched > 1) {
        char why[256];
        source_ambiguous(why, sizeof why, names, matched);
        send_dap_error_response(seq, cmd, why);
        return 0;
    }

    char candidate[4096], full[4200], resolved[4200], why[200];
    bool readable = false;
    for (int i = 0; !readable && source_view_candidate(i, names[0], candidate, sizeof candidate); i++) {
        if (candidate[0] && host_path_from_cwd(candidate, full, sizeof full))
            readable = caller_read_path(full, resolved, sizeof resolved, why, sizeof why);
    }
    SDL_RWops *rw = readable ? SDL_RWFromFile(resolved, "rb") : NULL;
    if (!rw) {
        send_dap_error_response(seq, cmd, unavailable);
        return 0;
    }
    const Sint64 size = SDL_RWsize(rw);
    if (size < 0) {
        SDL_RWclose(rw);
        send_dap_error_response(seq, cmd, unavailable);
        return 0;
    }
    if ((uint64_t)size > MCP_SOURCE_MAX) {
        SDL_RWclose(rw);
        send_dap_error_response(seq, cmd, "that source file is larger than 4 MiB");
        return 0;
    }
    char *text = (char *)malloc((size_t)size + 1);
    if (!text) {
        SDL_RWclose(rw);
        send_dap_error_response(seq, cmd, "out of memory");
        return 0;
    }
    const size_t got = SDL_RWread(rw, text, 1, (size_t)size);
    SDL_RWclose(rw);
    text[got] = '\0';
    // A JSON string built from this stops at NUL. 0xFF is never valid UTF-8, so
    // the reply shows it as U+FFFD like any other byte that is not text.
    for (size_t i = 0; i < got; i++)
        if (!text[i]) text[i] = (char)0xFF;

    cJSON *lines = cJSON_CreateArray();
    int total = 0;
    for (char *p = text, *end = text + got; p < end; ) {
        char *nl = (char *)memchr(p, '\n', (size_t)(end - p));
        char *stop = nl ? nl : end;
        total++;
        if (total >= start && total - start < count) {
            size_t len = (size_t)(stop - p);
            if (len && p[len - 1] == '\r') len--;
            const char kept = p[len];
            p[len] = '\0';
            cJSON_AddItemToArray(lines, cJSON_CreateString(p));
            p[len] = kept;
        }
        p = nl ? nl + 1 : end;
    }
    free(text);

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "file", names[0]);
    cJSON_AddNumberToObject(body, "startLine", start);
    cJSON_AddNumberToObject(body, "totalLines", total);
    cJSON_AddItemToObject(body, "lines", lines);
    send_dap_response(seq, cmd, true, body);
    return 0;
}

static int handle_x16_load_symbols(int seq, cJSON *args) {
    static const char cmd[] = "x16/loadSymbols";
    cJSON *pi = args ? cJSON_GetObjectItemCaseSensitive(args, "path") : NULL;
    cJSON *bi = args ? cJSON_GetObjectItemCaseSensitive(args, "bank") : NULL;
    if (!pi || !cJSON_IsString(pi) || !pi->valuestring[0]) {
        send_dap_error_response(seq, cmd, "'path' is required: an ld65 -Ln label file");
        return 0;
    }
    int bank = -1;
    if (bi && !cJSON_IsNull(bi)) {
        if (!cJSON_IsNumber(bi) || bi->valuedouble < 0 || bi->valuedouble > 255 ||
            bi->valuedouble != (int)bi->valuedouble) {
            send_dap_error_response(seq, cmd, "'bank' must be a bank number, 0-255");
            return 0;
        }
        bank = (int)bi->valuedouble;
    }
    char source[4200], err[300];
    if (!caller_read_path(pi->valuestring, source, sizeof source, err, sizeof err)) {
        send_dap_error_response(seq, cmd, err);
        return 0;
    }
    const int n = dbg_info_load_labels(source, bank);
    if (n < 0) {
        send_dap_error_response(seq, cmd, "cannot read that file");
        return 0;
    }
    if (n == 0) {
        send_dap_error_response(seq, cmd, "no labels in that file (expected lines like: al 00C000 .name)");
        return 0;
    }
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "path", pi->valuestring);
    cJSON_AddNumberToObject(body, "labels", n);
    dap_add_bank_field(body, bank);
    cJSON_AddNumberToObject(body, "totalSymbols", dbg_info_symbol_count());
    send_dap_response(seq, cmd, true, body);
    return 0;
}

static int handle_x16_lookup_symbol(int seq, cJSON *args) {
    static const char cmd[] = "x16/lookupSymbol";
    cJSON *ni = args ? cJSON_GetObjectItemCaseSensitive(args, "name") : NULL;
    cJSON *ai = args ? cJSON_GetObjectItemCaseSensitive(args, "address") : NULL;
    if ((!ni) == (!ai) || (ni && !cJSON_IsString(ni))) {
        send_dap_error_response(seq, cmd, "give either 'name' or 'address'");
        return 0;
    }
    cJSON *body = cJSON_CreateObject();
    if (ni) {
        dbg_name_hit_t hits[16];
        const int n = dbg_info_find_name(ni->valuestring, hits, 16);
        cJSON *arr = cJSON_AddArrayToObject(body, "matches");
        for (int i = 0; i < n; i++) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "address", hits[i].addr);
            dap_add_bank_field(o, hits[i].bank);
            cJSON_AddStringToObject(o, "kind", hits[i].kind == DBG_NAME_EQUATE ? "equate" : "label");
            cJSON_AddItemToArray(arr, o);
        }
        if (n == 0) {
            // Not a plain name: it may still be an expression such as main+4.
            dbg_resolved_t r;
            char why[200];
            if (!dbg_info_resolve(ni->valuestring, &r, why, sizeof why)) {
                cJSON_Delete(body);
                send_dap_error_response(seq, cmd, why);
                return 0;
            }
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "address", r.addr);
            dap_add_bank_field(o, r.bank);
            cJSON_AddStringToObject(o, "kind", "expression");
            cJSON_AddItemToArray(arr, o);
        }
    } else {
        uint32_t addr = 0;
        bool is_vram = false;
        if (cJSON_IsNumber(ai) && ai->valuedouble >= 0 && ai->valuedouble <= 0xFFFFFF) {
            addr = (uint32_t)ai->valuedouble;
        } else if (!cJSON_IsString(ai) || !dap_parse_mem_ref(ai->valuestring, &is_vram, &addr) ||
                   is_vram || addr > 0xFFFFFF) {
            cJSON_Delete(body);
            send_dap_error_response(seq, cmd, "'address' must be a CPU address");
            return 0;
        }
        const char *name = NULL;
        dbg_addr_t at = 0;
        cJSON_AddNumberToObject(body, "address", addr);
        if (dbg_info_addr_to_label_nearest(addr, &name, &at) && name) {
            cJSON_AddStringToObject(body, "name", name);
            cJSON_AddNumberToObject(body, "labelAddress", at);
            cJSON_AddNumberToObject(body, "offset", addr - at);
        } else {
            cJSON_AddNullToObject(body, "name");
        }
    }
    send_dap_response(seq, cmd, true, body);
    return 0;
}

static bool dap_contains_i(const char *hay, const char *needle) {
    const size_t n = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return true;
    }
    return n == 0;
}

static int handle_x16_list_symbols(int seq, cJSON *args) {
    static const char cmd[] = "x16/listSymbols";
    cJSON *fi = args ? cJSON_GetObjectItemCaseSensitive(args, "filter") : NULL;
    cJSON *si = args ? cJSON_GetObjectItemCaseSensitive(args, "start") : NULL;
    cJSON *ci = args ? cJSON_GetObjectItemCaseSensitive(args, "count") : NULL;
    if ((fi && !cJSON_IsString(fi)) || (si && !cJSON_IsNumber(si)) || (ci && !cJSON_IsNumber(ci))) {
        send_dap_error_response(seq, cmd, "'filter' is a string; 'start' and 'count' are numbers");
        return 0;
    }
    const char *filter = fi ? fi->valuestring : "";
    const int start = si && si->valuedouble > 0 ? (int)si->valuedouble : 0;
    int count = ci ? (int)ci->valuedouble : 100;
    if (count < 1) count = 1;
    if (count > 1000) count = 1000;

    cJSON *body = cJSON_CreateObject();
    cJSON *arr  = cJSON_AddArrayToObject(body, "symbols");
    int total = 0;
    const int n = dbg_info_symbol_count();
    for (int i = 0; i < n; i++) {
        const char *name = NULL;
        dbg_addr_t  addr = 0;
        if (!dbg_info_symbol_at(i, &name, &addr) || !name || !dap_contains_i(name, filter)) continue;
        if (total >= start && total < start + count) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "name", name);
            cJSON_AddNumberToObject(o, "address", addr);
            dap_add_bank_field(o, dbg_info_symbol_bank_at(i));
            cJSON_AddItemToArray(arr, o);
        }
        total++;
    }
    cJSON_AddNumberToObject(body, "total", total);
    cJSON_AddNumberToObject(body, "start", start);
    send_dap_response(seq, cmd, true, body);
    return 0;
}

// ─── Ranges of memory: x16/hashMemory, x16/dumpMemory ────────────────

#define DAP_RANGE_MAX 0x100000

// The whole range or an error: a hash of fewer bytes than were asked for would
// read as an answer about the range. Through the debugger's read path, because
// these must not fire read watches or move VERA's data port.
static uint8_t *dap_read_range(int seq, const char *cmd, cJSON *args,
                               uint32_t *addr_out, bool *vram_out, size_t *len_out) {
    cJSON *ri = args ? cJSON_GetObjectItemCaseSensitive(args, "memoryReference") : NULL;
    cJSON *ci = args ? cJSON_GetObjectItemCaseSensitive(args, "count") : NULL;
    cJSON *bi = args ? cJSON_GetObjectItemCaseSensitive(args, "x16Bank") : NULL;
    uint32_t addr = 0;
    bool is_vram = false;
    if (!ri || !cJSON_IsString(ri) || !dap_parse_mem_ref(ri->valuestring, &is_vram, &addr)) {
        send_dap_error_response(seq, cmd, "'memoryReference' must be a CPU address or vram:<hex>");
        return NULL;
    }
    if (!ci || !cJSON_IsNumber(ci) || ci->valuedouble < 1 || ci->valuedouble > DAP_RANGE_MAX ||
        ci->valuedouble != (double)(int)ci->valuedouble) {
        send_dap_error_response(seq, cmd, "'count' must be 1 to 1048576");
        return NULL;
    }
    const size_t   n     = (size_t)ci->valuedouble;
    const uint32_t space = is_vram ? 0x20000u : 0x1000000u;
    if (addr >= space || n > (size_t)(space - addr)) {
        send_dap_error_response(seq, cmd, "that range runs past the end of the address space");
        return NULL;
    }
    int x16_bank = USE_CURRENT_X16_BANK;
    if (bi) {
        if (is_vram || !cJSON_IsNumber(bi) || bi->valuedouble < 0 || bi->valuedouble > 255) {
            send_dap_error_response(seq, cmd, "'x16Bank' must be 0-255, and VRAM has no banks");
            return NULL;
        }
        x16_bank = (int)bi->valuedouble;
    }
    uint8_t *raw = (uint8_t *)malloc(n);
    if (!raw) {
        send_dap_error_response(seq, cmd, "out of memory");
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        const uint32_t cur = addr + (uint32_t)i;
        raw[i] = is_vram ? video_space_read(cur)
                         : debug_read6502((uint16_t)(cur & 0xFFFF), (uint8_t)(cur >> 16), x16_bank);
    }
    *addr_out = addr;
    *vram_out = is_vram;
    *len_out  = n;
    return raw;
}

static void dap_add_range_fields(cJSON *body, uint32_t addr, bool is_vram, size_t n) {
    char s[16];
    if (is_vram)                     snprintf(s, sizeof s, "0x%05X", (unsigned)addr);
    else if (is_gen2 || addr > 0xFFFF) snprintf(s, sizeof s, "0x%06X", (unsigned)addr);
    else                             snprintf(s, sizeof s, "0x%04X", (unsigned)addr);
    cJSON_AddStringToObject(body, "address", s);
    cJSON_AddStringToObject(body, "space", is_vram ? "vram" : "cpu");
    cJSON_AddNumberToObject(body, "length", (double)n);
}

static int handle_x16_hash_memory(int seq, cJSON *args) {
    static const char cmd[] = "x16/hashMemory";
    uint32_t addr;
    bool is_vram;
    size_t n;
    uint8_t *raw = dap_read_range(seq, cmd, args, &addr, &is_vram, &n);
    if (!raw) return 0;

    const uint32_t crc = mem_crc32(raw, n);
    uint8_t sha[32];
    mem_sha256(raw, n, sha);
    free(raw);

    char crc_hex[9], sha_hex[65];
    snprintf(crc_hex, sizeof crc_hex, "%08x", (unsigned)crc);
    for (int i = 0; i < 32; i++) snprintf(sha_hex + i * 2, 3, "%02x", sha[i]);

    cJSON *body = cJSON_CreateObject();
    dap_add_range_fields(body, addr, is_vram, n);
    cJSON_AddStringToObject(body, "crc32", crc_hex);
    cJSON_AddStringToObject(body, "sha256", sha_hex);
    send_dap_response(seq, cmd, true, body);
    return 0;
}

static int handle_x16_dump_memory(int seq, cJSON *args) {
    static const char cmd[] = "x16/dumpMemory";
    cJSON *pi = args ? cJSON_GetObjectItemCaseSensitive(args, "path") : NULL;
    cJSON *oi = args ? cJSON_GetObjectItemCaseSensitive(args, "overwrite") : NULL;
    if (!pi || !cJSON_IsString(pi) || !pi->valuestring[0]) {
        send_dap_error_response(seq, cmd, "'path' is required");
        return 0;
    }
    if (oi && !cJSON_IsBool(oi)) {
        send_dap_error_response(seq, cmd, "'overwrite' must be true or false");
        return 0;
    }

    char target[4200], err[300];
    const char *folder = client_write_root(err, sizeof err);
    if (!folder) {
        send_dap_error_response(seq, cmd, err);
        return 0;
    }
    bool exists = false;
    if (!host_write_path(folder, pi->valuestring, target, sizeof target, &exists, err, sizeof err)) {
        send_dap_error_response(seq, cmd, err);
        return 0;
    }
    if (exists && !cJSON_IsTrue(oi)) {
        send_dap_error_response(seq, cmd, "that file exists; pass overwrite to replace it");
        return 0;
    }

    uint32_t addr;
    bool is_vram;
    size_t n;
    uint8_t *raw = dap_read_range(seq, cmd, args, &addr, &is_vram, &n);
    if (!raw) return 0;

    SDL_RWops *f = SDL_RWFromFile(target, "wb");
    if (!f) {
        free(raw);
        send_dap_error_response(seq, cmd, "cannot open that file for writing");
        return 0;
    }
    const size_t wrote = SDL_RWwrite(f, raw, 1, n);
    const int closed = SDL_RWclose(f);
    free(raw);
    if (wrote != n || closed != 0) {
        send_dap_error_response(seq, cmd, "the file could not be written in full");
        return 0;
    }

    cJSON *body = cJSON_CreateObject();
    dap_add_range_fields(body, addr, is_vram, n);
    cJSON_AddStringToObject(body, "path", target);
    send_dap_response(seq, cmd, true, body);
    return 0;
}

// ─── x16/searchMemory ────────────────────────────────────────────────
//
// Offsets are flat: an address in the CPU map or VRAM, an offset into the window
// for one bank, and bank * window + offset across all banks. So a match in the
// CPU map can straddle $BFFF/$C000 as the CPU sees it, and one across all banks
// can run from a bank's end into the next, as a load that spans banks does. One
// bank stops at its window. Nothing wraps.

#define SEARCH_LIMIT_DEFAULT 64
#define SEARCH_LIMIT_MAX     1024

typedef struct {
    bool     vram;
    uint16_t base;  // CPU address of offset 0
    int      bank;  // the X16 bank read, unless `win` is set
    uint32_t win;   // all banks: the size of one bank's window
} search_view_t;

typedef struct {
    uint8_t       pat[MEM_SEARCH_MAX_PATTERN];
    size_t        m;
    const char   *space;
    search_view_t view;
    uint32_t      from, size;
    int           limit;
} search_request_t;

// Debug reads only: the I/O page must not see a device read.
static uint8_t search_read(uint32_t offset, void *ctx) {
    const search_view_t *v = (const search_view_t *)ctx;
    if (v->vram) return video_space_read(offset);
    if (v->win)
        return debug_read6502((uint16_t)(v->base + offset % v->win), 0, (int16_t)(offset / v->win));
    return debug_read6502((uint16_t)(v->base + offset), 0, (int16_t)v->bank);
}

// A number, or a string spelled "$C000" or "0xC000".
static bool search_arg_address(cJSON *v, uint32_t *out) {
    if (cJSON_IsNumber(v)) {
        if (v->valuedouble < 0 || v->valuedouble > 0xFFFFFF ||
            v->valuedouble != (double)(int)v->valuedouble)
            return false;
        *out = (uint32_t)v->valuedouble;
        return true;
    }
    if (!cJSON_IsString(v)) return false;
    const char *s = v->valuestring;
    if (s[0] == '$') s += 1;
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    else return false;
    if (!isxdigit((unsigned char)*s)) return false;
    char *end = NULL;
    const unsigned long n = strtoul(s, &end, 16);
    if (*end || n > 0xFFFFFF) return false;
    *out = (uint32_t)n;
    return true;
}

static bool search_arg_int(cJSON *v, int lo, int hi, int *out) {
    if (!cJSON_IsNumber(v) || v->valuedouble < lo || v->valuedouble > hi ||
        v->valuedouble != (double)(int)v->valuedouble)
        return false;
    *out = (int)v->valuedouble;
    return true;
}

static bool search_arg_pattern(cJSON *args, search_request_t *q, char *err, size_t errn) {
    cJSON *hex  = args ? cJSON_GetObjectItemCaseSensitive(args, "hex") : NULL;
    cJSON *text = args ? cJSON_GetObjectItemCaseSensitive(args, "text") : NULL;
    if (!hex == !text) {
        snprintf(err, errn, "give exactly one of 'hex' or 'text'");
        return false;
    }
    if (hex) {
        if (!cJSON_IsString(hex) || !mem_search_parse_hex(hex->valuestring, true, q->pat, &q->m)) {
            snprintf(err, errn, "'hex' must be 1 to %d bytes as pairs of hex digits, e.g. \"A9 00\"",
                     MEM_SEARCH_MAX_PATTERN);
            return false;
        }
        return true;
    }
    if (!cJSON_IsString(text) || !mem_search_parse_text(text->valuestring, q->pat, &q->m)) {
        snprintf(err, errn, "'text' must be 1 to %d characters", MEM_SEARCH_MAX_PATTERN);
        return false;
    }
    for (size_t i = 0; i < q->m; i++) {
        if (q->pat[i] >= 0x80) {
            snprintf(err, errn, "'text' must be ASCII: give other bytes as 'hex'");
            return false;
        }
    }
    return true;
}

static bool search_arg_where(cJSON *args, search_request_t *q, char *err, size_t errn) {
    cJSON *sp  = args ? cJSON_GetObjectItemCaseSensitive(args, "space") : NULL;
    cJSON *bi  = args ? cJSON_GetObjectItemCaseSensitive(args, "bank") : NULL;
    cJSON *sbi = args ? cJSON_GetObjectItemCaseSensitive(args, "startBank") : NULL;
    cJSON *si  = args ? cJSON_GetObjectItemCaseSensitive(args, "start") : NULL;
    cJSON *ei  = args ? cJSON_GetObjectItemCaseSensitive(args, "end") : NULL;

    q->space = "cpu";
    if (sp) {
        if (!cJSON_IsString(sp) ||
            (strcmp(sp->valuestring, "cpu") && strcmp(sp->valuestring, "ram") &&
             strcmp(sp->valuestring, "rom") && strcmp(sp->valuestring, "vram"))) {
            snprintf(err, errn, "'space' must be \"cpu\", \"ram\", \"rom\" or \"vram\"");
            return false;
        }
        q->space = sp->valuestring;
    }
    const bool is_ram = !strcmp(q->space, "ram"), is_rom = !strcmp(q->space, "rom");
    const bool banked = is_ram || is_rom;
    const int  nbanks = is_ram ? (int)num_ram_banks : NUM_ROM_BANKS;
    uint32_t   lo = 0, hi = 0xFFFF;
    if (is_ram) lo = 0xA000, hi = 0xBFFF;
    if (is_rom) lo = 0xC000, hi = 0xFFFF;
    if (!strcmp(q->space, "vram")) hi = 0x1FFFF;

    memset(&q->view, 0, sizeof q->view);
    q->view.vram = !strcmp(q->space, "vram");
    q->view.base = (uint16_t)lo;
    q->view.bank = USE_CURRENT_X16_BANK;

    int bank = -1;
    if (bi) {
        if (!banked) {
            snprintf(err, errn, "'bank' applies to space \"ram\" or \"rom\"");
            return false;
        }
        if (!search_arg_int(bi, 0, nbanks - 1, &bank)) {
            snprintf(err, errn, "that %s bank does not exist: this machine has banks 0 to %d",
                     is_ram ? "RAM" : "ROM", nbanks - 1);
            return false;
        }
        q->view.bank = bank;
    }
    const bool all_banks = banked && bank < 0;
    int start_bank = 0;
    if (sbi) {
        if (!all_banks) {
            snprintf(err, errn, "'startBank' applies only when searching all banks");
            return false;
        }
        if (!search_arg_int(sbi, 0, nbanks - 1, &start_bank)) {
            snprintf(err, errn, "that %s bank does not exist: this machine has banks 0 to %d",
                     is_ram ? "RAM" : "ROM", nbanks - 1);
            return false;
        }
    }
    if (all_banks && ei) {
        snprintf(err, errn, "'end' needs a single 'bank': all banks always run to the last one");
        return false;
    }

    uint32_t start = lo, end = hi;
    if (si && !search_arg_address(si, &start)) {
        snprintf(err, errn, "'start' must be a number or a string like \"$C000\" or \"0xC000\"");
        return false;
    }
    if (ei && !search_arg_address(ei, &end)) {
        snprintf(err, errn, "'end' must be a number or a string like \"$C000\" or \"0xC000\"");
        return false;
    }
    if (start < lo || start > hi || end < lo || end > hi) {
        snprintf(err, errn, "that range is outside %s, which is $%04X-$%04X", q->space,
                 (unsigned)lo, (unsigned)hi);
        return false;
    }
    if (start > end) {
        snprintf(err, errn, "'start' is past 'end'");
        return false;
    }

    if (all_banks) {
        q->view.win = hi - lo + 1;
        q->from     = (uint32_t)start_bank * q->view.win + (start - lo);
        q->size     = (uint32_t)nbanks * q->view.win;
    } else {
        q->from = start - lo;
        q->size = end - lo + 1;
    }
    return true;
}

static void search_format_address(char *s, size_t n, uint32_t addr, bool vram) {
    if (vram) snprintf(s, n, "0x%05X", (unsigned)addr);
    else      snprintf(s, n, "0x%04X", (unsigned)addr);
}

// Which of the CPU map's differently banked parts an address is in.
static int search_cpu_region(uint32_t addr) {
    return addr < 0xA000 ? 0 : addr < 0xC000 ? 1 : 2;
}

static cJSON *search_match(const search_request_t *q, uint32_t offset) {
    const search_view_t *v    = &q->view;
    const uint32_t       last = offset + (uint32_t)q->m - 1;
    cJSON *o = cJSON_CreateObject();
    char   s[16];
    if (v->vram) {
        search_format_address(s, sizeof s, offset, true);
        cJSON_AddStringToObject(o, "address", s);
        cJSON_AddNullToObject(o, "bank");
        cJSON_AddBoolToObject(o, "crossesBank", false);
    } else if (v->win) {
        search_format_address(s, sizeof s, v->base + offset % v->win, false);
        cJSON_AddStringToObject(o, "address", s);
        cJSON_AddNumberToObject(o, "bank", offset / v->win);
        cJSON_AddBoolToObject(o, "crossesBank", last / v->win != offset / v->win);
    } else if (v->bank >= 0) {
        search_format_address(s, sizeof s, v->base + offset, false);
        cJSON_AddStringToObject(o, "address", s);
        cJSON_AddNumberToObject(o, "bank", v->bank);
        cJSON_AddBoolToObject(o, "crossesBank", false);
    } else {
        search_format_address(s, sizeof s, offset, false);
        cJSON_AddStringToObject(o, "address", s);
        const int mapped = debug_current_x16_bank((int)offset, 0);
        if (mapped >= 0) cJSON_AddNumberToObject(o, "bank", mapped);
        else             cJSON_AddNullToObject(o, "bank");
        cJSON_AddBoolToObject(o, "crossesBank", search_cpu_region(offset) != search_cpu_region(last));
    }
    return o;
}

static int handle_x16_search_memory(int seq, cJSON *args) {
    static const char cmd[] = "x16/searchMemory";
    search_request_t q;
    char err[160];
    if (!search_arg_pattern(args, &q, err, sizeof err) ||
        !search_arg_where(args, &q, err, sizeof err)) {
        send_dap_error_response(seq, cmd, err);
        return 0;
    }
    cJSON *li = args ? cJSON_GetObjectItemCaseSensitive(args, "limit") : NULL;
    q.limit = SEARCH_LIMIT_DEFAULT;
    if (li && !search_arg_int(li, 1, SEARCH_LIMIT_MAX, &q.limit)) {
        send_dap_error_response(seq, cmd, "'limit' must be 1 to 1024");
        return 0;
    }

    uint32_t found[SEARCH_LIMIT_MAX];
    size_t   n = 0;
    uint32_t next = 0;
    const mem_search_stop_t stop =
        mem_search_scan(search_read, &q.view, q.size, q.pat, q.m, q.from, MEM_SEARCH_BUDGET,
                        found, (size_t)q.limit, &n, &next);

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "space", q.space);
    cJSON_AddNumberToObject(body, "length", (double)q.m);
    cJSON *list = cJSON_CreateArray();
    for (size_t i = 0; i < n; i++) cJSON_AddItemToArray(list, search_match(&q, found[i]));
    cJSON_AddItemToObject(body, "matches", list);
    cJSON_AddNumberToObject(body, "count", (double)n);
    cJSON_AddBoolToObject(body, "more", stop != MEM_SEARCH_DONE);
    if (stop != MEM_SEARCH_DONE) {
        cJSON_AddStringToObject(body, "stoppedBy", stop == MEM_SEARCH_LIMIT ? "limit" : "budget");
        cJSON *where = cJSON_CreateObject();
        char   s[16];
        if (q.view.win) {
            search_format_address(s, sizeof s, q.view.base + next % q.view.win, false);
            cJSON_AddStringToObject(where, "start", s);
            cJSON_AddNumberToObject(where, "startBank", next / q.view.win);
        } else {
            search_format_address(s, sizeof s, q.view.base + next, q.view.vram);
            cJSON_AddStringToObject(where, "start", s);
        }
        cJSON_AddItemToObject(body, "next", where);
    }
    send_dap_response(seq, cmd, true, body);
    return 0;
}

// ─── x16/audio, x16/setVoiceMute ─────────────────────────────────────

typedef struct {
    unsigned ui, dap, mcp, effective;
} voice_mute_view_t;

static voice_mute_view_t psg_mute_view(void) {
    return (voice_mute_view_t){ psg_debug_get_mute_mask(),
                                psg_debug_get_owner_mute_mask(VOICE_MUTE_DAP),
                                psg_debug_get_owner_mute_mask(VOICE_MUTE_MCP),
                                psg_debug_get_effective_mute_mask() };
}

static voice_mute_view_t ym_mute_view(void) {
    return (voice_mute_view_t){ YM_debug_get_mute_mask(),
                                YM_debug_get_owner_mute_mask(VOICE_MUTE_DAP),
                                YM_debug_get_owner_mute_mask(VOICE_MUTE_MCP),
                                YM_debug_get_effective_mute_mask() };
}

static void audio_add_mute_masks(cJSON *o, voice_mute_view_t m) {
    cJSON_AddNumberToObject(o, "ui", m.ui);
    cJSON_AddNumberToObject(o, "dap", m.dap);
    cJSON_AddNumberToObject(o, "mcp", m.mcp);
    cJSON_AddNumberToObject(o, "effective", m.effective);
}

static void audio_add_voice_mute(cJSON *o, voice_mute_view_t m, int voice) {
    const unsigned bit = 1u << voice;
    cJSON_AddBoolToObject(o, "muted", (m.effective & bit) != 0);
    cJSON *by = cJSON_CreateArray();
    if (m.ui & bit)  cJSON_AddItemToArray(by, cJSON_CreateString("ui"));
    if (m.dap & bit) cJSON_AddItemToArray(by, cJSON_CreateString("dap"));
    if (m.mcp & bit) cJSON_AddItemToArray(by, cJSON_CreateString("mcp"));
    cJSON_AddItemToObject(o, "mutedBy", by);
}

static cJSON *audio_bytes(const uint8_t *v, int n) {
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < n; i++) cJSON_AddItemToArray(a, cJSON_CreateNumber(v[i]));
    return a;
}

static cJSON *audio_psg_state(void) {
    const voice_mute_view_t mute = psg_mute_view();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "sampleHz", AUDIO_VERA_SAMPLE_HZ);
    cJSON_AddNumberToObject(o, "noiseLfsr", psg_debug_get_noise_state());
    cJSON *list = cJSON_AddArrayToObject(o, "voices");
    for (int ch = 0; ch < PSG_NUM_CHANNELS; ch++) {
        struct psg_debug_channel c;
        if (!psg_debug_peek_channel(ch, &c)) continue;
        uint8_t regs[4];
        for (int i = 0; i < 4; i++) regs[i] = psg_debug_get_reg((uint8_t)(ch * 4 + i));
        const double hz = audio_psg_hz(c.freq);
        char note[32];
        audio_note_from_hz(hz, note, sizeof note);

        cJSON *v = cJSON_CreateObject();
        cJSON_AddNumberToObject(v, "ch", ch);
        cJSON_AddItemToObject(v, "regs", audio_bytes(regs, 4));
        cJSON_AddStringToObject(v, "wave", audio_psg_wave_name(c.waveform));
        cJSON_AddNumberToObject(v, "freq", c.freq);
        cJSON_AddNumberToObject(v, "hz", hz);
        cJSON_AddStringToObject(v, "note", note);
        cJSON_AddNumberToObject(v, "pw", c.pw);
        if ((c.waveform & 3) == PSG_WF_PULSE)
            cJSON_AddNumberToObject(v, "dutyPercent", (double)(c.pw + 1) * 100.0 / 128.0);
        else if ((c.waveform & 3) != PSG_WF_NOISE)
            cJSON_AddNumberToObject(v, "shapeMask", (c.pw ^ 0x3F) & 0x3F);
        cJSON_AddNumberToObject(v, "vol", c.vol_raw);
        cJSON_AddNumberToObject(v, "volLut", c.vol_lut);
        cJSON_AddBoolToObject(v, "left", c.left);
        cJSON_AddBoolToObject(v, "right", c.right);
        cJSON_AddNumberToObject(v, "phase", c.phase & 0x1FFFFu);
        cJSON_AddNumberToObject(v, "noiseValue", c.noiseval);
        cJSON_AddNumberToObject(v, "lastSample", c.last_sample);
        audio_add_voice_mute(v, mute, ch);
        cJSON_AddItemToArray(list, v);
    }
    return o;
}

static cJSON *audio_ym_operator(int ch, int slot, uint8_t keyon) {
    static const char *const slots[4] = { "M1", "M2", "C1", "C2" };
    static const char *const eg[5] = { "-", "Attack", "Decay", "Sustain", "Release" };
    uint8_t r[6];
    for (int i = 0; i < 6; i++) r[i] = YM_debug_get_reg((uint8_t)(0x40 + i * 0x20 + slot * 8 + ch));

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "slot", slots[slot]);
    cJSON_AddItemToObject(o, "regs", audio_bytes(r, 6));
    cJSON_AddNumberToObject(o, "DT1", (r[0] >> 4) & 7);
    cJSON_AddNumberToObject(o, "MUL", r[0] & 0x0F);
    cJSON_AddNumberToObject(o, "TL", r[1] & 0x7F);
    cJSON_AddNumberToObject(o, "KS", r[2] >> 6);
    cJSON_AddNumberToObject(o, "AR", r[2] & 0x1F);
    cJSON_AddNumberToObject(o, "AMS-EN", r[3] >> 7);
    cJSON_AddNumberToObject(o, "D1R", r[3] & 0x1F);
    cJSON_AddNumberToObject(o, "DT2", r[4] >> 6);
    cJSON_AddNumberToObject(o, "D2R", r[4] & 0x1F);
    cJSON_AddNumberToObject(o, "D1L", r[5] >> 4);
    cJSON_AddNumberToObject(o, "RR", r[5] & 0x0F);
    cJSON_AddBoolToObject(o, "keyOn", (keyon >> slot) & 1);
    struct ym_debug_operator op;
    if (YM_debug_get_operator(slot * 8 + ch, &op)) {
        cJSON_AddStringToObject(o, "eg", op.eg_state <= YM_EG_RELEASE ? eg[op.eg_state] : "-");
        cJSON_AddNumberToObject(o, "egAttenuation", op.eg_attenuation);
    } else {
        cJSON_AddNullToObject(o, "eg");
        cJSON_AddNullToObject(o, "egAttenuation");
    }
    return o;
}

static cJSON *audio_ym_state(void) {
    static const char *const lfo_waves[4] = { "saw", "square", "triangle", "noise" };
    const voice_mute_view_t mute = ym_mute_view();
    const uint8_t status = YM_debug_get_status();
    const uint8_t r0f = YM_debug_get_reg(0x0F), r14 = YM_debug_get_reg(0x14);
    const uint8_t r1b = YM_debug_get_reg(0x1B);
    const uint16_t ta = (uint16_t)((YM_debug_get_reg(0x10) << 2) | (YM_debug_get_reg(0x11) & 3));
    const uint8_t tb = YM_debug_get_reg(0x12);
    uint8_t am = 0, pm = 0;
    YM_debug_get_lfo_depths(&am, &pm);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "clockHz", AUDIO_YM_CLOCK_HZ);
    cJSON_AddNumberToObject(o, "sampleHz", AUDIO_YM_SAMPLE_HZ);
    cJSON_AddNumberToObject(o, "status", status);
    cJSON_AddBoolToObject(o, "busy", YM_debug_is_busy());
    cJSON_AddBoolToObject(o, "irq", YM_irq());
    cJSON *lfo = cJSON_AddObjectToObject(o, "lfo");
    cJSON_AddNumberToObject(lfo, "rate", YM_debug_get_reg(0x18));
    cJSON_AddStringToObject(lfo, "waveform", lfo_waves[r1b & 3]);
    cJSON_AddNumberToObject(lfo, "amDepth", am);
    cJSON_AddNumberToObject(lfo, "pmDepth", pm);
    cJSON *noise = cJSON_AddObjectToObject(o, "noise");
    cJSON_AddBoolToObject(noise, "enabled", (r0f & 0x80) != 0);
    cJSON_AddNumberToObject(noise, "frequency", r0f & 0x1F);
    cJSON *ct = cJSON_AddObjectToObject(o, "ct");
    cJSON_AddNumberToObject(ct, "CT1", (r1b >> 6) & 1);
    cJSON_AddNumberToObject(ct, "CT2", (r1b >> 7) & 1);
    cJSON_AddNumberToObject(o, "timerControl", r14);
    cJSON_AddBoolToObject(o, "csm", (r14 & 0x80) != 0);
    cJSON *a = cJSON_AddObjectToObject(o, "timerA");
    cJSON_AddNumberToObject(a, "value", ta);
    cJSON_AddNumberToObject(a, "ms", audio_ym_timer_a_ms(ta));
    cJSON_AddBoolToObject(a, "load", (r14 & 0x01) != 0);
    cJSON_AddBoolToObject(a, "enable", (r14 & 0x04) != 0);
    cJSON_AddBoolToObject(a, "overflow", (status & 0x01) != 0);
    cJSON *b = cJSON_AddObjectToObject(o, "timerB");
    cJSON_AddNumberToObject(b, "value", tb);
    cJSON_AddNumberToObject(b, "ms", audio_ym_timer_b_ms(tb));
    cJSON_AddBoolToObject(b, "load", (r14 & 0x02) != 0);
    cJSON_AddBoolToObject(b, "enable", (r14 & 0x08) != 0);
    cJSON_AddBoolToObject(b, "overflow", (status & 0x02) != 0);

    cJSON *list = cJSON_AddArrayToObject(o, "channels");
    for (int ch = 0; ch < YM_NUM_CHANNELS; ch++) {
        const uint8_t regs[4] = { YM_debug_get_reg((uint8_t)(0x20 + ch)), YM_debug_get_reg((uint8_t)(0x28 + ch)),
                                  YM_debug_get_reg((uint8_t)(0x30 + ch)), YM_debug_get_reg((uint8_t)(0x38 + ch)) };
        const uint8_t kc = regs[1] & 0x7F, kf = regs[2] & 0xFC, keyon = YM_debug_get_keyon(ch);
        struct audio_ym_note note;
        audio_ym_note_decode(kc, kf, &note);
        char pan[4] = { (regs[0] & 0x40) ? 'L' : '-', '/', (regs[0] & 0x80) ? 'R' : '-', 0 };

        cJSON *c = cJSON_CreateObject();
        cJSON_AddNumberToObject(c, "ch", ch);
        cJSON_AddItemToObject(c, "regs", audio_bytes(regs, 4));
        cJSON_AddNumberToObject(c, "KC", kc);
        cJSON_AddNumberToObject(c, "KF", kf >> 2);
        cJSON_AddStringToObject(c, "note", note.text);
        cJSON_AddBoolToObject(c, "noteValid", note.valid);
        cJSON_AddNumberToObject(c, "hz", note.hz);
        cJSON_AddNumberToObject(c, "ALG", regs[0] & 7);
        cJSON_AddNumberToObject(c, "FB", (regs[0] >> 3) & 7);
        cJSON_AddStringToObject(c, "pan", pan);
        cJSON_AddNumberToObject(c, "PMS", (regs[3] >> 4) & 7);
        cJSON_AddNumberToObject(c, "AMS", regs[3] & 3);
        cJSON_AddNumberToObject(c, "keyOnMask", keyon);
        audio_add_voice_mute(c, mute, ch);
        cJSON *ops = cJSON_AddArrayToObject(c, "operators");
        for (int slot = 0; slot < 4; slot++) cJSON_AddItemToArray(ops, audio_ym_operator(ch, slot, keyon));
        cJSON_AddItemToArray(list, c);
    }
    return o;
}

static cJSON *audio_pcm_state(void) {
    struct pcm_debug_state s;
    pcm_debug_get_state(&s);
    const struct audio_pcm_format fmt = audio_pcm_format_decode(s.ctrl);
    const double rate_hz = audio_pcm_rate_hz(s.rate);
    const unsigned frames = s.fifo_cnt / (unsigned)fmt.bytes_per_frame;

    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "ctrl", s.ctrl);
    cJSON_AddNumberToObject(o, "rate", s.rate);
    cJSON_AddStringToObject(o, "format", fmt.name);
    cJSON_AddNumberToObject(o, "bytesPerFrame", fmt.bytes_per_frame);
    cJSON_AddNumberToObject(o, "volume", s.ctrl & 0x0F);
    cJSON_AddNumberToObject(o, "volumeLut", audio_pcm_volume_lut(s.ctrl));
    cJSON_AddNumberToObject(o, "rateStep", audio_pcm_rate_step(s.rate));
    cJSON_AddNumberToObject(o, "rateHz", rate_hz);
    cJSON_AddNumberToObject(o, "fifoQueued", s.fifo_cnt);
    cJSON_AddNumberToObject(o, "fifoSize", s.fifo_size);
    cJSON_AddNumberToObject(o, "fifoUsable", s.fifo_size ? s.fifo_size - 1 : 0);
    cJSON_AddNumberToObject(o, "framesQueued", frames);
    cJSON_AddNumberToObject(o, "bufferedMs", rate_hz > 0.0 ? (double)frames * 1000.0 / rate_hz : 0.0);
    cJSON_AddNumberToObject(o, "readIndex", s.fifo_rdidx);
    cJSON_AddNumberToObject(o, "writeIndex", s.fifo_wridx);
    cJSON_AddBoolToObject(o, "almostEmpty", s.almost_empty);
    cJSON_AddNumberToObject(o, "curL", s.cur_l);
    cJSON_AddNumberToObject(o, "curR", s.cur_r);
    cJSON_AddNumberToObject(o, "phase", s.phase);
    return o;
}

static int handle_x16_audio(int seq, cJSON *args) {
    static const char cmd[] = "x16/audio";
    cJSON *c = args ? cJSON_GetObjectItemCaseSensitive(args, "chip") : NULL;
    const char *only = NULL;
    if (c) {
        if (!cJSON_IsString(c) || (strcmp(c->valuestring, "psg") && strcmp(c->valuestring, "ym") &&
                                   strcmp(c->valuestring, "pcm"))) {
            send_dap_error_response(seq, cmd, "'chip' must be psg, ym or pcm");
            return 0;
        }
        only = c->valuestring;
    }

    cJSON *body = cJSON_CreateObject();
    cJSON_AddBoolToObject(body, "audioRunning", audio_is_running());
    if (!only || !strcmp(only, "psg")) cJSON_AddItemToObject(body, "psg", audio_psg_state());
    if (!only || !strcmp(only, "ym"))  cJSON_AddItemToObject(body, "ym", audio_ym_state());
    if (!only || !strcmp(only, "pcm")) cJSON_AddItemToObject(body, "pcm", audio_pcm_state());
    cJSON *mute = cJSON_AddObjectToObject(body, "mute");
    audio_add_mute_masks(cJSON_AddObjectToObject(mute, "psg"), psg_mute_view());
    audio_add_mute_masks(cJSON_AddObjectToObject(mute, "ym"), ym_mute_view());
    send_dap_response(seq, cmd, true, body);
    return 0;
}

// An explicit state rather than a toggle, so that a retry cannot undo it. Only
// the caller's own mask changes.
static int handle_x16_set_voice_mute(int seq, cJSON *args) {
    static const char cmd[] = "x16/setVoiceMute";
    cJSON *c = args ? cJSON_GetObjectItemCaseSensitive(args, "chip") : NULL;
    cJSON *v = args ? cJSON_GetObjectItemCaseSensitive(args, "voice") : NULL;
    cJSON *m = args ? cJSON_GetObjectItemCaseSensitive(args, "muted") : NULL;
    if (!c || !cJSON_IsString(c) || (strcmp(c->valuestring, "psg") && strcmp(c->valuestring, "ym"))) {
        send_dap_error_response(seq, cmd, "'chip' is required: psg or ym");
        return 0;
    }
    if (!m || !cJSON_IsBool(m)) {
        send_dap_error_response(seq, cmd, "'muted' is required: true or false");
        return 0;
    }
    const bool is_psg = !strcmp(c->valuestring, "psg");
    const int voices = is_psg ? PSG_NUM_CHANNELS : YM_NUM_CHANNELS;
    unsigned bits = (1u << voices) - 1u;
    if (v) {
        if (!cJSON_IsNumber(v) || v->valuedouble < 0 || v->valuedouble >= voices ||
            v->valuedouble != (double)(int)v->valuedouble) {
            send_dap_error_response(seq, cmd, is_psg ? "'voice' must be 0 to 15 on psg"
                                                     : "'voice' must be 0 to 7 on ym");
            return 0;
        }
        bits = 1u << (int)v->valuedouble;
    }

    const voice_mute_owner_t owner = current_caller == DBG_CALLER_MCP ? VOICE_MUTE_MCP : VOICE_MUTE_DAP;
    const unsigned held = is_psg ? psg_debug_get_owner_mute_mask(owner) : YM_debug_get_owner_mute_mask(owner);
    const unsigned next = cJSON_IsTrue(m) ? (held | bits) : (held & ~bits);
    if (is_psg) psg_debug_set_owner_mute_mask(owner, (uint16_t)next);
    else        YM_debug_set_owner_mute_mask(owner, (uint8_t)next);

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "chip", c->valuestring);
    if (v) cJSON_AddNumberToObject(body, "voice", (int)v->valuedouble);
    cJSON_AddBoolToObject(body, "muted", cJSON_IsTrue(m));
    audio_add_mute_masks(body, is_psg ? psg_mute_view() : ym_mute_view());
    send_dap_response(seq, cmd, true, body);
    return 0;
}

// ─── x16/io, x16/ioTrace, x16/ioCapture, x16/interrupt ──────────────

static cJSON *io_device_names(uint32_t mask) {
    cJSON *names = cJSON_CreateArray();
    for (int d = 0; d < IO_DEV_COUNT; d++) {
        if (mask & IO_TRACE_BIT(d))
            cJSON_AddItemToArray(names, cJSON_CreateString(io_trace_device_id((io_device_t)d)));
    }
    return names;
}

static void io_add_capture(cJSON *body) {
    cJSON *o = cJSON_AddObjectToObject(body, "capture");
    cJSON_AddBoolToObject(o, "enabled", io_trace_enabled);
    cJSON_AddItemToObject(o, "ui", io_device_names(io_trace_owner_mask(IO_TRACE_OWNER_UI)));
    cJSON_AddItemToObject(o, "dap", io_device_names(io_trace_owner_mask(IO_TRACE_OWNER_DAP)));
    cJSON_AddItemToObject(o, "mcp", io_device_names(io_trace_owner_mask(IO_TRACE_OWNER_MCP)));
    cJSON_AddItemToObject(o, "effective", io_device_names(io_trace_enabled ? io_trace_device_mask : 0));
}

static cJSON *io_byte_array(const uint8_t *b, int n) {
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) cJSON_AddItemToArray(arr, cJSON_CreateNumber(b[i]));
    return arr;
}

// The level main.c tests after each instruction, by source. Reading it clears
// nothing: every one of these is a comparison of flags the device holds.
static cJSON *io_irq_sources(void) {
    cJSON *src = cJSON_CreateArray();
    if (video_get_irq_out()) cJSON_AddItemToArray(src, cJSON_CreateString("vera"));
    if (via1_irq()) cJSON_AddItemToArray(src, cJSON_CreateString("via1"));
    if (has_via2 && via2_irq()) cJSON_AddItemToArray(src, cJSON_CreateString("via2"));
    if (ym2151_irq_support && YM_irq()) cJSON_AddItemToArray(src, cJSON_CreateString("ym"));
    if (has_midi_card && midi_serial_irq()) cJSON_AddItemToArray(src, cJSON_CreateString("midi"));
    return src;
}

static void io_add_irq(cJSON *body) {
    cJSON *o = cJSON_AddObjectToObject(body, "irq");
    cJSON *src = io_irq_sources();
    cJSON_AddBoolToObject(o, "line", cJSON_GetArraySize(src) > 0);
    cJSON_AddItemToObject(o, "sources", src);
    cJSON_AddBoolToObject(o, "iFlag", (regs.status & FLAG_INTERRUPT) != 0);
    cJSON_AddBoolToObject(o, "inWai", waiting != 0);
}

static cJSON *via_flag_names(uint8_t bits) {
    static const char *const names[7] = { "ca2", "ca1", "sr", "cb2", "cb1", "t2", "t1" };
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < 7; i++) {
        if (bits & (1u << i)) cJSON_AddItemToArray(arr, cJSON_CreateString(names[i]));
    }
    return arr;
}

static void io_add_via(cJSON *body, const char *key, int which, bool fitted) {
    cJSON *o = cJSON_AddObjectToObject(body, key);
    cJSON_AddBoolToObject(o, "fitted", fitted);
    if (!fitted) return;

    via_debug_state_t v;
    via_debug_get_state(which, &v);
    // What the CPU would read at each address, through the debug path: it does
    // not step the I2C bus or clear a flag.
    uint8_t seen[16];
    for (uint8_t r = 0; r < 16; r++) seen[r] = which ? via2_read(r, true) : via1_read(r, true);

    cJSON_AddItemToObject(o, "regs", io_byte_array(seen, 16));
    cJSON_AddNumberToObject(o, "orb", v.regs[0]);
    cJSON_AddNumberToObject(o, "ora", v.regs[1]);
    cJSON_AddNumberToObject(o, "ddrb", v.regs[2]);
    cJSON_AddNumberToObject(o, "ddra", v.regs[3]);
    cJSON_AddNumberToObject(o, "sr", v.regs[10]);
    cJSON_AddNumberToObject(o, "acr", v.regs[11]);
    cJSON_AddNumberToObject(o, "pcr", v.regs[12]);
    cJSON_AddNumberToObject(o, "ifr", seen[13]);
    cJSON_AddNumberToObject(o, "ier", seen[14]);
    cJSON_AddItemToObject(o, "flags", via_flag_names(v.regs[13]));
    cJSON_AddItemToObject(o, "enabled", via_flag_names(v.regs[14]));
    cJSON_AddBoolToObject(o, "irq", (v.regs[13] & v.regs[14]) != 0);
    cJSON *t1 = cJSON_AddObjectToObject(o, "t1");
    cJSON_AddNumberToObject(t1, "count", v.timer_count[0]);
    cJSON_AddNumberToObject(t1, "latch", v.timer_latch[0]);
    cJSON_AddBoolToObject(t1, "running", v.timer_running[0]);
    cJSON_AddBoolToObject(t1, "reloading", v.timer1_m1);
    cJSON *t2 = cJSON_AddObjectToObject(o, "t2");
    cJSON_AddNumberToObject(t2, "count", v.timer_count[1]);
    cJSON_AddNumberToObject(t2, "latchLow", v.timer_latch[1]);
    cJSON_AddBoolToObject(t2, "running", v.timer_running[1]);
    cJSON_AddBoolToObject(o, "pb7", v.pb7_output);
}

static void io_add_i2c(cJSON *body) {
    i2c_debug_state_t s;
    i2c_debug_get_state(&s);
    cJSON *o = cJSON_AddObjectToObject(body, "i2c");
    cJSON_AddNumberToObject(o, "state", s.state);
    cJSON_AddBoolToObject(o, "readMode", s.read_mode);
    cJSON_AddNumberToObject(o, "value", s.value);
    cJSON_AddNumberToObject(o, "count", s.count);
    cJSON_AddNumberToObject(o, "device", s.device);
    cJSON_AddStringToObject(o, "deviceName", s.device_name);
    cJSON_AddNumberToObject(o, "clkIn", s.clk_in);
    cJSON_AddNumberToObject(o, "dataIn", s.data_in);
    cJSON_AddNumberToObject(o, "dataOut", s.data_out);
    cJSON_AddNumberToObject(o, "transactionsStarted", s.transactions_started);
    cJSON_AddNumberToObject(o, "transactionsCompleted", s.transactions_completed);
    cJSON_AddNumberToObject(o, "bytesRead", s.bytes_read);
    cJSON_AddNumberToObject(o, "bytesWritten", s.bytes_written);
    cJSON_AddNumberToObject(o, "kbdFill", s.kbd_fill);
    cJSON_AddNumberToObject(o, "mseFill", s.mse_fill);
}

static void io_add_smc(cJSON *body) {
    smc_debug_state_t s;
    smc_debug_get_state(&s);
    cJSON *o = cJSON_AddObjectToObject(body, "smc");
    cJSON_AddNumberToObject(o, "defaultReadOp", s.default_read_op);
    cJSON_AddNumberToObject(o, "activityLed", s.activity_led);
    cJSON_AddItemToObject(o, "i2cData", io_byte_array(s.i2c_data, SMC_I2C_DATA_LEN));
    cJSON_AddNumberToObject(o, "i2cDataPos", s.i2c_data_pos);
    cJSON_AddNumberToObject(o, "mseCount", s.mse_count);
    cJSON_AddBoolToObject(o, "requestedReset", s.requested_reset);
    cJSON_AddNumberToObject(o, "kbdFill", s.kbd_fill);
    cJSON_AddNumberToObject(o, "mseFill", s.mse_fill);
}

static void io_add_rtc(cJSON *body) {
    rtc_debug_state_t s;
    rtc_debug_get_state(&s);
    cJSON *o = cJSON_AddObjectToObject(body, "rtc");
    cJSON_AddBoolToObject(o, "running", s.running);
    cJSON_AddBoolToObject(o, "h24", s.h24);
    cJSON_AddNumberToObject(o, "seconds", s.seconds);
    cJSON_AddNumberToObject(o, "minutes", s.minutes);
    cJSON_AddNumberToObject(o, "hours", s.hours);
    cJSON_AddNumberToObject(o, "dayOfWeek", s.day_of_week);
    cJSON_AddNumberToObject(o, "day", s.day);
    cJSON_AddNumberToObject(o, "month", s.month);
    cJSON_AddNumberToObject(o, "year", s.year);
    cJSON_AddNumberToObject(o, "i2cDataPos", s.i2c_data_pos);
    char hex[sizeof s.nvram * 2 + 1];
    for (size_t i = 0; i < sizeof s.nvram; i++) snprintf(hex + i * 2, 3, "%02X", s.nvram[i]);
    cJSON_AddStringToObject(o, "nvram", hex);
    cJSON_AddBoolToObject(o, "nvramDirty", s.nvram_dirty);
}

static void io_add_joystick(cJSON *body) {
    joystick_debug_state_t js;
    joystick_debug_get_state(&js);
    cJSON *o = cJSON_AddObjectToObject(body, "joystick");
    cJSON_AddBoolToObject(o, "latch", js.latch);
    cJSON_AddNumberToObject(o, "data", js.data);
    cJSON *ports = cJSON_AddArrayToObject(o, "ports");
    for (int i = 0; i < NUM_JOYSTICKS; i++) {
        const joystick_slot_debug_t *s = &js.slots[i];
        cJSON *p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "port", i + 1);
        // An undriven port reads high forever, which is how the KERNAL tells an
        // empty socket from a pad; shift says the same.
        cJSON_AddBoolToObject(p, "driven", s->virtual_active || s->controller_bound);
        cJSON_AddStringToObject(p, "source", s->virtual_active ? "virtual"
                                             : s->controller_bound ? "controller" : "none");
        cJSON_AddBoolToObject(p, "slotEnabled", s->enabled);
        cJSON_AddNumberToObject(p, "mask", s->button_mask);
        cJSON_AddItemToObject(p, "buttons", dap_joy_buttons_array(s->button_mask));
        cJSON_AddNumberToObject(p, "shift", s->shift_mask);
        cJSON_AddItemToArray(ports, p);
    }
}

static int handle_x16_io(int seq, cJSON *args) {
    static const char cmd[] = "x16/io";
    static const char *const devices[] = { "via1", "via2", "i2c", "smc", "rtc", "joystick" };
    cJSON *d = args ? cJSON_GetObjectItemCaseSensitive(args, "device") : NULL;
    int only = -1;
    if (d) {
        for (int i = 0; i < (int)(sizeof devices / sizeof devices[0]); i++) {
            if (cJSON_IsString(d) && !strcmp(d->valuestring, devices[i])) only = i;
        }
        if (only < 0) {
            send_dap_error_response(seq, cmd, "'device' must be via1, via2, i2c, smc, rtc or joystick");
            return 0;
        }
    }
    cJSON *body = cJSON_CreateObject();
    io_add_irq(body);
    if (only < 0 || only == 0) io_add_via(body, "via1", 0, true);
    if (only < 0 || only == 1) io_add_via(body, "via2", 1, has_via2);
    if (only < 0 || only == 2) io_add_i2c(body);
    if (only < 0 || only == 3) io_add_smc(body);
    if (only < 0 || only == 4) io_add_rtc(body);
    if (only < 0 || only == 5) io_add_joystick(body);
    send_dap_response(seq, cmd, true, body);
    return 0;
}

// Reads only. Turning capture on is x16/ioCapture, so that a read-only client
// has no argument that reaches it.
static int handle_x16_io_trace(int seq, cJSON *args) {
    static const char cmd[] = "x16/ioTrace";
    const uint64_t next = io_trace_next_seq(), oldest = io_trace_oldest_seq();
    uint64_t cursor = oldest;
    int limit = 100;

    cJSON *c = args ? cJSON_GetObjectItemCaseSensitive(args, "cursor") : NULL;
    if (c) {
        if (!cJSON_IsNumber(c) || c->valuedouble < 0 || c->valuedouble > 9007199254740992.0 ||
            c->valuedouble != (double)(uint64_t)c->valuedouble) {
            send_dap_error_response(seq, cmd, "'cursor' must be a whole number from an earlier result");
            return 0;
        }
        cursor = (uint64_t)c->valuedouble;
        if (cursor > next) {
            send_dap_error_response(seq, cmd, "'cursor' is past the newest event");
            return 0;
        }
        if (cursor < 1) cursor = 1;   // events count from 1
    }
    cJSON *l = args ? cJSON_GetObjectItemCaseSensitive(args, "limit") : NULL;
    if (l) {
        if (!cJSON_IsNumber(l) || l->valuedouble < 1 || l->valuedouble > 1024 ||
            l->valuedouble != (int)l->valuedouble) {
            send_dap_error_response(seq, cmd, "'limit' must be a whole number from 1 to 1024");
            return 0;
        }
        limit = (int)l->valuedouble;
    }

    const uint64_t lost = cursor < oldest ? oldest - cursor : 0;
    uint64_t at = cursor < oldest ? oldest : cursor;
    cJSON *body = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(body, "entries");
    for (int n = 0; n < limit && at < next; n++, at++) {
        const io_event_t *e = io_trace_at_seq(at);
        if (!e) break;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "seq", (double)at);
        cJSON_AddNumberToObject(o, "cycles", e->cycles);
        cJSON_AddNumberToObject(o, "pc", e->pc);
        cJSON_AddNumberToObject(o, "programBank", e->pc_bank);
        cJSON_AddStringToObject(o, "device", io_trace_device_id((io_device_t)e->device));
        cJSON_AddStringToObject(o, "kind", e->kind == IO_EVENT_ACCESS ? "access" : "decoded");
        if (e->has_addr) {
            cJSON_AddNumberToObject(o, "address", e->addr);
            cJSON_AddStringToObject(o, "op", e->is_write ? "write" : "read");
        }
        if (e->kind == IO_EVENT_ACCESS) cJSON_AddNumberToObject(o, "value", e->value);
        else                            cJSON_AddStringToObject(o, "text", e->text);
        cJSON_AddItemToArray(list, o);
    }
    cJSON_AddNumberToObject(body, "cursor", (double)at);
    cJSON_AddNumberToObject(body, "lost", (double)lost);
    cJSON_AddBoolToObject(body, "more", at < next);
    cJSON_AddNumberToObject(body, "capacity", io_trace_capacity());
    io_add_capture(body);
    send_dap_response(seq, cmd, true, body);
    return 0;
}

// The caller's whole set, so that a retry cannot change it. [] lets go. Without
// a list, the default: everything but VERA, whose data ports flood the ring, and
// open bus.
static int handle_x16_io_capture(int seq, cJSON *args) {
    static const char cmd[] = "x16/ioCapture";
    cJSON *list = args ? cJSON_GetObjectItemCaseSensitive(args, "devices") : NULL;
    uint32_t mask = IO_TRACE_MASK_DEFAULT;
    if (list) {
        if (!cJSON_IsArray(list)) {
            send_dap_error_response(seq, cmd, "'devices' must be an array of device names");
            return 0;
        }
        mask = 0;
        cJSON *item;
        cJSON_ArrayForEach(item, list) {
            const io_device_t dev = cJSON_IsString(item) ? io_trace_device_from_id(item->valuestring)
                                                         : IO_DEV_COUNT;
            if (dev == IO_DEV_COUNT) {
                send_dap_error_response(seq, cmd, "unknown device: use via1, via2, vera, spi, ym, emu, "
                                                  "midi, openbus, sdcard, files, i2c, joystick or serial");
                return 0;
            }
            mask |= IO_TRACE_BIT(dev);
        }
    }
    io_trace_set_owner_mask(current_caller == DBG_CALLER_MCP ? IO_TRACE_OWNER_MCP : IO_TRACE_OWNER_DAP,
                            mask);
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "cursor", (double)io_trace_next_seq());
    io_add_capture(body);
    send_dap_response(seq, cmd, true, body);
    return 0;
}

// A pulse through the CPU's own entry points, between two instructions. The IRQ
// line is not a stored level: main.c ORs the devices' flags after every
// instruction. So this asserts nothing that could stay up, and clears no flag
// that a device is holding.
static int handle_x16_interrupt(int seq, cJSON *args) {
    static const char cmd[] = "x16/interrupt";
    cJSON *k = args ? cJSON_GetObjectItemCaseSensitive(args, "kind") : NULL;
    if (!k || !cJSON_IsString(k) || (strcmp(k->valuestring, "irq") && strcmp(k->valuestring, "nmi"))) {
        send_dap_error_response(seq, cmd, "'kind' is required: irq or nmi");
        return 0;
    }
    const bool is_nmi = !strcmp(k->valuestring, "nmi");
    const bool i_flag = (regs.status & FLAG_INTERRUPT) != 0;
    const bool taken = is_nmi || !i_flag;
    const bool was_waiting = waiting != 0;
    const bool paused = currentMode == DMODE_STOP;
    const uint16_t vector = (uint16_t)((regs.e ? 0xFFF0 : 0xFFE0) + (is_nmi ? 0x0A : 0x0E));

    if (is_nmi) machine_nmi();
    else        irq6502();

    // Already halted on the handler, so the entry is not a new reason to stop:
    // left pending, a stop filter would fire on it after the next resume.
    if (paused && taken) (void)cpu_irq_take_entered_kinds();

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "kind", k->valuestring);
    cJSON_AddBoolToObject(body, "taken", taken);
    cJSON_AddBoolToObject(body, "held", !taken);
    cJSON_AddBoolToObject(body, "iFlagWasSet", i_flag);
    cJSON_AddBoolToObject(body, "wokeFromWai", was_waiting);
    cJSON_AddNumberToObject(body, "vector", vector);
    cJSON_AddNumberToObject(body, "pc", regs.pc);
    cJSON_AddNumberToObject(body, "programBank", regs.k);
    cJSON_AddNumberToObject(body, "sp", regs.sp);
    cJSON_AddItemToObject(body, "deviceLines", io_irq_sources());
    if (!taken) {
        cJSON_AddStringToObject(body, "note", "the I flag is set, so the IRQ was not taken. The CPU keeps "
                                              "no record of a pulse: it will not fire when I clears.");
    }
    send_dap_response(seq, cmd, true, body);
    if (paused && taken) debug_server_notify_stopped("interrupt");
    return 0;
}

// ─── x16/setWarp, x16/mouse ──────────────────────────────────────────

// An explicit state rather than a toggle, so that a retry cannot undo it.
static int handle_x16_set_warp(int seq, cJSON *args) {
    static const char cmd[] = "x16/setWarp";
    cJSON *e = args ? cJSON_GetObjectItemCaseSensitive(args, "enabled") : NULL;
    if (!e || !cJSON_IsBool(e)) {
        send_dap_error_response(seq, cmd, "'enabled' is required: true or false");
        return 0;
    }
    warp_held = cJSON_IsTrue(e);
    machine_set_warp(warp_held);
    cJSON *body = cJSON_CreateObject();
    cJSON_AddBoolToObject(body, "enabled", warp_mode);
    send_dap_response(seq, cmd, true, body);
    return 0;
}

static bool dap_arg_range_int(cJSON *args, const char *key, int lo, int hi, int *out) {
    cJSON *v = args ? cJSON_GetObjectItemCaseSensitive(args, key) : NULL;
    *out = 0;
    if (!v) return true;
    if (!cJSON_IsNumber(v) || v->valuedouble < lo || v->valuedouble > hi ||
        v->valuedouble != (double)(int)v->valuedouble)
        return false;
    *out = (int)v->valuedouble;
    return true;
}

static int handle_x16_mouse(int seq, cJSON *args) {
    static const char cmd[] = "x16/mouse";
    static const char *const names[3] = {"left", "right", "middle"};
    int dx, dy, wheel;
    if (!dap_arg_range_int(args, "dx", -255, 255, &dx) ||
        !dap_arg_range_int(args, "dy", -255, 255, &dy) ||
        !dap_arg_range_int(args, "wheel", -7, 7, &wheel)) {
        send_dap_error_response(seq, cmd, "'dx' and 'dy' must be -255 to 255, 'wheel' -7 to 7");
        return 0;
    }
    cJSON *bi = args ? cJSON_GetObjectItemCaseSensitive(args, "buttons") : NULL;
    uint8_t held = mouse_buttons();
    if (bi) {
        if (!cJSON_IsArray(bi)) {
            send_dap_error_response(seq, cmd, "'buttons' must be a list: left, right, middle");
            return 0;
        }
        held = 0;
        cJSON *b;
        cJSON_ArrayForEach(b, bi) {
            int which = -1;
            for (int i = 0; i < 3; i++) {
                if (cJSON_IsString(b) && !strcmp(b->valuestring, names[i])) which = i;
            }
            if (which < 0) {
                send_dap_error_response(seq, cmd, "'buttons' may hold left, right and middle");
                return 0;
            }
            held |= (uint8_t)(1 << which);
        }
    }

    if (bi || dx || dy || wheel) {
        for (int i = 0; i < 3; i++) {
            if (held & (1 << i)) mouse_button_down(i);
            else                 mouse_button_up(i);
        }
        if (dx || dy) mouse_move(dx, dy);
        if (wheel) mouse_set_wheel((int8_t)wheel);
        mouse_send_state();
    }

    cJSON *body = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(body, "buttons");
    for (int i = 0; i < 3; i++) {
        if (held & (1 << i)) cJSON_AddItemToArray(list, cJSON_CreateString(names[i]));
    }
    cJSON_AddBoolToObject(body, "kernalMouse", kernal_mouse_enabled);
    send_dap_response(seq, cmd, true, body);
    return 0;
}

// ─── x16/loadPrg ─────────────────────────────────────────────────────

// Reported to the debug-info tracker as this channel. The KERNAL's own loads
// never use it: 15 is the command channel, which carries no file.
#define DAP_LOAD_CHANNEL 15

// Places a program's bytes and brings its .dbg with it, through the same tracker
// a real LOAD reports to, so breakpoints are invalidated and retried. It does
// not go through BASIC, so BASIC's end-of-program pointer is left alone.
static int handle_x16_load_prg(int seq, cJSON *args) {
    static const char cmd[] = "x16/loadPrg";
    cJSON *pi = args ? cJSON_GetObjectItemCaseSensitive(args, "path") : NULL;
    cJSON *ai = args ? cJSON_GetObjectItemCaseSensitive(args, "address") : NULL;
    if (!pi || !cJSON_IsString(pi) || !pi->valuestring[0]) {
        send_dap_error_response(seq, cmd, "'path' is required");
        return 0;
    }
    if (step.pending || !DEBUGIsPaused()) {
        send_dap_error_response(seq, cmd, "the machine must be paused");
        return 0;
    }
    int override_at = -1;
    if (ai) {
        uint32_t a = 0;
        bool is_vram = false;
        if (cJSON_IsNumber(ai) && ai->valuedouble >= 0 && ai->valuedouble <= 0xFFFF) {
            a = (uint32_t)ai->valuedouble;
        } else if (!cJSON_IsString(ai) || !dap_parse_mem_ref(ai->valuestring, &is_vram, &a) ||
                   is_vram || a > 0xFFFF) {
            send_dap_error_response(seq, cmd, "'address' must be a CPU address in bank 0");
            return 0;
        }
        override_at = (int)a;
    }

    char source[4200], err[300];
    if (!caller_read_path(pi->valuestring, source, sizeof source, err, sizeof err)) {
        send_dap_error_response(seq, cmd, err);
        return 0;
    }
    // The .dbg beside it gets read too, and a link there could lead anywhere.
    if (current_caller != DBG_CALLER_DAP) {
        char beside[1024], resolved[4200];
        if (dbg_info_dbg_path_for(source, beside, sizeof beside) && host_path_present(beside) &&
            !caller_read_path(beside, resolved, sizeof resolved, err, sizeof err)) {
            send_dap_error_response(seq, cmd, "the .dbg beside that file is outside the allowed folders");
            return 0;
        }
    }

    SDL_RWops *f = SDL_RWFromFile(source, "rb");
    if (!f) {
        send_dap_error_response(seq, cmd, "cannot read that file");
        return 0;
    }
    const Sint64 size = SDL_RWsize(f);
    if (size < 3 || size > 0x10001) {
        SDL_RWclose(f);
        send_dap_error_response(seq, cmd, "not a program: expected a 2-byte load address and data");
        return 0;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)size);
    const size_t got = buf ? SDL_RWread(f, buf, 1, (size_t)size) : 0;
    SDL_RWclose(f);
    if (got != (size_t)size) {
        free(buf);
        send_dap_error_response(seq, cmd, "cannot read that file");
        return 0;
    }

    const uint32_t start = override_at >= 0 ? (uint32_t)override_at
                                            : (uint32_t)(buf[0] | (buf[1] << 8));
    const uint32_t len = (uint32_t)size - 2;
    const uint32_t end = start + len - 1;
    // Low RAM, or the banked window. Never the bank registers, I/O or ROM.
    const bool fits = (start >= 2 && end < 0x9F00) || (start >= 0xA000 && end <= 0xBFFF);
    if (!fits) {
        free(buf);
        char why[120];
        snprintf(why, sizeof why, "$%04X-$%04X is not all RAM in one window", (unsigned)start,
                 (unsigned)(end & 0xFFFFF));
        send_dap_error_response(seq, cmd, why);
        return 0;
    }

    const uint8_t ram_bank = memory_get_ram_bank();
    dbg_load_begin(DAP_LOAD_CHANNEL, source);
    for (uint32_t i = 0; i < len; i++) {
        const uint16_t at = (uint16_t)(start + i);
        debug_write6502(at, 0, buf[i + 2]);
        dbg_load_note_byte(DAP_LOAD_CHANNEL, at, ram_bank, i == len - 1);
    }
    free(buf);
    const bool debug_info = dbg_load_poll();

    cJSON *body = cJSON_CreateObject();
    dap_add_range_fields(body, start, false, len);
    if (start >= 0xA000) cJSON_AddNumberToObject(body, "bank", ram_bank);
    cJSON_AddBoolToObject(body, "debugInfo", debug_info);
    send_dap_response(seq, cmd, true, body);
    return 0;
}

// Run one request, whoever it came from. Returns 1 if execution mode was changed.
static int dispatch_request(int seq, const char *cmd, cJSON *args) {
    int result = 0;

    // A stop that has already happened is answered as that, whatever this
    // request goes on to do.
    run_to_poll();

    // Another request taking over execution ends a step in flight. Answer it
    // now, rather than leave it to complete at some unrelated later stop. A goto
    // on a running machine is refused, so it takes nothing over.
    if ((step.pending || run_to.pending) &&
        (!strcmp(cmd, "continue") || !strcmp(cmd, "next") || !strcmp(cmd, "stepIn") ||
         !strcmp(cmd, "stepOut") || !strcmp(cmd, "restart") ||
         (!strcmp(cmd, "goto") && currentMode == DMODE_STOP) ||
         !strcmp(cmd, "terminate"))) {
        step_finish("cancelled");
        run_to_finish(false, "cancelled");
    }

    if (dap_no_debug &&
        (!strcmp(cmd, "setBreakpoints") || !strcmp(cmd, "setFunctionBreakpoints") ||
         !strcmp(cmd, "setInstructionBreakpoints") || !strcmp(cmd, "setDataBreakpoints")))
        result = dap_refuse_breakpoints(seq, cmd, args);
    else if (!strcmp(cmd, "initialize"))    result = handle_dap_initialize(seq, args);
    else if (!strcmp(cmd, "launch"))        result = handle_dap_launch(seq, args);
    else if (!strcmp(cmd, "configurationDone")) result = handle_dap_configuration_done(seq);
    else if (!strcmp(cmd, "threads"))       result = handle_dap_threads(seq);
    else if (!strcmp(cmd, "stackTrace"))    result = handle_dap_stack_trace(seq, args);
    else if (!strcmp(cmd, "x16/callStack")) result = handle_x16_call_stack(seq, args);
    else if (!strcmp(cmd, "x16/sourceLocation")) result = handle_x16_source_location(seq, args);
    else if (!strcmp(cmd, "scopes"))        result = handle_dap_scopes(seq, args);
    else if (!strcmp(cmd, "variables"))     result = handle_dap_variables(seq, args);
    else if (!strcmp(cmd, "setBreakpoints"))result = handle_dap_set_breakpoints(seq, args);
    else if (!strcmp(cmd, "continue"))      result = handle_dap_continue(seq, args);
    else if (!strcmp(cmd, "next"))          result = handle_dap_next(seq, args);
    else if (!strcmp(cmd, "stepIn"))        result = handle_dap_step_in(seq, args);
    else if (!strcmp(cmd, "stepOut"))       result = handle_dap_step_out(seq, args);
    else if (!strcmp(cmd, "pause"))         result = handle_dap_pause(seq, args);
    else if (!strcmp(cmd, "disconnect"))    result = handle_dap_disconnect(seq, args);
    else if (!strcmp(cmd, "restart"))       result = handle_dap_restart(seq, args);
    else if (!strcmp(cmd, "gotoTargets"))   result = handle_dap_goto_targets(seq, args);
    else if (!strcmp(cmd, "goto"))          result = handle_dap_goto(seq, args);
    else if (!strcmp(cmd, "evaluate"))      result = handle_dap_evaluate(seq, args);
    else if (!strcmp(cmd, "readMemory"))    result = handle_dap_read_memory(seq, args);
    else if (!strcmp(cmd, "writeMemory"))   result = handle_dap_write_memory(seq, args);
    else if (!strcmp(cmd, "x16/status"))    result = handle_x16_status(seq, args);
    else if (!strcmp(cmd, "x16/vera"))      result = handle_x16_vera(seq, args);
    else if (!strcmp(cmd, "x16/screen"))    result = handle_x16_screen(seq, args);
    else if (!strcmp(cmd, "x16/screenshot")) result = handle_x16_screenshot(seq, args);
    else if (!strcmp(cmd, "x16/registers")) result = handle_x16_registers(seq, args);
    else if (!strcmp(cmd, "x16/sendKey"))   result = handle_x16_send_key(seq, args);
    else if (!strcmp(cmd, "x16/type"))      result = handle_x16_type(seq, args);
    else if (!strcmp(cmd, "x16/joystick"))  result = handle_x16_joystick(seq, args);
    else if (!strcmp(cmd, "x16/step"))      result = handle_x16_step(seq, args);
    else if (!strcmp(cmd, "x16/runTo"))     result = handle_x16_run_to(seq, args);
    else if (!strcmp(cmd, "x16/saveState")) result = handle_x16_save_state(seq, args);
    else if (!strcmp(cmd, "x16/loadState")) result = handle_x16_load_state(seq, args);
    else if (!strcmp(cmd, "x16/perfStats")) result = handle_x16_perf_stats(seq, args);
    else if (!strcmp(cmd, "x16/perfConfig")) result = handle_x16_perf_config(seq, args);
    else if (!strcmp(cmd, "x16/perfHist")) result = handle_x16_perf_hist(seq, args);
    else if (!strcmp(cmd, "x16/hostPerf")) result = handle_x16_host_perf(seq, args);
    else if (!strcmp(cmd, "x16/capture")) result = handle_x16_capture(seq, args);
    else if (!strcmp(cmd, "x16/listBreakpoints")) result = handle_x16_list_breakpoints(seq, args);
    else if (!strcmp(cmd, "x16/readLog")) result = handle_x16_read_log(seq, args);
    else if (!strcmp(cmd, "x16/loadSymbols")) result = handle_x16_load_symbols(seq, args);
    else if (!strcmp(cmd, "x16/lookupSymbol")) result = handle_x16_lookup_symbol(seq, args);
    else if (!strcmp(cmd, "x16/listSymbols")) result = handle_x16_list_symbols(seq, args);
    else if (!strcmp(cmd, "x16/hashMemory")) result = handle_x16_hash_memory(seq, args);
    else if (!strcmp(cmd, "x16/dumpMemory")) result = handle_x16_dump_memory(seq, args);
    else if (!strcmp(cmd, "x16/searchMemory")) result = handle_x16_search_memory(seq, args);
    else if (!strcmp(cmd, "x16/audio")) result = handle_x16_audio(seq, args);
    else if (!strcmp(cmd, "x16/setVoiceMute")) result = handle_x16_set_voice_mute(seq, args);
    else if (!strcmp(cmd, "x16/io")) result = handle_x16_io(seq, args);
    else if (!strcmp(cmd, "x16/ioTrace")) result = handle_x16_io_trace(seq, args);
    else if (!strcmp(cmd, "x16/ioCapture")) result = handle_x16_io_capture(seq, args);
    else if (!strcmp(cmd, "x16/interrupt")) result = handle_x16_interrupt(seq, args);
    else if (!strcmp(cmd, "x16/setWarp")) result = handle_x16_set_warp(seq, args);
    else if (!strcmp(cmd, "x16/mouse")) result = handle_x16_mouse(seq, args);
    else if (!strcmp(cmd, "x16/loadPrg")) result = handle_x16_load_prg(seq, args);
    else if (!strcmp(cmd, "x16/sdcard"))    result = handle_x16_sdcard(seq, args);
    else if (!strcmp(cmd, "x16/spriteMuxStats")) result = handle_x16_sprite_mux_stats(seq, args);
    else if (!strcmp(cmd, "x16/spriteMuxConfig")) result = handle_x16_sprite_mux_config(seq, args);

    else if (!strcmp(cmd, "disassemble"))   result = handle_dap_disassemble(seq, args);
    else if (!strcmp(cmd, "setVariable"))   result = handle_dap_set_variable(seq, args);
    else if (!strcmp(cmd, "setExpression")) result = handle_dap_set_expression(seq, args);
    else if (!strcmp(cmd, "completions"))   result = handle_dap_completions(seq, args);
    else if (!strcmp(cmd, "terminate"))     result = handle_dap_terminate(seq, args);
    else if (!strcmp(cmd, "attach"))        result = handle_dap_attach(seq, args);
    else if (!strcmp(cmd, "setInstructionBreakpoints")) result = handle_dap_set_instruction_breakpoints(seq, args);
    else if (!strcmp(cmd, "dataBreakpointInfo")) result = handle_dap_data_breakpoint_info(seq, args);
    else if (!strcmp(cmd, "setDataBreakpoints")) result = handle_dap_set_data_breakpoints(seq, args);
    else if (!strcmp(cmd, "loadedSources")) result = handle_dap_loaded_sources(seq, args);
    else if (!strcmp(cmd, "source"))        result = handle_dap_source(seq, args);
    else if (!strcmp(cmd, "breakpointLocations")) result = handle_dap_breakpoint_locations(seq, args);
    else if (!strcmp(cmd, "stepInTargets")) result = handle_dap_step_in_targets(seq, args);
    else if (!strcmp(cmd, "setExceptionBreakpoints"))
        result = handle_dap_set_exception_breakpoints(seq, args);
    else if (!strcmp(cmd, "exceptionInfo")) result = handle_dap_exception_info(seq, args);
    else if (!strcmp(cmd, "setFunctionBreakpoints")) {
        result = handle_dap_set_function_breakpoints(seq, args);
    }
    else if (current_caller == DBG_CALLER_MCP && args && !strcmp(cmd, "mcp/setBreakpoint"))
        result = handle_mcp_set_breakpoint(seq, args);
    else if (current_caller == DBG_CALLER_MCP && args && !strcmp(cmd, "mcp/removeBreakpoint"))
        result = handle_mcp_remove_breakpoint(seq, args);
    else if (current_caller == DBG_CALLER_MCP && args && !strcmp(cmd, "mcp/enableBreakpoint"))
        result = handle_mcp_enable_breakpoint(seq, args);
    else if (current_caller == DBG_CALLER_MCP && args && !strcmp(cmd, "mcp/readSource"))
        result = handle_mcp_read_source(seq, args);
    else if (current_caller == DBG_CALLER_MCP && args && !strcmp(cmd, "mcp/evaluate"))
        result = handle_mcp_evaluate(seq, args);
    else if (current_caller == DBG_CALLER_MCP && args && !strcmp(cmd, "mcp/setStopOn"))
        result = handle_mcp_set_stop_on(seq, args);
    else {
        fprintf(stderr, "[dap] Unhandled command: %s\n", cmd);
        send_dap_error_response(seq, cmd, "command not supported");
    }

    return result;
}

// Process a single DAP message. Returns 1 if execution mode was changed.
static int dispatch_dap(const char *json_body) {
    cJSON *root = cJSON_Parse(json_body);
    if (!root) {
        fprintf(stderr, "[dap] Failed to parse JSON: %.80s\n", json_body);
        return 0;
    }

    cJSON *type_item = cJSON_GetObjectItemCaseSensitive(root, "type");
    cJSON *seq_item = cJSON_GetObjectItemCaseSensitive(root, "seq");
    cJSON *cmd_item = cJSON_GetObjectItemCaseSensitive(root, "command");
    cJSON *args = cJSON_GetObjectItemCaseSensitive(root, "arguments");

    if (!type_item || !cJSON_IsString(type_item) || strcmp(type_item->valuestring, "request") != 0) {
        // Only handle requests
        cJSON_Delete(root);
        return 0;
    }

    int seq = (seq_item && cJSON_IsNumber(seq_item)) ? (int)seq_item->valuedouble : 0;
    const char *cmd = (cmd_item && cJSON_IsString(cmd_item)) ? cmd_item->valuestring : "";

    printf("[dap] << %s (seq=%d)\n", cmd, seq);

    const int result = dispatch_request(seq, cmd, args);
    cJSON_Delete(root);
    return result;
}

// ─── Calls from another front end ───────────────────────────────────

void debug_server_set_frontend(const dbg_frontend_t *f) {
    frontend.holds_run_state = f ? f->holds_run_state : NULL;
    frontend.step_done       = f ? f->step_done : NULL;
    frontend.can_resume      = f ? f->can_resume : NULL;
}

// Named one by one. Session, configuration and breakpoint-table requests would
// reach into a DAP client's state, and evaluate is a console with its own
// breakpoint commands.
static const char *const callable_commands[] = {
    "x16/status", "x16/registers", "readMemory", "writeMemory", "disassemble",
    "setVariable", "pause", "continue", "next", "stepIn", "stepOut", "restart",
    "x16/step", "x16/runTo", "x16/listBreakpoints", "x16/type", "x16/sendKey", "x16/joystick",
    "x16/saveState", "x16/loadState", "x16/perfStats", "x16/screen", "x16/screenshot", "x16/vera",
    "mcp/setBreakpoint", "mcp/removeBreakpoint", "mcp/enableBreakpoint", "mcp/evaluate",
    "x16/loadSymbols", "x16/lookupSymbol", "x16/listSymbols", "x16/callStack",
    "loadedSources", "x16/sourceLocation", "mcp/readSource",
    "x16/hashMemory", "x16/dumpMemory", "x16/searchMemory", "x16/setWarp", "x16/mouse", "x16/loadPrg",
    "x16/perfHist", "x16/perfConfig", "x16/hostPerf", "x16/capture",
    "x16/sdcard", "x16/spriteMuxStats", "x16/spriteMuxConfig", "x16/readLog",
    "mcp/setStopOn", "x16/audio", "x16/setVoiceMute",
    "x16/io", "x16/ioTrace", "x16/ioCapture", "x16/interrupt",
};

int debug_server_call(const char *command, cJSON *args, dbg_reply_t *reply) {
    memset(reply, 0, sizeof *reply);

    bool allowed = false;
    for (size_t i = 0; i < sizeof callable_commands / sizeof callable_commands[0]; i++) {
        if (!strcmp(command, callable_commands[i])) {
            allowed = true;
            break;
        }
    }
    if (!allowed || reply_sink) {
        snprintf(reply->message, sizeof reply->message, "%s is not available here", command);
        return 0;
    }

    reply_sink = reply;
    reply_answered = false;
    current_caller = DBG_CALLER_MCP;
    const int result = dispatch_request(0, command, args);
    reply_sink = NULL;
    current_caller = DBG_CALLER_DAP;

    reply->deferred = !reply_answered;
    if (reply->deferred) reply->success = true;
    return result;
}

void debug_server_release_frontend(void) {
    for (int i = 0; i < debug_bp_count(); ) {
        const struct breakpoint *b = debug_bp_at(i);
        if (!(b->owners & (1u << DEBUG_OWNER_MCP))) {
            i++;
            continue;
        }
        const int     pc = b->pc, x16 = b->x16Bank;
        const uint8_t bank = b->bank;
        debug_bp_remove_for(pc, bank, x16, DEBUG_OWNER_MCP);
        // Gone, so its condition goes too. Still here, someone else holds it.
        if (debug_bp_find(pc, bank, x16) < 0) debug_bp_forget(pc, bank, x16);
        else                                  i++;
    }
    debug_wp_clear_owner(DEBUG_OWNER_MCP);
    debug_stop_filter_set(DEBUG_OWNER_MCP, 0);
    DEBUGCancelStepFor(DEBUG_OWNER_MCP);
    if (step.pending && step.caller != DBG_CALLER_DAP) {
        step.pending = false;
        audio_events_end();
    }
    perf_budget_arm(PERF_OWNER_MCP, false);
    vera_bandwidth_arm(VERA_BW_OWNER_MCP, false);
    sprite_trace_arm(SPRITE_TRACE_OWNER_MCP, false);
    psg_debug_set_owner_mute_mask(VOICE_MUTE_MCP, 0);
    YM_debug_set_owner_mute_mask(VOICE_MUTE_MCP, 0);
    io_trace_set_owner_mask(IO_TRACE_OWNER_MCP, 0);
    if (capture_is_live() && cap_serial() == mcp_capture_serial) cap_stop();

    // A headless machine is never left halted with nobody able to restart it.
    if (currentMode == DMODE_STOP && client_sock == SOCKET_INVALID &&
        !dap_other_owner_of_run_state())
        DEBUGContinue();
}

// ─── Connection management ──────────────────────────────────────────

// Everything the session installed in the machine. Run before resuming, so a
// breakpoint set by a client that has gone cannot stop a headless emulator with
// nobody left to notice.
static void dap_release_breakpoints(void) {
    // Everything this session asked for, by name. Each entry goes only if no
    // other owner still wants it, so a -bp breakpoint or one the user set with
    // F9 at the same address survives -- without this file having to know that
    // they exist.
    debug_bp_clear_owner(DEBUG_OWNER_DAP_SOURCE);
    debug_bp_clear_owner(DEBUG_OWNER_DAP_FUNCTION);
    debug_bp_clear_owner(DEBUG_OWNER_DAP_INSTRUCTION);
    debug_bp_clear_owner(DEBUG_OWNER_DAP_CONSOLE);
    debug_wp_clear_owner(DEBUG_OWNER_DAP_SOURCE);
    debug_wp_clear_owner(DEBUG_OWNER_DAP_CONSOLE);
    debug_stop_filter_set(DEBUG_OWNER_DAP_SOURCE, 0);

    num_dap_bps   = 0;
    num_func_bps  = 0;
    num_instr_bps = 0;
    num_data_bps  = 0;
}

// A noDebug session arms nothing, and says so for each entry it was sent.
static int dap_refuse_breakpoints(int seq, const char *command, cJSON *args) {
    cJSON *list = args ? cJSON_GetObjectItemCaseSensitive(args, "breakpoints") : NULL;
    if (!list && args) list = cJSON_GetObjectItemCaseSensitive(args, "lines");
    const int n = (list && cJSON_IsArray(list)) ? cJSON_GetArraySize(list) : 0;
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *bp = cJSON_CreateObject();
        cJSON_AddBoolToObject(bp, "verified", false);
        cJSON_AddStringToObject(bp, "message", "launched with noDebug");
        cJSON_AddItemToArray(arr, bp);
    }
    cJSON *body = cJSON_CreateObject();
    cJSON_AddItemToObject(body, "breakpoints", arr);
    send_dap_response(seq, command, true, body);
    return 0;
}

static void dap_release_session_state(void) {
    dap_release_breakpoints();
    dap_line_base = 1;
    dap_col_base  = 1;
    dap_paths_are_uris = false;
    dap_no_debug = false;

    // A step-over or step-out THIS session started has a breakpoint of its own,
    // and nothing else can retract it. One the local debugger started belongs to
    // the user at the keyboard and is left alone; the debugger decides which is
    // which, since it is the one holding the step.
    DEBUGCancelStepFor(DEBUG_OWNER_DAP_SOURCE);
    run_to.pending = false;

    dap_stop_on_entry = false;
    dap_stop_announced = false;
    dap_launched = false;
    // A step nobody is waiting for must not stop the machine some frames from
    // now, with no client left to resume it. One another front end asked for
    // still has someone waiting.
    if (!(step.pending && step.caller != DBG_CALLER_DAP)) {
        step.pending = false;
        audio_events_end();
    }

    // Let go of anything this session armed for polling, so a client that
    // asked for statistics and then disconnected does not strand the
    // accounting (or, for sprite_trace, its ~600KB of buffers) running for
    // nobody. Symmetric with the owner-arm calls in handle_x16_perf_stats,
    // handle_x16_perf_config, handle_x16_sprite_mux_stats and
    // handle_x16_sprite_mux_config above.
    perf_budget_arm(PERF_OWNER_DAP, false);
    vera_bandwidth_arm(VERA_BW_OWNER_DAP, false);
    sprite_trace_arm(SPRITE_TRACE_OWNER_DAP, false);
    psg_debug_set_owner_mute_mask(VOICE_MUTE_DAP, 0);
    YM_debug_set_owner_mute_mask(VOICE_MUTE_DAP, 0);
    io_trace_set_owner_mask(IO_TRACE_OWNER_DAP, 0);
}

static void disconnect_client(void) {
    if (client_sock != SOCKET_INVALID) {
        CLOSE_SOCKET(client_sock);
        client_sock = SOCKET_INVALID;
        log_frame_drop();
        // Clear the contents, not just the length. A dropped oversized frame
        // otherwise stayed in the buffer as a NUL-terminated string, and the
        // next client -- accepted before it has written anything -- had that
        // header re-parsed and was dropped on connect, for the life of the
        // process.
        recv_buf_len = 0;
        recv_buf[0] = '\0';
        // Everything the session installed goes before anything else, so a
        // breakpoint it set cannot stop a machine it is no longer watching.
        dap_release_session_state();
        dap_seq = 1;
        printf("[dap] Client disconnected\n");

        // Client dropped without sending a proper "disconnect" request. Never
        // kill the emulator — just end the DAP session.
        //
        // Only auto-resume when there is NO interactive debugger UI: in that
        // case a headless, DAP-only session would otherwise stay frozen forever
        // with nothing able to resume it. When the ImGui/SDL debugger is up the
        // user owns the run state (they may have deliberately paused, or be
        // mid-step), so a stray client connect/disconnect must not resume the
        // machine out from under them.
        if (dap_session_active) {
            dap_session_active = false;
            // A UI that was asked for AND is still running: the guest can clear
            // debugger_enabled by writing $9FB0, which suppresses the overlay,
            // and leaving the machine stopped for a window nobody can see would
            // strand it.
            bool has_ui = dap_other_owner_of_run_state();
            if (currentMode == DMODE_STOP && !has_ui) {
                printf("[dap] Session ended, resuming emulator\n");
                currentMode = DMODE_RUN;
            } else {
                printf("[dap] Session ended (run state left unchanged)\n");
            }
        }
    }
}

static void try_accept(void) {
    if (client_sock != SOCKET_INVALID) return;

    struct sockaddr_in client_addr;
    int addr_len = sizeof(client_addr);
#ifdef _WIN32
    socket_t new_sock = accept(listen_sock, (struct sockaddr *)&client_addr, &addr_len);
#else
    socket_t new_sock = accept(listen_sock, (struct sockaddr *)&client_addr, (socklen_t *)&addr_len);
#endif
    if (new_sock == SOCKET_INVALID) return;

    set_nonblocking(new_sock);
    client_sock = new_sock;
    recv_buf_len = 0;
    dap_seq = 1;
    dap_initialize_answered = false;
    // This client hears its own logpoints from now on, and nothing older.
    dbg_log_ring_sink(logpoint_ring(),
                      (uint16_t)((1u << DEBUG_OWNER_DAP_SOURCE) | (1u << DEBUG_OWNER_DAP_FUNCTION) |
                                 (1u << DEBUG_OWNER_DAP_INSTRUCTION) | (1u << DEBUG_OWNER_DAP_CONSOLE)));
    printf("[dap] Client connected from %s:%d\n",
           inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
}

// ─── DAP framing: Content-Length parser ─────────────────────────────

// DAP borrows its framing from HTTP, so an HTTP POST otherwise parses as a DAP
// message: the request line is skipped as an unknown header and the request's
// own Content-Length frames its body. A web page can make a browser send that
// to a loopback port. A request line is never a "Name: value" field, whatever
// its method, and browsers always send Host.
static bool dap_header_block_is_http(const char *line, const char *end) {
    while (line < end) {
        const char *eol = memchr(line, '\n', (size_t)(end - line));
        if (!eol) eol = end;
        const char *stop = eol;
        if (stop > line && stop[-1] == '\r') stop--;

        if (stop > line) {
            const char *p = line;
            while (p < stop && (isalnum((unsigned char)*p) || *p == '-' || *p == '_')) p++;
            if (p == line || p == stop || *p != ':') return true;

            size_t name_len = (size_t)(p - line);
            if ((name_len == 4 && dap_has_prefix_i(line, "host")) ||
                (name_len == 6 && dap_has_prefix_i(line, "origin"))) {
                return true;
            }
        }
        line = eol + 1;
    }
    return false;
}

// Try to extract a complete DAP message from the receive buffer.
// Copies the JSON body into `out` (NUL-terminated) and sets *consumed.
// Returns false if no complete message is available yet.
//
// The body is copied rather than terminated in place. Writing the NUL at
// header_size + content_length lands on the first byte of the NEXT message
// whenever two arrive in one read -- which is the normal case, since a client
// sends setBreakpoints/setFunctionBreakpoints/configurationDone as a burst
// without waiting for each response. That byte then survived compaction as a
// leading NUL, so every later strstr() for the header terminator saw an empty
// string and the connection stopped dispatching until it overflowed.
static bool try_extract_dap_message(char *out, size_t out_size, int *consumed) {
    // Look for Content-Length header
    char *header_end = strstr(recv_buf, "\r\n\r\n");
    if (!header_end) return false;

    int header_size = (int)(header_end - recv_buf) + 4; // include \r\n\r\n

    if (dap_header_block_is_http(recv_buf, header_end)) {
        fprintf(stderr, "[dap] HTTP request on the DAP port; dropping client\n");
        disconnect_client();
        *consumed = 0;
        return false;
    }

    // Parse Content-Length
    int content_length = -1;
    char *cl = recv_buf;
    while (cl < header_end) {
        if (strncmp(cl, "Content-Length:", 15) == 0) {
            cl += 15;
            while (*cl == ' ' || *cl == '\t') cl++;
            content_length = atoi(cl);
            break;
        }
        // Skip to next line
        char *nl = strchr(cl, '\n');
        if (!nl || nl >= header_end) break;
        cl = nl + 1;
    }

    if (content_length < 0) {
        fprintf(stderr, "[dap] Missing Content-Length header\n");
        *consumed = header_size;
        return false;
    }
    if (content_length >= (int)out_size) {
        // Too large to be one of ours. The body may not even have arrived yet,
        // so consuming header + length here would skip bytes that are not in
        // the buffer and then parse the tail of the body as a fresh header.
        // There is no way to resynchronise on a stream we cannot buffer.
        fprintf(stderr, "[dap] message of %d bytes is too large; dropping client\n",
                content_length);
        disconnect_client();
        *consumed = 0;
        return false;
    }

    // Check if we have the complete body
    if (recv_buf_len < header_size + content_length) return false;

    memcpy(out, recv_buf + header_size, (size_t)content_length);
    out[content_length] = '\0';
    *consumed = header_size + content_length;
    return true;
}

// ─── Public API ─────────────────────────────────────────────────────

int debug_server_init(int port) {
    if (!platform_init()) return -1;

    listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == SOCKET_INVALID) {
        fprintf(stderr, "[dap] Failed to create socket\n");
        return -1;
    }

    int optval = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&optval, sizeof(optval));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);

    if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "[dap] Failed to bind to port %d: error %d\n", port, SOCK_ERRNO);
        CLOSE_SOCKET(listen_sock);
        listen_sock = SOCKET_INVALID;
        return -1;
    }

    if (listen(listen_sock, 1) != 0) {
        fprintf(stderr, "[dap] Failed to listen: error %d\n", SOCK_ERRNO);
        CLOSE_SOCKET(listen_sock);
        listen_sock = SOCKET_INVALID;
        return -1;
    }

    set_nonblocking(listen_sock);
    server_enabled = true;
    server_port = port;
    printf("[dap] Debug Adapter Protocol server listening on 127.0.0.1:%d\n", port);
    return 0;
}

static void send_dap_event_thread_started(void) {
    cJSON *tb = cJSON_CreateObject();
    cJSON_AddStringToObject(tb, "reason", "started");
    cJSON_AddNumberToObject(tb, "threadId", 1);
    send_dap_event("thread", tb);
}

// Lines go out in batches, one output event each, so that a logpoint in a hot
// loop costs the host a few sends a frame rather than one per hit. Done here and
// not where a logpoint fires, because sending can drop the client.
#define DAP_LOG_INTERVAL_MS 16
#define DAP_LOG_BATCH_LINES 256

static void dap_send_logs(void) {
    static Uint32 last_batch = 0;
    static char   batch[DAP_LOG_BATCH_LINES * DBG_LOG_TEXT + 64];

    if (!dap_initialize_answered) return;
    if (log_frame) {
        log_frame_push();
        if (log_frame || client_sock == SOCKET_INVALID) return;
    }
    dbg_log_ring_t *ring = logpoint_ring();
    if (ring->sink_next >= ring->next && !ring->sink_lost) return;
    const Uint32 now = SDL_GetTicks();
    if (now - last_batch < DAP_LOG_INTERVAL_MS) return;
    last_batch = now;

    uint64_t lost = 0;
    const dbg_log_entry_t *e = dbg_log_ring_take(ring, &lost);
    if (!e && !lost) return;
    size_t o = 0;
    if (lost)
        o = (size_t)snprintf(batch, sizeof batch, "[logpoints: %llu lines lost]\n",
                             (unsigned long long)lost);
    for (int n = 0; e; ) {
        const size_t len = strlen(e->text);
        memcpy(batch + o, e->text, len);
        o += len;
        batch[o++] = '\n';
        if (++n >= DAP_LOG_BATCH_LINES) break;
        e = dbg_log_ring_take(ring, &lost);
    }
    batch[o] = '\0';

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "category", "console");
    cJSON_AddStringToObject(body, "output", batch);
    cJSON *event = cJSON_CreateObject();
    cJSON_AddNumberToObject(event, "seq", dap_seq++);
    cJSON_AddStringToObject(event, "type", "event");
    cJSON_AddStringToObject(event, "event", "output");
    cJSON_AddItemToObject(event, "body", body);
    log_frame = dap_frame(event, &log_frame_len);
    log_frame_off = 0;
    cJSON_Delete(event);
    if (!log_frame) {
        log_frame_len = 0;
        disconnect_client();
        return;
    }
    log_frame_push();
}

int debug_server_poll(void) {
    if (!server_enabled && !mcp_server_is_enabled()) return 0;

    // Keep the 64-bit cycle count current even under -headless, which has no
    // frames to do it.
    emulated_cycles();
    run_to_poll();

    // Either front end can be running without the other.
    int mode_changed = mcp_server_poll();
    if (!server_enabled) {
        step_poll();
        return mode_changed;
    }

    try_accept();
    if (client_sock == SOCKET_INVALID) {
        step_poll();
        return mode_changed;
    }

    step_poll();

    // Push any budget overruns before reading: a client that never polls still
    // learns that the guest is missing its frame.
    perf_poll_overruns();
    dap_send_logs();
    if (client_sock == SOCKET_INVALID) return mode_changed;

    // Receive data (non-blocking)
    int space = RECV_BUF_SIZE - recv_buf_len - 2; // -2 for null terminator space
    if (space <= 0) {
        fprintf(stderr, "[dap] Receive buffer overflow, disconnecting\n");
        disconnect_client();
        return mode_changed;
    }

    int n = recv(client_sock, recv_buf + recv_buf_len, space, 0);
    if (n > 0) {
        recv_buf_len += n;
        recv_buf[recv_buf_len] = '\0';
    } else if (n == 0) {
        disconnect_client();
        return mode_changed;
    } else {
        if (!WOULD_BLOCK) {
            disconnect_client();
            return mode_changed;
        }
    }

    // Process all complete DAP messages in the buffer
    for (;;) {
        static char body[RECV_BUF_SIZE];
        int consumed = 0;
        if (!try_extract_dap_message(body, sizeof body, &consumed)) {
            if (consumed > 0) {
                int remaining = recv_buf_len - consumed;
                if (remaining > 0) memmove(recv_buf, recv_buf + consumed, remaining);
                if (remaining < 0) remaining = 0;
                recv_buf_len = remaining;
                recv_buf[recv_buf_len] = '\0';
                continue;
            }
            break;
        }

        // Compact before dispatching. A handler can call back into the socket
        // (an event, or an error response), and leaving a consumed message in
        // the buffer would let it be seen twice.
        int remaining = recv_buf_len - consumed;
        if (remaining > 0) {
            memmove(recv_buf, recv_buf + consumed, remaining);
        }
        recv_buf_len = remaining > 0 ? remaining : 0;
        recv_buf[recv_buf_len] = '\0';

        if (dispatch_dap(body)) {
            mode_changed = 1;
        }

        // After each message, not once per batch: a pause that ends a step is
        // answered straight after it, and an x16/step queued behind that pause
        // finds the previous step already complete.
        step_poll();
    }

    return mode_changed;
}

void debug_server_note_resumed(void) {
    mcp_server_note_resumed();
    // A resume the client did not ask for -- someone pressed F5 or F10 in the
    // SDL debug window while a session was attached. It is still watching a
    // machine it believes is halted, so tell it. Resumes that came from a DAP
    // request have already sent their own event and cleared the flag.
    if (dap_stop_announced && server_enabled && client_sock != SOCKET_INVALID) {
        send_continued_event();
        return;   // send_continued_event() clears the flag
    }
    dap_stop_announced = false;
}

void debug_server_notify_stopped(const char *reason) {
    mcp_server_note_stopped();
    if (!server_enabled || client_sock == SOCKET_INVALID) return;
    // Too early to send, so not announced either: configurationDone then reports
    // this stop to the client.
    if (!dap_initialize_answered) return;
    // An x16/step reply says why the step ended; the client asked not to be
    // sent the event as well. Not announced, so nothing owes a continued later.
    if (step.pending && !step.events) return;
    dap_stop_announced = true;

    cJSON *body = cJSON_CreateObject();

    // Map our reason strings to DAP reason strings
    if (!strcmp(reason, "breakpoint")) {
        // Find which DAP breakpoints were hit, and name the kind that asked. A
        // stop from something else (-bp, F9) has no ids and stays "breakpoint".
        cJSON *hit_ids = cJSON_CreateArray();
        const char *kind = NULL;
        for (int i = 0; i < num_dap_bps; i++) {
            // Both halves of the key, and only breakpoints that are actually
            // armed: pending ones hold addr 0 and were reported as hit every
            // time the machine stopped at $0000.
            if (dap_bps[i].verified && !dap_bps[i].log[0] && dap_bps[i].addr == regs.pc &&
                dap_bps[i].bank == regs.k &&
                debug_bank_selector_matches(dap_bps[i].x16Bank, regs.pc, regs.k)) {
                cJSON_AddItemToArray(hit_ids, cJSON_CreateNumber(dap_bps[i].dap_id));
                kind = "breakpoint";
            }
        }
        // Owner-gated: bp_remove in the console can delete an entry these
        // tables still list.
        for (int i = 0; i < num_func_bps; i++) {
            if (func_bp_addrs[i] == regs.pc && func_bp_banks[i] == regs.k &&
                debug_bank_selector_matches(func_bp_x16[i], regs.pc, regs.k) &&
                debug_bp_has_owner(func_bp_addrs[i], func_bp_banks[i], func_bp_x16[i],
                                   DEBUG_OWNER_DAP_FUNCTION)) {
                cJSON_AddItemToArray(hit_ids, cJSON_CreateNumber(func_bp_ids[i]));
                if (!kind) kind = "function breakpoint";
            }
        }
        for (int i = 0; i < num_instr_bps; i++) {
            if (instr_bp_ids[i] && instr_bp_addrs[i] == regs.pc && instr_bp_banks[i] == regs.k &&
                debug_bank_selector_matches(instr_bp_x16[i], regs.pc, regs.k) &&
                debug_bp_has_owner(instr_bp_addrs[i], instr_bp_banks[i], instr_bp_x16[i],
                                   DEBUG_OWNER_DAP_INSTRUCTION)) {
                cJSON_AddItemToArray(hit_ids, cJSON_CreateNumber(instr_bp_ids[i]));
                if (!kind) kind = "instruction breakpoint";
            }
        }
        cJSON_AddStringToObject(body, "reason", kind ? kind : "breakpoint");
        if (cJSON_GetArraySize(hit_ids) > 0) {
            cJSON_AddItemToObject(body, "hitBreakpointIds", hit_ids);
        } else {
            cJSON_Delete(hit_ids);
        }
    } else if (!strcmp(reason, "data breakpoint")) {
        cJSON_AddStringToObject(body, "reason", "data breakpoint");
        // The recorded stop rather than the core's last hit: one instruction
        // can store to several watched bytes, and the stop names the first.
        debug_stop_info_t stop;
        DEBUGGetStopInfo(&stop);
        cJSON *hit_ids = cJSON_CreateArray();
        if (stop.has_access) {
            const uint8_t space = stop.space == DEBUG_STOP_SPACE_VRAM ? DEBUG_WP_SPACE_VRAM
                                                                      : DEBUG_WP_SPACE_CPU;
            const uint8_t access = stop.access == DEBUG_STOP_ACCESS_READ ? DEBUG_WP_ACCESS_READ
                                                                         : DEBUG_WP_ACCESS_WRITE;
            for (int i = 0; i < num_data_bps; i++) {
                if (data_bps[i].space == space &&
                    debug_wp_has_owner_ex(space, data_bps[i].addr, DEBUG_BANK_ANY, DEBUG_OWNER_DAP_SOURCE) &&
                    debug_wp_fires_on_ex(space, access, stop.fetch, data_bps[i].addr, DEBUG_BANK_ANY,
                                         stop.address, stop.value))
                    cJSON_AddItemToArray(hit_ids, cJSON_CreateNumber(data_bps[i].id));
            }
        }
        if (cJSON_GetArraySize(hit_ids) > 0) {
            cJSON_AddItemToObject(body, "hitBreakpointIds", hit_ids);
        } else {
            cJSON_Delete(hit_ids);
        }
    } else if (!strcmp(reason, "exception")) {
        cJSON_AddStringToObject(body, "reason", "exception");
        debug_stop_exception_t ex;
        if (DEBUGGetStopException(&ex)) {
            char text[64];
            exception_text(&ex, text, sizeof text);
            cJSON_AddStringToObject(body, "description", "Paused on exception");
            cJSON_AddStringToObject(body, "text", text);
        }
    } else if (!strcmp(reason, "step")) {
        cJSON_AddStringToObject(body, "reason", "step");
    } else if (!strcmp(reason, "user")) {
        cJSON_AddStringToObject(body, "reason", "pause");
    } else {
        cJSON_AddStringToObject(body, "reason", reason);
    }

    cJSON_AddNumberToObject(body, "threadId", 1);
    cJSON_AddBoolToObject(body, "allThreadsStopped", true);
    debug_server_add_stop_info(body);
    send_dap_event("stopped", body);
}

bool debug_server_has_client(void) {
    return server_enabled && client_sock != SOCKET_INVALID;
}

bool debug_server_is_enabled(void) {
    return server_enabled || mcp_server_is_enabled();
}

bool debug_server_step_pending(void) {
    return step.pending;
}

void debug_server_frame_end(void) {
    machine_frames++;
    process_frames++;
    emulated_cycles();
    frame_end_ticks = clockticks6502;
    frame_end_seen  = true;

    if (step.pending && currentMode != DMODE_STOP) {
        step.frames_run++;
        audio_events_frame_end();
        if (--step.frames_left == 0) {
            // Stop before the next instruction. The reply goes from the poll
            // once the machine has actually parked (see step_poll).
            step.by_frames = true;
            currentMode = DMODE_STOP;
        }
    }
}

bool debug_server_wait(uint32_t timeout_ms) {
    uintptr_t mcp_socks[MCP_SERVER_MAX_SOCKETS];
    const int num_mcp = mcp_server_sockets(mcp_socks, MCP_SERVER_MAX_SOCKETS);
    if (!server_enabled && !num_mcp) return false;

    // Before a client connects, the listening socket turns readable when one
    // does. After, only the client matters: try_accept() ignores the listener
    // while a session is open, so waking for it would just spin.
    const socket_t s = !server_enabled ? SOCKET_INVALID
                     : (client_sock != SOCKET_INVALID) ? client_sock : listen_sock;
    if (s == SOCKET_INVALID && !num_mcp) {
        SDL_Delay(timeout_ms);
        return true;
    }

    fd_set rd;
    FD_ZERO(&rd);
    socket_t high = 0;
    if (s != SOCKET_INVALID) {
        FD_SET(s, &rd);
        high = s;
    }
    for (int i = 0; i < num_mcp; i++) {
        const socket_t m = (socket_t)mcp_socks[i];
        FD_SET(m, &rd);
        if (m > high) high = m;
    }
    struct timeval tv;
    tv.tv_sec  = (long)(timeout_ms / 1000);
    tv.tv_usec = (long)((timeout_ms % 1000) * 1000);
    // The first argument is ignored by Winsock; POSIX wants the highest fd + 1.
    const int r = select((int)high + 1, &rd, NULL, NULL, &tv);
    if (r < 0) {
        // EINTR or a socket that just died. Do not turn the paused loop into a
        // spin; the next poll will notice a dead socket and drop it.
        SDL_Delay(timeout_ms);
    }
    return true;
}

void debug_server_shutdown(void) {
    mcp_server_shutdown();
    if (client_sock != SOCKET_INVALID) {
        // Signal the debuggee exited, then that the session terminated.
        cJSON *eb = cJSON_CreateObject();
        cJSON_AddNumberToObject(eb, "exitCode", 0);
        send_dap_event("exited", eb);
        send_dap_event("terminated", NULL);
        CLOSE_SOCKET(client_sock);
        client_sock = SOCKET_INVALID;
    }
    if (listen_sock != SOCKET_INVALID) {
        CLOSE_SOCKET(listen_sock);
        listen_sock = SOCKET_INVALID;
    }
    server_enabled = false;
    dbg_info_free();
    platform_cleanup();
    printf("[dap] Shut down\n");
}
