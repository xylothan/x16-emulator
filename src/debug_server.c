// Commander X16 Emulator — Debug Adapter Protocol (DAP) Server
// Speaks DAP over TCP for direct IDE integration (Visual Studio, VS Code, etc.)
// Transport: Content-Length framed JSON over TCP, default port 9009

#include <SDL.h>
#include "debug_server.h"
#include "dbg_info.h"
#include "debugger.h"
#include "disasm.h"
#include "code_map.h"
#include "perf_budget.h"
#include "perf_hist.h"
#include "host_prof.h"
#include "profile_session.h"
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

#include "cpu/fake6502.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <cjson/cJSON.h>

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
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <fcntl.h>
  #include <unistd.h>
  #include <errno.h>
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

// Send buffer
#define SEND_BUF_SIZE 65536
static char send_buf[SEND_BUF_SIZE];

// Server state
static socket_t listen_sock = SOCKET_INVALID;
static socket_t client_sock = SOCKET_INVALID;
static bool     server_enabled = false;
static int      server_port = 0;

// DAP sequence counter (for events we send)
static int dap_seq = 1;

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
static int      num_func_bps = 0;
static uint16_t instr_bp_addrs[128];
static uint8_t  instr_bp_banks[128];
static int      num_instr_bps = 0;

static bool     dap_session_active = false;
// Whether the stop the machine is currently sitting in has been announced. A
// breakpoint can hit while the client is still configuring, and configurationDone
// would then report the same halt a second time under a different reason.
static bool     dap_stop_announced = false;

// Is there a local debugger UI whose user owns the run state?
//
// When there is, a client connecting or going away must not resume the machine:
// somebody is sitting at a breakpoint looking at it. When there is not -- a
// headless -debugport run -- nothing else can ever resume it, so a session
// ending has to.
//
// Both front ends count, and both need their own test. -imgui does not set
// debug_window_enabled, and -debugport sets debugger_enabled by itself, so
// neither flag alone answers the question: testing only the first ignored the
// graphical debugger entirely and resumed the machine out from under it.
static bool
dap_local_ui_owns_run_state(void)
{
	return (debug_window_enabled && debugger_enabled)
	       || (imgui_debugger_enabled && video_debug_ui_available());
}

// Forward declarations
static void send_dap_event(const char *event_name, cJSON *body);
static void send_dap_message(cJSON *json);
static int dap_apply_bp_condition(dbg_addr_t addr, const char *cond_str, const char *hit_str,
                                  int default_bank);
static int dap_cond_bank_pin(dbg_addr_t addr, const char *cond_str, int default_bank);
static bool dap_parse_instruction_ref(const char *s, long offset, struct breakpoint *out);
static void dap_fmt_bp(char *out, size_t n, int pc, uint8_t bank, int x16Bank);

// Say which bank a breakpoint is pinned to, so a client can see it.
static void dap_add_bank_message(cJSON *bp, int x16Bank) {
    if (x16Bank == DEBUG_BANK_ANY) return;
    char msg[48];
    snprintf(msg, sizeof msg, "bank $%02X only", (unsigned)x16Bank);
    cJSON_AddStringToObject(bp, "message", msg);
}

// Re-resolve unverified breakpoints after new .dbg file loads
static void retry_unverified_breakpoints(void) {
    for (int i = 0; i < num_dap_bps; i++) {
        if (!dap_bps[i].verified) {
            dbg_addr_t addr = 0;
            int seg_bank = -1;
            if (dbg_info_source_to_addr_bank(dap_bps[i].file, dap_bps[i].line, &addr, &seg_bank)) {
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
                    cJSON_AddNumberToObject(bp, "line", dap_bps[i].line);
                    dap_add_bank_message(bp, bank_pin);
                    cJSON_AddItemToObject(body, "breakpoint", bp);
                    cJSON_AddStringToObject(body, "reason", "changed");
                    send_dap_event("breakpoint", body);
                }
            }
        }
    }
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
                cJSON_AddNumberToObject(bp, "line", dap_bps[i].line);
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
    if (line > 0) cJSON_AddNumberToObject(bp, "line", line);
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
        const int pin = debug_normalise_bank(seg_bank, pc, pb);
        if (pin == func_bp_x16[i]) continue;
        move_bp_bank(pc, pb, func_bp_x16[i], pin, DEBUG_OWNER_DAP_FUNCTION);
        func_bp_x16[i] = pin;
        send_bp_changed(func_bp_ids[i], 0, pin);
    }
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

static void send_dap_message(cJSON *json) {
    if (client_sock == SOCKET_INVALID) {
        cJSON_Delete(json);
        return;
    }
    char *body = cJSON_PrintUnformatted(json);
    if (body) {
        int body_len = (int)strlen(body);
        int header_len = snprintf(send_buf, sizeof(send_buf),
                                  "Content-Length: %d\r\n\r\n", body_len);
        if (header_len + body_len >= SEND_BUF_SIZE) {
            // Too large to frame. Dropping it silently left the client waiting
            // on a request_seq that would never be answered -- a source file
            // over 64K is enough to do it, and the editor shows a tab that
            // never finishes loading. The receive side already drops a client
            // it cannot buffer; do the same rather than hang.
            fprintf(stderr, "[dap] response of %d bytes exceeds the send buffer; dropping client\n",
                    header_len + body_len);
            cJSON_free(body);
            cJSON_Delete(json);
            disconnect_client();
            return;
        }
        {
            memcpy(send_buf + header_len, body, body_len);
            int total = header_len + body_len;
            int offset = 0;
            // A client that stops reading fills the socket buffer, and this is
            // the emulator's only thread. Retrying forever parks the machine on
            // a peer that may never come back, so give up after a bounded wait
            // and drop the client instead: a dead session is recoverable, a
            // frozen emulator is not.
            int stalls = 0;
            const int max_stalls = 500;          // ~500ms of a peer not reading
            while (offset < total) {
                int n = send(client_sock, send_buf + offset, total - offset, 0);
                if (n > 0) {
                    offset += n;
                    stalls = 0;
                } else if (n < 0 && WOULD_BLOCK) {
                    if (++stalls > max_stalls) {
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
        cJSON_free(body);
    }
    cJSON_Delete(json);
}

static void send_dap_response(int request_seq, const char *command, bool success, cJSON *body) {
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

static void send_dap_error_response(int request_seq, const char *command, const char *message) {
    cJSON *body = cJSON_CreateObject();
    cJSON *error_obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(error_obj, "id", 1);
    cJSON_AddStringToObject(error_obj, "format", message);
    cJSON_AddItemToObject(body, "error", error_obj);

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

static void send_dap_event(const char *event_name, cJSON *body) {
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

static int handle_dap_initialize(int seq, cJSON *args) {
    (void)args;
    cJSON *caps = cJSON_CreateObject();
    cJSON_AddBoolToObject(caps, "supportsConfigurationDoneRequest", true);
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
    cJSON_AddBoolToObject(caps, "supportsSetExpression", false);
    cJSON_AddBoolToObject(caps, "supportsValueFormattingOptions", true);
    cJSON_AddBoolToObject(caps, "supportsFunctionBreakpoints", true);
    // Newly implemented in this build:
    cJSON_AddBoolToObject(caps, "supportsDataBreakpoints", true);
    cJSON_AddBoolToObject(caps, "supportsInstructionBreakpoints", true);
    cJSON_AddBoolToObject(caps, "supportsLoadedSourcesRequest", true);
    cJSON_AddBoolToObject(caps, "supportsBreakpointLocationsRequest", true);
    cJSON_AddBoolToObject(caps, "supportsStepInTargetsRequest", true);
    cJSON_AddBoolToObject(caps, "supportsConditionalBreakpoints", true);
    cJSON_AddBoolToObject(caps, "supportsHitConditionalBreakpoints", true);
    // Non-standard, so that a client can tell this emulator's performance
    // budget surface from one that would reject the requests.
    cJSON_AddBoolToObject(caps, "supportsX16PerfStats", true);
    // Non-standard: a full breakpoint/watchpoint listing regardless of who
    // set each one, and per-frame sprite-multiplexing stats.
    cJSON_AddBoolToObject(caps, "supportsX16ListBreakpoints", true);
    cJSON_AddBoolToObject(caps, "supportsX16SpriteMuxStats", true);
    send_dap_response(seq, "initialize", true, caps);

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
    }
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
    if (client_sock == SOCKET_INVALID) {
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
        if (!dap_stop_announced) debug_server_notify_stopped("breakpoint");
        return 1;
    }

    // The client wants control before anything executes, and is waiting for the
    // stopped event to know it has it.
    if (dap_stop_on_entry) {
        dap_stop_on_entry = false;
        currentMode = DMODE_STOP;
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

    const char *src_file = NULL;
    int         src_line = 0;
    bool        has_source = dbg_info_addr_to_source_banked(addr, (int)memory_get_ram_bank(),
                                                            &src_file, &src_line);

    const char *label = NULL;
    char        name_buf[80];
    const char *afmt = (bank != 0 || is_gen2) ? "%s ($%02X:%04X)" : "%s ($%04X)";
    const char *nfmt = (bank != 0 || is_gen2) ? "$%02X:%04X" : "$%04X";
    if (dbg_info_addr_to_label(addr, &label) && label) {
        if (bank != 0 || is_gen2) snprintf(name_buf, sizeof(name_buf), afmt, label, bank, addr);
        else                      snprintf(name_buf, sizeof(name_buf), afmt, label, addr);
    } else {
        if (bank != 0 || is_gen2) snprintf(name_buf, sizeof(name_buf), nfmt, bank, addr);
        else                      snprintf(name_buf, sizeof(name_buf), nfmt, addr);
    }
    cJSON_AddStringToObject(frame, "name", name_buf);
    cJSON_AddNumberToObject(frame, "line", has_source ? src_line : 0);
    cJSON_AddNumberToObject(frame, "column", 1);

    if (has_source) {
        cJSON *source = cJSON_CreateObject();
        cJSON_AddStringToObject(source, "path", src_file);
        const char *basename = strrchr(src_file, '/');
        if (!basename) basename = strrchr(src_file, '\\');
        cJSON_AddStringToObject(source, "name", basename ? basename + 1 : src_file);
        cJSON_AddItemToObject(frame, "source", source);
    } else {
        cJSON *hint = cJSON_CreateObject();
        cJSON_AddStringToObject(hint, "kind", is_current ? "normal" : "subtle");
        cJSON_AddItemToObject(frame, "presentationHint", hint);
        cJSON *source = cJSON_CreateObject();
        cJSON_AddStringToObject(source, "name", name_buf);
        cJSON_AddNumberToObject(source, "sourceReference", 0);
        cJSON_AddStringToObject(source, "origin", "disassembly");
        cJSON_AddItemToObject(frame, "source", source);
    }

    char instr_ref[16];
    if (bank != 0 || is_gen2)
        snprintf(instr_ref, sizeof(instr_ref), "0x%06X", ((uint32_t)bank << 16) | addr);
    else
        snprintf(instr_ref, sizeof(instr_ref), "0x%04X", addr);
    cJSON_AddStringToObject(frame, "instructionPointerReference", instr_ref);
    return frame;
}

static int handle_dap_stack_trace(int seq, cJSON *args) {
    int startFrame = 0, levels = 0;
    if (args) {
        cJSON *sf = cJSON_GetObjectItemCaseSensitive(args, "startFrame");
        if (sf && cJSON_IsNumber(sf)) startFrame = (int)sf->valuedouble;
        cJSON *lv = cJSON_GetObjectItemCaseSensitive(args, "levels");
        if (lv && cJSON_IsNumber(lv)) levels = (int)lv->valuedouble;
    }

    // Heuristically unwind the hardware stack (no frame pointer on 6502): frame 0
    // is the current PC, then scan for return-address pairs preceded by a JSR.
    uint16_t addrs[64];
    int      nf = 0;
    addrs[nf++] = regs.pc;

    bool     native = regs.is65c816 && !regs.e;
    uint32_t sp     = regs.sp;
    uint32_t start  = native ? (sp + 1) : (0x100 + (sp & 0xFF) + 1);
    uint32_t end    = native ? (sp + 1 + 256) : 0x200;
    for (uint32_t a = start; a + 1 < end && nf < 64;) {
        uint8_t  lo     = debug_read6502((uint16_t)a, 0, USE_CURRENT_X16_BANK);
        uint8_t  hi     = debug_read6502((uint16_t)(a + 1), 0, USE_CURRENT_X16_BANK);
        uint16_t pushed = (uint16_t)(lo | (hi << 8));
        uint8_t  opc    = debug_read6502((uint16_t)(pushed - 2), regs.k, USE_CURRENT_X16_BANK);
        if (opc == 0x20) { addrs[nf++] = (uint16_t)(pushed + 1); a += 2; }
        else             { a += 1; }
    }

    cJSON *body   = cJSON_CreateObject();
    cJSON *frames = cJSON_CreateArray();
    int    total  = nf;
    if (startFrame < 0) startFrame = 0;
    if (startFrame > nf) startFrame = nf;
    int    last   = (levels > 0) ? (startFrame + levels) : nf;
    if (last > nf) last = nf;
    for (int i = startFrame; i < last; i++) {
        cJSON_AddItemToArray(frames, build_stack_frame(i + 1, addrs[i], regs.k, i == 0));
    }
    cJSON_AddItemToObject(body, "stackFrames", frames);
    cJSON_AddNumberToObject(body, "totalFrames", total);
    send_dap_response(seq, "stackTrace", true, body);
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
    unsigned long v = strtoul(s, &endp, base);
    if (!endp || endp == s) return false;
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

// Direct-page base + offset, matching the CPU's direct_page_add() (support.h).
// The X16 r0-r15 pseudo-registers live at D+$02..D+$21; D is 0 under the KERNAL
// ABI but can be non-zero in 65C816 native (GS) code, so honor it.
static uint16_t dap_dp_add(uint16_t offset) {
    if (regs.e && (regs.dp & 0x00FF) == 0)
        return (uint16_t)((regs.dp & 0xFF00) | (uint8_t)((uint8_t)(regs.dp & 0xFF) + (offset & 0xFF)));
    return (uint16_t)(regs.dp + offset);
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
            uint16_t a  = dap_dp_add((uint16_t)(0x02 + 2 * n));
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
    if (!strncmp(s, "byte[", 5)) { operand = 5; oaddr = (uint16_t)dap_parse_num(s + 5); const char *b = strchr(s, ']'); s = b ? b + 1 : s + strlen(s); }
    else if (!strncmp(s, "word[", 5)) { operand = 6; oaddr = (uint16_t)dap_parse_num(s + 5); const char *b = strchr(s, ']'); s = b ? b + 1 : s + strlen(s); }
    else if (*s == '$' || (*s == '0' && (s[1] == 'x' || s[1] == 'X'))) { operand = 5; oaddr = (uint16_t)dap_parse_num(s); while (*s && *s != ' ' && *s != '=' && *s != '!' && *s != '<' && *s != '>') s++; }
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
    while (*s == ' ') s++;
    *out_operand = operand;
    *out_oaddr   = oaddr;
    *out_op      = op;
    *out_value   = dap_parse_num(s);
    return true;
}

// If a term is "bank[==]N" or "rombank[==]N" (case-insensitive), return the bank
// number; "bank==any" returns DAP_BANK_TERM_ANY; anything else -1. Used to pin a
// breakpoint to a specific RAM/ROM bank, or to keep it in every bank.
#define DAP_BANK_TERM_ANY (-2)
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
    if (dap_ci_prefix(s, "any") || *s == '*') return DAP_BANK_TERM_ANY;
    return (int)(dap_parse_num(s) & 0xFF);
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
    while (*s == ' ') s++;
    if (!*s) return false;
    *out_op  = op;
    *out_val = dap_parse_num(s);
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
    char buf[192];
    buf[0] = '\0';
    if (cond_str && cond_str[0]) {
        strncpy(buf, cond_str, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
    }

    debug_bp_clear_condition(pc, bank, bank_pin);
    debug_bp_set_ignore(pc, bank, bank_pin, 0);
    // Stated in full every time, deliberately. Removing a breakpoint leaves its
    // condition record behind -- that is how a hit count survives an
    // enable/disable toggle -- so a client re-sending a breakpoint without a
    // condition would otherwise inherit the one it had before. The hit count
    // itself is left alone: an editor re-sends a whole file's breakpoints on
    // every keystroke, and a count that restarted each time could not drive a
    // hit condition at all.

    if (buf[0]) {
        bool value_set = false;
        char *p = buf;
        while (p && *p) {
            char *amp = strstr(p, "&&");
            if (amp) *amp = '\0';                 // terminate this term
            if (dap_parse_bank_term(p) == -1 && !value_set) {
                int operand, op; uint16_t oaddr; uint32_t val;
                if (dap_parse_value_term(p, &operand, &oaddr, &op, &val)) {
                    debug_bp_set_condition(pc, bank, bank_pin, operand, oaddr, op, val);
                    value_set = true;
                }
            }
            p = amp ? amp + 2 : NULL;
        }
    }

    if (hit_str && hit_str[0]) {
        const char *h = hit_str;
        while (*h && !(*h >= '0' && *h <= '9')) h++;
        uint32_t n = (uint32_t)strtoul(h, NULL, 0);
        if (n > 0) debug_bp_set_ignore(pc, bank, bank_pin, n - 1); // stop on the Nth hit
    }
    return bank_pin;
}

static int handle_dap_set_breakpoints(int seq, cJSON *args) {
    if (!args) {
        send_dap_error_response(seq, "setBreakpoints", "missing arguments");
        return 0;
    }

    cJSON *source = cJSON_GetObjectItemCaseSensitive(args, "source");
    cJSON *bps = cJSON_GetObjectItemCaseSensitive(args, "breakpoints");
    if (!source || !bps) {
        send_dap_error_response(seq, "setBreakpoints", "missing source or breakpoints");
        return 0;
    }

    cJSON *path_item = cJSON_GetObjectItemCaseSensitive(source, "path");
    const char *src_path = (path_item && cJSON_IsString(path_item)) ? path_item->valuestring : "";

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

    int bp_count = cJSON_GetArraySize(bps);
    for (int i = 0; i < bp_count && num_dap_bps < MAX_DAP_BREAKPOINTS; i++) {
        cJSON *bp_item = cJSON_GetArrayItem(bps, i);
        cJSON *line_item = cJSON_GetObjectItemCaseSensitive(bp_item, "line");
        int line = (line_item && cJSON_IsNumber(line_item)) ? (int)line_item->valuedouble : 0;

        cJSON *result_bp = cJSON_CreateObject();
        int bp_id = next_dap_bp_id++;

        // Kept verbatim so the breakpoint can be re-armed exactly as asked for
        // if an overlay reload forces it to be resolved again.
        const char *cond_str = "";
        const char *hit_str  = "";
        {
            cJSON *c = cJSON_GetObjectItemCaseSensitive(bp_item, "condition");
            if (c && cJSON_IsString(c) && c->valuestring) cond_str = c->valuestring;
            cJSON *h = cJSON_GetObjectItemCaseSensitive(bp_item, "hitCondition");
            if (h && cJSON_IsString(h) && h->valuestring) hit_str = h->valuestring;
        }

        // Try to resolve source:line to address
        dbg_addr_t addr = 0;
        int seg_bank = -1;
        bool resolved = dbg_info_source_to_addr_bank(src_path, line, &addr, &seg_bank);

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
                cJSON_AddNumberToObject(result_bp, "id", bp_id);
                cJSON_AddBoolToObject(result_bp, "verified", false);
                cJSON_AddStringToObject(result_bp, "message",
                                        "Out of memory; breakpoint not armed");
                cJSON_AddNumberToObject(result_bp, "line", line);
                cJSON *bp_source = cJSON_CreateObject();
                cJSON_AddStringToObject(bp_source, "path", src_path);
                cJSON_AddItemToObject(result_bp, "source", bp_source);
                cJSON_AddItemToArray(result_bps, result_bp);
                continue;
            }

            dap_bps[num_dap_bps].dap_id = bp_id;
            dap_bps[num_dap_bps].addr = (uint16_t)(addr & 0xFFFF);
            dap_bps[num_dap_bps].bank = (uint8_t)(addr >> 16);
            dap_bps[num_dap_bps].x16Bank = bank_pin;
            strncpy(dap_bps[num_dap_bps].file, src_path, sizeof(dap_bps[num_dap_bps].file) - 1);
            dap_bps[num_dap_bps].line = line;
            dap_bps[num_dap_bps].verified = true;
            strncpy(dap_bps[num_dap_bps].cond, cond_str, sizeof(dap_bps[num_dap_bps].cond) - 1);
            dap_bps[num_dap_bps].cond[sizeof(dap_bps[num_dap_bps].cond) - 1] = '\0';
            strncpy(dap_bps[num_dap_bps].hit_cond, hit_str, sizeof(dap_bps[num_dap_bps].hit_cond) - 1);
            dap_bps[num_dap_bps].hit_cond[sizeof(dap_bps[num_dap_bps].hit_cond) - 1] = '\0';
            num_dap_bps++;


            cJSON_AddNumberToObject(result_bp, "id", bp_id);
            cJSON_AddBoolToObject(result_bp, "verified", true);
            cJSON_AddNumberToObject(result_bp, "line", line);
            dap_add_bank_message(result_bp, bank_pin);

            // Add source info
            cJSON *bp_source = cJSON_CreateObject();
            cJSON_AddStringToObject(bp_source, "path", src_path);
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
            cJSON_AddNumberToObject(result_bp, "line", line);
            cJSON *bp_source = cJSON_CreateObject();
            cJSON_AddStringToObject(bp_source, "path", src_path);
            cJSON_AddItemToObject(result_bp, "source", bp_source);

            // Store as pending — will set hardware BP when matching .dbg loads
            dap_bps[num_dap_bps].dap_id = bp_id;
            dap_bps[num_dap_bps].addr = 0;
            dap_bps[num_dap_bps].bank = 0;
            dap_bps[num_dap_bps].x16Bank = DEBUG_BANK_ANY;
            strncpy(dap_bps[num_dap_bps].file, src_path, sizeof(dap_bps[num_dap_bps].file) - 1);
            dap_bps[num_dap_bps].line = line;
            dap_bps[num_dap_bps].verified = false;
            strncpy(dap_bps[num_dap_bps].cond, cond_str, sizeof(dap_bps[num_dap_bps].cond) - 1);
            dap_bps[num_dap_bps].cond[sizeof(dap_bps[num_dap_bps].cond) - 1] = '\0';
            strncpy(dap_bps[num_dap_bps].hit_cond, hit_str, sizeof(dap_bps[num_dap_bps].hit_cond) - 1);
            dap_bps[num_dap_bps].hit_cond[sizeof(dap_bps[num_dap_bps].hit_cond) - 1] = '\0';
            num_dap_bps++;
        }
        cJSON_AddItemToArray(result_bps, result_bp);
    }

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
static int handle_dap_next(int seq, cJSON *args) {
    (void)args;
    dap_stop_announced = false;   // this request implies execution continues
    DEBUGStepOverAuto(DEBUG_OWNER_DAP_SOURCE);
    send_dap_response(seq, "next", true, NULL);
    return 1;
}

static int handle_dap_step_in(int seq, cJSON *args) {
    (void)args;
    dap_stop_announced = false;   // this request implies execution continues
    DEBUGStepIntoAuto();
    send_dap_response(seq, "stepIn", true, NULL);
    return 1;
}

static int handle_dap_step_out(int seq, cJSON *args) {
    (void)args;
    dap_stop_announced = false;   // this request implies execution continues
    DEBUGStepOut(DEBUG_OWNER_DAP_SOURCE);
    send_dap_response(seq, "stepOut", true, NULL);
    return 1;
}

static int handle_dap_pause(int seq, cJSON *args) {
    (void)args;
    const bool wasRunning = (currentMode != DMODE_STOP);
    // Emits the stopped event itself on the RUN->STOP transition.
    DEBUGBreakToDebugger();
    send_dap_response(seq, "pause", true, NULL);
    // Already halted -- a previous client left it that way, or a breakpoint got
    // there first. There is no transition for DEBUGBreakToDebugger() to report,
    // and the protocol still owes this request a stopped event; without one the
    // client waits forever for a pause that has in fact already happened.
    if (!wasRunning && !dap_stop_announced) {
        debug_server_notify_stopped("pause");
    }
    return 1;
}

static int handle_dap_disconnect(int seq, cJSON *args) {
    // Default to terminating since the emulator is always launched for debugging
    bool terminate = true;
    if (args) {
        cJSON *term_item = cJSON_GetObjectItemCaseSensitive(args, "terminateDebuggee");
        if (term_item && cJSON_IsBool(term_item)) {
            terminate = cJSON_IsTrue(term_item);
        }
    }
    send_dap_response(seq, "disconnect", true, NULL);

    if (terminate) {
        SDL_Event quit_event;
        quit_event.type = SDL_QUIT;
        SDL_PushEvent(&quit_event);
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
        if (currentMode == DMODE_STOP && !dap_local_ui_owns_run_state()) {
            currentMode = DMODE_RUN;
        }
    }
    return 0;
}

static int handle_dap_restart(int seq, cJSON *args) {
    (void)args;
    machine_reset();
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
    int line = line_item->valueint;

    // Look up the address for this source line using debug info
    dbg_addr_t addr = 0;
    bool found = false;
    if (path && cJSON_IsString(path)) {
        found = dbg_info_source_to_addr(path->valuestring, line, &addr);
    }

    cJSON *body = cJSON_CreateObject();
    cJSON *targets = cJSON_CreateArray();

    if (found) {
        cJSON *target = cJSON_CreateObject();
        cJSON_AddNumberToObject(target, "id", (int)addr);
        char label[32];
        snprintf(label, sizeof(label), "$%04X", addr);
        cJSON_AddStringToObject(target, "label", label);
        cJSON_AddNumberToObject(target, "line", line);
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

    uint16_t addr = (uint16_t)target_id->valueint;
    regs.pc = addr;

    send_dap_response(seq, "goto", true, NULL);

    // Send stopped event at new location
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "reason", "goto");
    cJSON_AddNumberToObject(body, "threadId", 1);
    cJSON_AddBoolToObject(body, "allThreadsStopped", true);
    send_dap_event("stopped", body);

    return 0;
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

    // Parse expression: $XXXX = memory peek, register names, bp_* commands
    char result[1024] = "???";

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
    } else if (expr[0] == '$' || expr[0] == '0') {
        // Memory address (24-bit: high byte is the 65C816 bank byte).
        uint32_t addr;
        if (expr[0] == '$') {
            addr = (uint32_t)strtoul(expr + 1, NULL, 16);
        } else {
            addr = (uint32_t)strtoul(expr, NULL, 16);
        }
        addr &= 0xFFFFFF;
        uint8_t val = debug_read6502((uint16_t)(addr & 0xFFFF), (uint8_t)(addr >> 16), USE_CURRENT_X16_BANK);
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
            len += snprintf(result + len, sizeof(result) - len, " $%04X(%d)",
                           wp->addr, wp->len);
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
    } else if (strncmp(expr, "vera_reg", 8) == 0) {
        // vera_reg — dump all VERA registers ($9F20-$9F3F)
        int len = 0;
        for (int i = 0; i < 32 && len < (int)sizeof(result) - 10; i++) {
            uint8_t val = vera_read_dbg((uint8_t)i);
            len += snprintf(result + len, sizeof(result) - len, "%02X ", val);
        }
    }

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "result", result);
    cJSON_AddNumberToObject(body, "variablesReference", 0);
    send_dap_response(seq, "evaluate", true, body);
    return 0;
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
    int count = (int)count_item->valuedouble;
    const uint32_t addr_mask = is_vram ? 0x1FFFF : 0xFFFFFF;
    addr = (uint32_t)((int)addr + offset) & addr_mask;
    if (count > 4096) count = 4096;
    if (count < 0) count = 0;

    // Build base64 data (DAP readMemory uses base64)
    // For simplicity, send as hex-encoded data with unreadableBytes=0
    static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned char *raw = (unsigned char *)malloc(count);
    char *b64 = (char *)malloc((count * 4 / 3) + 8);
    if (!raw || !b64) {
        free(raw); free(b64);
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
                         : debug_read6502((uint16_t)(cur & 0xFFFF), (uint8_t)(cur >> 16), USE_CURRENT_X16_BANK);
    }

    // Base64 encode
    int b64_len = 0;
    for (int i = 0; i < count; i += 3) {
        int remaining = count - i;
        unsigned int triplet = raw[i] << 16;
        if (remaining > 1) triplet |= raw[i + 1] << 8;
        if (remaining > 2) triplet |= raw[i + 2];

        b64[b64_len++] = b64chars[(triplet >> 18) & 0x3F];
        b64[b64_len++] = b64chars[(triplet >> 12) & 0x3F];
        b64[b64_len++] = (remaining > 1) ? b64chars[(triplet >> 6) & 0x3F] : '=';
        b64[b64_len++] = (remaining > 2) ? b64chars[triplet & 0x3F] : '=';
    }
    b64[b64_len] = '\0';

    cJSON *body = cJSON_CreateObject();
    char addr_str[16];
    // Echoed in the space it was read from, so a client can page through VRAM by
    // feeding this straight back with an offset.
    if (is_vram)
        snprintf(addr_str, sizeof(addr_str), "vram:0x%05X", addr & 0x1FFFF);
    else if (is_gen2)
        snprintf(addr_str, sizeof(addr_str), "0x%06X", addr & 0xFFFFFF);
    else
        snprintf(addr_str, sizeof(addr_str), "0x%04X", addr & 0xFFFF);
    cJSON_AddStringToObject(body, "address", addr_str);
    cJSON_AddNumberToObject(body, "unreadableBytes", 0);
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

    if (buttons_item && !cJSON_IsArray(buttons_item)) {
        send_dap_error_response(seq, "x16/joystick", "'buttons' must be an array of names");
        return 0;
    }
    if (mask_item && !cJSON_IsNumber(mask_item)) {
        send_dap_error_response(seq, "x16/joystick", "'mask' must be a number");
        return 0;
    }

    if (enabled_item && cJSON_IsBool(enabled_item) && !cJSON_IsTrue(enabled_item)) {
        joystick_clear_virtual(index);
    } else if (buttons_item || mask_item) {
        // Start from "nothing pressed" so an absent button is a released one.
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
                    char err[96];
                    snprintf(err, sizeof(err), "unknown button '%s'", b->valuestring);
                    send_dap_error_response(seq, "x16/joystick", err);
                    return 0;
                }
                mask &= (uint16_t)~bit;
            }
        }
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
    // time is wall clock and runs about five times higher, so a client that
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

// ─── Whole-program histogram (x16/perfHist) ─────────────────────────────────
// Arguments, all optional, applied in this order:
//   enabled: bool        arm/disarm (allocates ~21 MB on first arm)
//   caller:  {start,end} the main-loop range whose JSRs open tasks
//   reset:   bool        zero everything
//   dump:    "path"      write the sparse histogram (see perf_hist.h)
// The body always carries the totals and the task table.
static int handle_x16_perf_hist(int seq, cJSON *args) {
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
    item = args ? cJSON_GetObjectItemCaseSensitive(args, "dump") : NULL;
    if (cJSON_IsString(item) && item->valuestring)
        dumped = perf_hist_dump(item->valuestring);

    cJSON *body = cJSON_CreateObject();
    cJSON_AddBoolToObject(body, "enabled", perf_hist_enabled);
    cJSON_AddNumberToObject(body, "frames", perf_hist_frames());
    cJSON_AddNumberToObject(body, "cycles", (double)perf_hist_total_cycles());
    cJSON_AddNumberToObject(body, "irqCycles", (double)perf_hist_irq_cycles());
    cJSON_AddNumberToObject(body, "loopCycles", (double)perf_hist_loop_cycles());
    if (dumped != -2)
        cJSON_AddNumberToObject(body, "dumped", (double)dumped);
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

// ─── Capture mode (x16/capture) ─────────────────────────────────────────────
// Record a stretch of running with the debugger quiet, then read where the
// guest's time went, by routine. See profile_capture.h.
// Arguments, all optional, applied in this order:
//   start: {frames, seconds, onMarker, recorders: ["hist","budget","vram"]}
//          begin one (recorders default to ["hist"]); refused while one runs
//   stop:  true   end a running capture, or cancel one armed for a marker
//   top:   n      report rows to return (default 50; 0 = none)
// The body always carries the state; once a capture has ended, its report.
static int handle_x16_capture(int seq, cJSON *args) {
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
                                            "unknown recorder (expected hist, tree, budget or vram)");
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
    }
    cJSON *stop = args ? cJSON_GetObjectItemCaseSensitive(args, "stop") : NULL;
    if (cJSON_IsTrue(stop))
        cap_stop();
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

    const prof_report_t *r = profile_session_report();
    if (r && cap_state() == CAP_DONE) {
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
        cJSON_AddItemToObject(body, "report", rep);
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
    if (!perf_budget_owner_wants(PERF_OWNER_DAP) && !perf_budget_is_enabled()) {
        perf_budget_arm(PERF_OWNER_DAP, true);
    }
    if (!vera_bandwidth_owner_wants(VERA_BW_OWNER_DAP) && !vera_bandwidth_is_enabled()) {
        vera_bandwidth_arm(VERA_BW_OWNER_DAP, true);
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

    cJSON *item = cJSON_GetObjectItemCaseSensitive(args, "enabled");
    if (cJSON_IsBool(item)) {
        const bool want = cJSON_IsTrue(item);
        if (!perf_budget_arm(PERF_OWNER_DAP, want)) {
            send_dap_error_response(seq, "x16/perfConfig",
                                    "could not allocate the history ring");
            return 0;
        }
        // Turning profiling on is as clear a statement of intent as there is,
        // so overrun events follow it unless the client says otherwise below.
        perf_overrun_events = want;
        // The bandwidth accounting follows the same switch. Two requests to
        // answer one question ("profile this machine") would be a surface
        // nobody wants to remember.
        vera_bandwidth_arm(VERA_BW_OWNER_DAP, want);
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
    if (cJSON_IsBool(item))
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
static cJSON *owners_bitmask_to_json(uint16_t owners) {
    static const char *names[] = {
        "cli", "ui", "dapSource", "dapFunction", "dapInstruction", "dapConsole", "step"
    };
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < DEBUG_OWNER_COUNT; i++) {
        if (!(owners & (1u << i))) continue;
        if (i < (int)(sizeof names / sizeof names[0])) {
            cJSON_AddItemToArray(arr, cJSON_CreateString(names[i]));
        } else {
            char buf[16];
            snprintf(buf, sizeof buf, "owner%d", i);
            cJSON_AddItemToArray(arr, cJSON_CreateString(buf));
        }
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
                    cJSON_AddNumberToObject(o, "line", dap_bps[j].line);
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
        cJSON_AddNumberToObject(o, "addr", wp->addr);
        cJSON_AddNumberToObject(o, "len", wp->len);
        cJSON_AddNumberToObject(o, "x16Bank", wp->x16Bank);
        cJSON_AddBoolToObject(o, "active", wp->active);
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
    if (!sprite_trace_owner_wants(SPRITE_TRACE_OWNER_DAP) && !sprite_trace_active) {
        sprite_trace_arm(SPRITE_TRACE_OWNER_DAP, true);
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
        if (!sprite_trace_arm(SPRITE_TRACE_OWNER_DAP, cJSON_IsTrue(item))) {
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

static int handle_dap_disassemble(int seq, cJSON *args) {
    if (!args) {
        send_dap_error_response(seq, "disassemble", "missing arguments");
        return 0;
    }
    cJSON *ref_item = cJSON_GetObjectItemCaseSensitive(args, "memoryReference");
    cJSON *count_item = cJSON_GetObjectItemCaseSensitive(args, "instructionCount");
    cJSON *offset_item = cJSON_GetObjectItemCaseSensitive(args, "offset");

    if (!ref_item || !cJSON_IsString(ref_item)) {
        send_dap_error_response(seq, "disassemble", "missing memoryReference");
        return 0;
    }

    uint32_t ref = (uint32_t)strtoul(ref_item->valuestring, NULL, 0);
    int offset = (offset_item && cJSON_IsNumber(offset_item)) ? (int)offset_item->valuedouble : 0;
    int count = (count_item && cJSON_IsNumber(count_item)) ? (int)count_item->valuedouble : 10;
    uint32_t ref24 = (uint32_t)(((int)ref + offset)) & 0xFFFFFF;
    uint16_t addr = (uint16_t)(ref24 & 0xFFFF);
    uint8_t  bank = (uint8_t)(ref24 >> 16); // 65C816 program bank
    if (count > 200) count = 200;
    if (count < 0) count = 0;

    // Decode via the anchored code map so operand widths follow the recorded
    // 65C816 M/X/E flag state (the naive path assumed a fixed width and drifted).
    uint8_t rambank = memory_get_ram_bank();
    uint8_t rombank = memory_get_rom_bank();
    static code_map_line_t lines[200];
    int n = code_map_disasm_forward(addr, bank, rambank, rombank, count, lines, 200, NULL);

    cJSON *instructions = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        const code_map_line_t *ln = &lines[i];

        cJSON *inst = cJSON_CreateObject();
        char addr_str[16];
        if (bank != 0 || is_gen2)
            snprintf(addr_str, sizeof(addr_str), "0x%06X", ((uint32_t)bank << 16) | ln->addr);
        else
            snprintf(addr_str, sizeof(addr_str), "0x%04X", ln->addr);
        cJSON_AddStringToObject(inst, "address", addr_str);
        cJSON_AddStringToObject(inst, "instruction", ln->text);

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
            cJSON *source = cJSON_CreateObject();
            cJSON_AddStringToObject(source, "path", src_file);
            cJSON_AddItemToObject(inst, "source", source);
            cJSON_AddNumberToObject(inst, "line", src_line);
        }

        cJSON_AddItemToArray(instructions, inst);
    }

    cJSON *body = cJSON_CreateObject();
    cJSON_AddItemToObject(body, "instructions", instructions);
    send_dap_response(seq, "disassemble", true, body);
    return 0;
}

// ─── DAP command dispatch ───────────────────────────────────────────

// ─── setVariable ─────────────────────────────────────────────────────
static int handle_dap_set_variable(int seq, cJSON *args) {
    if (!args) { send_dap_error_response(seq, "setVariable", "missing arguments"); return 0; }
    cJSON *ref_i  = cJSON_GetObjectItemCaseSensitive(args, "variablesReference");
    cJSON *name_i = cJSON_GetObjectItemCaseSensitive(args, "name");
    cJSON *val_i  = cJSON_GetObjectItemCaseSensitive(args, "value");
    int ref = (ref_i && cJSON_IsNumber(ref_i)) ? (int)ref_i->valuedouble : 0;
    const char *name   = (name_i && cJSON_IsString(name_i)) ? name_i->valuestring : "";
    const char *valstr = (val_i && cJSON_IsString(val_i)) ? val_i->valuestring : "";
    uint32_t v = dap_parse_num(valstr);
    bool ok = true;
    bool a16 = regs.is65c816 && !regs.e && !(regs.status & FLAG_MEMORY_WIDTH);
    bool x16 = regs.is65c816 && !regs.e && !(regs.status & FLAG_INDEX_WIDTH);

    if (ref == VARREF_REGISTERS) {
        if      (!strcmp(name, "PC")) regs.pc = (uint16_t)v;
        else if (!strcmp(name, "A"))  { if (a16) regs.c = (uint16_t)v; else regs.a = (uint8_t)v; }
        else if (!strcmp(name, "X"))  { if (x16) regs.x = (uint16_t)v; else regs.xl = (uint8_t)v; }
        else if (!strcmp(name, "Y"))  { if (x16) regs.y = (uint16_t)v; else regs.yl = (uint8_t)v; }
        else if (!strcmp(name, "SP")) regs.sp = regs.is65c816 ? (uint16_t)v : (uint16_t)((regs.sp & 0xFF00) | (v & 0xFF));
        else if (!strcmp(name, "D"))  regs.dp = (uint16_t)v;
        else if (!strcmp(name, "DBR")) regs.db = (uint8_t)v;
        else if (!strcmp(name, "PBR")) regs.k = (uint8_t)v;
        else if (name[0] == 'P')      regs.status = (uint8_t)v; // "P (Status)"
        else if (!strcmp(name, "RAM Bank")) memory_set_ram_bank((uint8_t)v);
        else if (!strcmp(name, "ROM Bank")) memory_set_rom_bank((uint8_t)v);
        else ok = false;
    } else if (ref == VARREF_STACK) {
        debug_write6502((uint16_t)dap_parse_num(name), 0, (uint8_t)v); // name is "$XXXX"
    } else if (ref == VARREF_VREGS) {
        // name is "Rn" (n = 0..15) → 16-bit little-endian at direct-page + $02 + 2*n.
        if ((name[0] == 'R' || name[0] == 'r') && name[1]) {
            int n = atoi(name + 1);
            if (n >= 0 && n <= 15) {
                uint16_t a = dap_dp_add((uint16_t)(0x02 + 2 * n));
                debug_write6502(a, 0, (uint8_t)(v & 0xFF));
                debug_write6502((uint16_t)(a + 1), 0, (uint8_t)((v >> 8) & 0xFF));
            } else ok = false;
        } else ok = false;
    } else {
        ok = false; // zero-page rows are 16-byte groups — not individually settable
    }

    if (!ok) { send_dap_error_response(seq, "setVariable", "cannot set this variable"); return 0; }
    char result[40];
    snprintf(result, sizeof result, "$%X (%u)", v, v);
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "value", result);
    cJSON_AddNumberToObject(body, "variablesReference", 0);
    send_dap_response(seq, "setVariable", true, body);
    return 0;
}

// ─── terminate (end debugging; keep the emulator running) ────────────
static int handle_dap_terminate(int seq, cJSON *args) {
    (void)args;
    send_dap_response(seq, "terminate", true, NULL);
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
    if (!dap_local_ui_owns_run_state()) {
        currentMode = DMODE_RUN;
    }
    return 1;
}

// ─── attach (the emulator is already running; mirror launch) ─────────
static int handle_dap_attach(int seq, cJSON *args) {
    if (args) {
        cJSON *dbg_file = cJSON_GetObjectItemCaseSensitive(args, "dbgFile");
        if (dbg_file && cJSON_IsString(dbg_file)) dbg_info_load(dbg_file->valuestring);
    }
    send_dap_response(seq, "attach", true, NULL);
    send_dap_event_thread_started();
    return 0;
}

// ─── setFunctionBreakpoints (label breakpoints via .dbg) ─────────────
static int handle_dap_set_function_breakpoints(int seq, cJSON *args) {
    // "Here is the complete list" -- so let go of everything this kind of
    // breakpoint asked for, and start again. Anything another owner also wants
    // stays armed for them; nothing here has to know who they are.
    debug_bp_clear_owner(DEBUG_OWNER_DAP_FUNCTION);
    num_func_bps = 0;

    cJSON *body = cJSON_CreateObject();
    cJSON *arr  = cJSON_CreateArray();
    cJSON *bps  = args ? cJSON_GetObjectItemCaseSensitive(args, "breakpoints") : NULL;
    int    n    = (bps && cJSON_IsArray(bps)) ? cJSON_GetArraySize(bps) : 0;
    for (int i = 0; i < n; i++) {
        cJSON *fb = cJSON_GetArrayItem(bps, i);
        cJSON *nm = cJSON_GetObjectItemCaseSensitive(fb, "name");
        cJSON *rb = cJSON_CreateObject();
        dbg_addr_t addr;
        bool verified = false;
        int seg_bank = -1;
        if (nm && cJSON_IsString(nm) && dbg_info_label_to_addr_bank(nm->valuestring, &addr, &seg_bank)
            && num_func_bps < 128) {
            // Pinned to the bank the label's segment was loaded into. Armed for
            // any bank, a label at $A000 also stopped in every other bank's code
            // at $A000.
            const int pc  = (int)(addr & 0xFFFF);
            const uint8_t pb = (uint8_t)(addr >> 16);
            const int x16 = debug_normalise_bank(seg_bank, pc, pb);
            struct breakpoint bp = { pc, pb, x16, 0, false };
            if (debug_bp_add_for(bp, DEBUG_OWNER_DAP_FUNCTION) != DEBUG_ADD_FULL) {
                const int id = next_dap_bp_id++;
                func_bp_banks[num_func_bps] = pb;
                func_bp_addrs[num_func_bps] = (uint16_t)pc;
                func_bp_x16[num_func_bps]   = x16;
                func_bp_ids[num_func_bps]   = id;
                strncpy(func_bp_names[num_func_bps], nm->valuestring, sizeof func_bp_names[0] - 1);
                func_bp_names[num_func_bps][sizeof func_bp_names[0] - 1] = '\0';
                num_func_bps++;
                cJSON_AddNumberToObject(rb, "id", id);
                cJSON_AddNumberToObject(rb, "instructionReference", addr);
                dap_add_bank_message(rb, x16);
                verified = true;
            }
        }
        cJSON_AddBoolToObject(rb, "verified", verified);
        cJSON_AddItemToArray(arr, rb);
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

// A number running to the end of the string: $hex, 0xhex or decimal.
static bool dap_number_to_end(const char *s) {
    while (*s == ' ') s++;
    int base = 10;
    if (*s == '$') { s++; base = 16; }
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; base = 16; }
    const char *start = s;
    while (base == 16 ? isxdigit((unsigned char)*s) : isdigit((unsigned char)*s)) s++;
    if (s == start) return false;
    while (*s == ' ') s++;
    return *s == '\0';
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
        if (b != -1) {
            // dap_parse_bank_term() reads any digits it finds; make sure that
            // is all there was.
            const char *v = t + (dap_ci_prefix(t, "rombank") ? 7 : 4);
            while (*v == ' ' || *v == '=' || *v == '#') v++;
            const bool any = dap_ci_prefix(v, "any") ? !v[3] || v[3] == ' ' : *v == '*';
            if (!any && !dap_number_to_end(v)) {
                snprintf(err, errn, "cannot read the bank in \"%s\"", t);
                return false;
            }
            if (*bank_out != -1 && *bank_out != b) {
                snprintf(err, errn, "condition names two different banks");
                return false;
            }
            *bank_out = b;
        } else {
            int operand, op; uint16_t oaddr; uint32_t val;
            const char *v = strpbrk(t, "=!<>");
            if (v) v += strspn(v, "=!<>");
            if (!dap_parse_value_term(t, &operand, &oaddr, &op, &val) || !v || !dap_number_to_end(v)) {
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
    while (*h == ' ') h++;
    if (h[0] == '>' && h[1] == '=') h += 2;
    while (*h == ' ') h++;
    if (!isdigit((unsigned char)*h)) return false;
    return dap_number_to_end(h) && strtoul(h, NULL, 10) > 0;
}

// x16Dump on an instruction breakpoint makes it a DUMP breakpoint: it does not
// stop; each firing appends memory to a file (debug_core.c).
//   "x16Dump": {"file": path, "regions": [{"addr": n, "len": n, "bank": n?}]}
// addr is a 24-bit CPU address; bank pins an x16 RAM bank for $A000-$BFFF.
static uint8_t dap_dump_read(uint32_t addr, int x16Bank) {
    return debug_read6502((uint16_t)(addr & 0xFFFF), (uint8_t)(addr >> 16), x16Bank);
}

static bool dap_apply_dump(const struct breakpoint *bp, cJSON *dump) {
    cJSON *file = cJSON_GetObjectItemCaseSensitive(dump, "file");
    cJSON *regs = cJSON_GetObjectItemCaseSensitive(dump, "regions");
    if (!file || !cJSON_IsString(file) || !regs || !cJSON_IsArray(regs))
        return false;
    uint32_t addr[16], len[16];
    int      bank[16], n = 0;
    cJSON   *r;
    cJSON_ArrayForEach(r, regs) {
        cJSON *a = cJSON_GetObjectItemCaseSensitive(r, "addr");
        cJSON *l = cJSON_GetObjectItemCaseSensitive(r, "len");
        cJSON *b = cJSON_GetObjectItemCaseSensitive(r, "bank");
        if (n >= 16 || !a || !l || !cJSON_IsNumber(a) || !cJSON_IsNumber(l))
            return false;
        addr[n] = (uint32_t)a->valuedouble;
        len[n]  = (uint32_t)l->valuedouble;
        bank[n] = (b && cJSON_IsNumber(b)) ? (int)b->valuedouble : USE_CURRENT_X16_BANK;
        n++;
    }
    debug_bp_dump_read = dap_dump_read;
    return debug_bp_set_dump(bp->pc, bp->bank, bp->x16Bank, file->valuestring, n, addr, len, bank);
}

static int handle_dap_set_instruction_breakpoints(int seq, cJSON *args) {
    // The request REPLACES the set.  Close the previous set's dump files first:
    // clearing an owner leaves condition records behind, and a dump left on a
    // record would ride along on the next plain breakpoint at that address.
    // Remember the keys too, so the conditions of the ones not asked for again
    // can be dropped below rather than left for a later breakpoint there.
    struct { int pc; uint8_t bank; int x16Bank; } old[128];
    int n_old = 0;
    for (int i = 0; i < debug_bp_count(); i++) {
        const struct breakpoint *b = debug_bp_at(i);
        if (b->owners & (1u << DEBUG_OWNER_DAP_INSTRUCTION)) {
            debug_bp_clear_dump(b->pc, b->bank, b->x16Bank);
            if (n_old < 128) {
                old[n_old].pc = b->pc;
                old[n_old].bank = b->bank;
                old[n_old].x16Bank = b->x16Bank;
                n_old++;
            }
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
            dap_apply_bp_condition(a24, cond_str, hit_str, bp.x16Bank);
            if (debug_bp_add_for(bp, DEBUG_OWNER_DAP_INSTRUCTION) != DEBUG_ADD_FULL) {
                instr_bp_banks[num_instr_bps] = bp.bank;
                instr_bp_addrs[num_instr_bps++] = (uint16_t)bp.pc;
                cJSON_AddNumberToObject(rb, "id", next_dap_bp_id++);
                verified = true;
                cJSON *dump = cJSON_GetObjectItemCaseSensitive(ib, "x16Dump");
                if (dump) {
                    if (dap_apply_dump(&bp, dump))
                        cJSON_AddBoolToObject(rb, "x16Dump", true);
                    else {
                        debug_bp_remove_for(bp.pc, bp.bank, bp.x16Bank, DEBUG_OWNER_DAP_INSTRUCTION);
                        if (debug_bp_find(bp.pc, bp.bank, bp.x16Bank) < 0)
                            debug_bp_forget(bp.pc, bp.bank, bp.x16Bank);
                        num_instr_bps--;
                        verified = false;
                        snprintf(why, sizeof why,
                                 "x16Dump needs {file, regions: [{addr, len, bank?}]}, at most 16 "
                                 "regions, and a file that can be opened");
                    }
                } else {
                    debug_bp_clear_dump(bp.pc, bp.bank, bp.x16Bank);
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
static int handle_dap_data_breakpoint_info(int seq, cJSON *args) {
    cJSON *name_i = args ? cJSON_GetObjectItemCaseSensitive(args, "name") : NULL;
    cJSON *body = cJSON_CreateObject();
    if (name_i && cJSON_IsString(name_i)) {
        uint16_t addr = (uint16_t)dap_parse_num(name_i->valuestring);
        char dataId[16]; snprintf(dataId, sizeof dataId, "%u", addr);
        char desc[64];   snprintf(desc, sizeof desc, "write $%04X", addr);
        cJSON_AddStringToObject(body, "dataId", dataId);
        cJSON_AddStringToObject(body, "description", desc);
        cJSON *at = cJSON_CreateArray();
        cJSON_AddItemToArray(at, cJSON_CreateString("write"));
        cJSON_AddItemToObject(body, "accessTypes", at);
        cJSON_AddBoolToObject(body, "canPersist", false);
    } else {
        cJSON_AddNullToObject(body, "dataId");
        cJSON_AddStringToObject(body, "description", "not available");
    }
    send_dap_response(seq, "dataBreakpointInfo", true, body);
    return 0;
}

// ─── setDataBreakpoints (maps to emulator write watchpoints) ─────────
static int handle_dap_set_data_breakpoints(int seq, cJSON *args) {
    // The client's complete list, so let go of the previous one. Watches the
    // console made are owned separately and are left alone, as are any from -wp.
    debug_wp_clear_owner(DEBUG_OWNER_DAP_SOURCE);

    cJSON *body = cJSON_CreateObject();
    cJSON *arr  = cJSON_CreateArray();
    cJSON *bps  = args ? cJSON_GetObjectItemCaseSensitive(args, "breakpoints") : NULL;
    int    n    = (bps && cJSON_IsArray(bps)) ? cJSON_GetArraySize(bps) : 0;
    for (int i = 0; i < n; i++) {
        cJSON *db = cJSON_GetArrayItem(bps, i);
        cJSON *id = cJSON_GetObjectItemCaseSensitive(db, "dataId");
        cJSON *rb = cJSON_CreateObject();
        bool verified = false;
        if (id && cJSON_IsString(id)) {
            uint16_t addr = (uint16_t)dap_parse_num(id->valuestring);
            if (debug_wp_add_for(addr, 1, DEBUG_BANK_ANY, DEBUG_OWNER_DAP_SOURCE)
                != DEBUG_ADD_FULL) {
                verified = true;
                // Optional written-value filter via the DAP dataBreakpoint's
                // `condition` (e.g. "== $05", "!=0", "$FF"): only break when the
                // byte written to addr compares true.
                cJSON *c = cJSON_GetObjectItemCaseSensitive(db, "condition");
                if (c && cJSON_IsString(c) && c->valuestring[0]) {
                    int op; uint32_t val;
                    if (dap_parse_simple_cmp(c->valuestring, &op, &val))
                        debug_wp_set_value(addr, DEBUG_BANK_ANY, op, (uint8_t)val);
                }
            }
        }
        cJSON_AddBoolToObject(rb, "verified", verified);
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
    int n = dbg_info_file_count();
    for (int i = 0; i < n; i++) {
        const char *nm = NULL;
        if (dbg_info_file_at(i, &nm) && nm) {
            const char *base = nm;
            for (const char *p = nm; *p; ++p) if (*p == '/' || *p == '\\') base = p + 1;
            cJSON *src = cJSON_CreateObject();
            cJSON_AddStringToObject(src, "name", base);
            cJSON_AddStringToObject(src, "path", nm);
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
    if (args) {
        cJSON *src = cJSON_GetObjectItemCaseSensitive(args, "source");
        if (src) {
            cJSON *p = cJSON_GetObjectItemCaseSensitive(src, "path");
            if (!p) p = cJSON_GetObjectItemCaseSensitive(src, "name");
            if (p && cJSON_IsString(p)) path = p->valuestring;
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
    if (args) {
        cJSON *src = cJSON_GetObjectItemCaseSensitive(args, "source");
        if (src) { cJSON *p = cJSON_GetObjectItemCaseSensitive(src, "path"); if (p && cJSON_IsString(p)) path = p->valuestring; }
        cJSON *l  = cJSON_GetObjectItemCaseSensitive(args, "line");    if (l && cJSON_IsNumber(l))  line = (int)l->valuedouble;
        cJSON *el = cJSON_GetObjectItemCaseSensitive(args, "endLine"); if (el && cJSON_IsNumber(el)) endLine = (int)el->valuedouble;
    }
    if (endLine < line) endLine = line;
    cJSON *body = cJSON_CreateObject();
    cJSON *arr  = cJSON_CreateArray();
    for (int ln = line; path && ln <= endLine; ln++) {
        dbg_addr_t addr;
        if (dbg_info_source_to_addr(path, ln, &addr)) {
            cJSON *loc = cJSON_CreateObject();
            cJSON_AddNumberToObject(loc, "line", ln);
            cJSON_AddItemToArray(arr, loc);
        }
    }
    cJSON_AddItemToObject(body, "breakpoints", arr);
    send_dap_response(seq, "breakpointLocations", true, body);
    return 0;
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

    int result = 0;

    printf("[dap] << %s (seq=%d)\n", cmd, seq);

    if (!strcmp(cmd, "initialize"))         result = handle_dap_initialize(seq, args);
    else if (!strcmp(cmd, "launch"))        result = handle_dap_launch(seq, args);
    else if (!strcmp(cmd, "configurationDone")) result = handle_dap_configuration_done(seq);
    else if (!strcmp(cmd, "threads"))       result = handle_dap_threads(seq);
    else if (!strcmp(cmd, "stackTrace"))    result = handle_dap_stack_trace(seq, args);
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
    else if (!strcmp(cmd, "x16/registers")) result = handle_x16_registers(seq, args);
    else if (!strcmp(cmd, "x16/sendKey"))   result = handle_x16_send_key(seq, args);
    else if (!strcmp(cmd, "x16/type"))      result = handle_x16_type(seq, args);
    else if (!strcmp(cmd, "x16/joystick"))  result = handle_x16_joystick(seq, args);
    else if (!strcmp(cmd, "x16/perfStats")) result = handle_x16_perf_stats(seq, args);
    else if (!strcmp(cmd, "x16/perfConfig")) result = handle_x16_perf_config(seq, args);
    else if (!strcmp(cmd, "x16/perfHist")) result = handle_x16_perf_hist(seq, args);
    else if (!strcmp(cmd, "x16/hostPerf")) result = handle_x16_host_perf(seq, args);
    else if (!strcmp(cmd, "x16/capture")) result = handle_x16_capture(seq, args);
    else if (!strcmp(cmd, "x16/listBreakpoints")) result = handle_x16_list_breakpoints(seq, args);
    else if (!strcmp(cmd, "x16/spriteMuxStats")) result = handle_x16_sprite_mux_stats(seq, args);
    else if (!strcmp(cmd, "x16/spriteMuxConfig")) result = handle_x16_sprite_mux_config(seq, args);

    else if (!strcmp(cmd, "disassemble"))   result = handle_dap_disassemble(seq, args);
    else if (!strcmp(cmd, "setVariable"))   result = handle_dap_set_variable(seq, args);
    else if (!strcmp(cmd, "terminate"))     result = handle_dap_terminate(seq, args);
    else if (!strcmp(cmd, "attach"))        result = handle_dap_attach(seq, args);
    else if (!strcmp(cmd, "setInstructionBreakpoints")) result = handle_dap_set_instruction_breakpoints(seq, args);
    else if (!strcmp(cmd, "dataBreakpointInfo")) result = handle_dap_data_breakpoint_info(seq, args);
    else if (!strcmp(cmd, "setDataBreakpoints")) result = handle_dap_set_data_breakpoints(seq, args);
    else if (!strcmp(cmd, "loadedSources")) result = handle_dap_loaded_sources(seq, args);
    else if (!strcmp(cmd, "source"))        result = handle_dap_source(seq, args);
    else if (!strcmp(cmd, "breakpointLocations")) result = handle_dap_breakpoint_locations(seq, args);
    else if (!strcmp(cmd, "stepInTargets")) result = handle_dap_step_in_targets(seq, args);
    else if (!strcmp(cmd, "setExceptionBreakpoints")) {
        // VS always sends this — just acknowledge
        send_dap_response(seq, "setExceptionBreakpoints", true, NULL);
    }
    else if (!strcmp(cmd, "setFunctionBreakpoints")) {
        result = handle_dap_set_function_breakpoints(seq, args);
    }
    else {
        fprintf(stderr, "[dap] Unhandled command: %s\n", cmd);
        send_dap_error_response(seq, cmd, "command not supported");
    }

    cJSON_Delete(root);
    return result;
}

// ─── Connection management ──────────────────────────────────────────

// Everything the session installed in the machine. Run before resuming, so a
// breakpoint set by a client that has gone cannot stop a headless emulator with
// nobody left to notice.
static void dap_release_session_state(void) {
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

    num_dap_bps   = 0;
    num_func_bps  = 0;
    num_instr_bps = 0;

    // A step-over or step-out THIS session started has a breakpoint of its own,
    // and nothing else can retract it. One the local debugger started belongs to
    // the user at the keyboard and is left alone; the debugger decides which is
    // which, since it is the one holding the step.
    DEBUGCancelStepFor(DEBUG_OWNER_DAP_SOURCE);

    dap_stop_on_entry = false;
    dap_stop_announced = false;

    // Let go of anything this session armed for polling, so a client that
    // asked for statistics and then disconnected does not strand the
    // accounting (or, for sprite_trace, its ~600KB of buffers) running for
    // nobody. Symmetric with the owner-arm calls in handle_x16_perf_stats,
    // handle_x16_perf_config, handle_x16_sprite_mux_stats and
    // handle_x16_sprite_mux_config above.
    perf_budget_arm(PERF_OWNER_DAP, false);
    vera_bandwidth_arm(VERA_BW_OWNER_DAP, false);
    sprite_trace_arm(SPRITE_TRACE_OWNER_DAP, false);
}

static void disconnect_client(void) {
    if (client_sock != SOCKET_INVALID) {
        CLOSE_SOCKET(client_sock);
        client_sock = SOCKET_INVALID;
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
            bool has_ui = dap_local_ui_owns_run_state();
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
    printf("[dap] Client connected from %s:%d\n",
           inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
}

// ─── DAP framing: Content-Length parser ─────────────────────────────

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

int debug_server_poll(void) {
    if (!server_enabled) return 0;

    try_accept();
    if (client_sock == SOCKET_INVALID) return 0;

    // Push any budget overruns before reading: a client that never polls still
    // learns that the guest is missing its frame.
    perf_poll_overruns();

    // Receive data (non-blocking)
    int space = RECV_BUF_SIZE - recv_buf_len - 2; // -2 for null terminator space
    if (space <= 0) {
        fprintf(stderr, "[dap] Receive buffer overflow, disconnecting\n");
        disconnect_client();
        return 0;
    }

    int n = recv(client_sock, recv_buf + recv_buf_len, space, 0);
    if (n > 0) {
        recv_buf_len += n;
        recv_buf[recv_buf_len] = '\0';
    } else if (n == 0) {
        disconnect_client();
        return 0;
    } else {
        if (!WOULD_BLOCK) {
            disconnect_client();
            return 0;
        }
    }

    // Process all complete DAP messages in the buffer
    int mode_changed = 0;
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

    }

    return mode_changed;
}

void debug_server_note_resumed(void) {
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
    if (!server_enabled || client_sock == SOCKET_INVALID) return;
    dap_stop_announced = true;

    cJSON *body = cJSON_CreateObject();

    // Map our reason strings to DAP reason strings
    if (!strcmp(reason, "breakpoint")) {
        cJSON_AddStringToObject(body, "reason", "breakpoint");
        // Find which DAP breakpoint was hit
        cJSON *hit_ids = cJSON_CreateArray();
        for (int i = 0; i < num_dap_bps; i++) {
            // Both halves of the key, and only breakpoints that are actually
            // armed: pending ones hold addr 0 and were reported as hit every
            // time the machine stopped at $0000.
            if (dap_bps[i].verified && dap_bps[i].addr == regs.pc &&
                dap_bps[i].bank == regs.k &&
                debug_bank_selector_matches(dap_bps[i].x16Bank, regs.pc, regs.k)) {
                cJSON_AddItemToArray(hit_ids, cJSON_CreateNumber(dap_bps[i].dap_id));
            }
        }
        if (cJSON_GetArraySize(hit_ids) > 0) {
            cJSON_AddItemToObject(body, "hitBreakpointIds", hit_ids);
        } else {
            cJSON_Delete(hit_ids);
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
    send_dap_event("stopped", body);
}

bool debug_server_has_client(void) {
    return server_enabled && client_sock != SOCKET_INVALID;
}

bool debug_server_is_enabled(void) {
    return server_enabled;
}

void debug_server_shutdown(void) {
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
