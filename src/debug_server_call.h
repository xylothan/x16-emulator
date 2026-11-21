// Commander X16 Emulator — running debug server requests for another front end.
//
// The DAP handlers are the one implementation of every debugger operation. A
// second front end calls them through here rather than growing its own copy, so
// that a feature behaves the same however it was reached. Only built when the
// debug server is (HAS_DAP), since the interface is in terms of cJSON.

#ifndef _DEBUG_SERVER_CALL_H_
#define _DEBUG_SERVER_CALL_H_

#include <stdbool.h>
#include <cjson/cJSON.h>

typedef enum { DBG_CALLER_DAP, DBG_CALLER_MCP } dbg_caller_t;

typedef struct {
    bool  success;
    bool  deferred;      // the handler answers later, through step_done
    cJSON *body;         // owned by the caller; NULL if the handler sent none
    char  message[256];  // why it failed
} dbg_reply_t;

// What the server needs to know from the other front end. Either may be NULL.
typedef struct {
    // True while it is using the machine. A DAP session ending then leaves the
    // run state alone, and an editor's Stop does not quit the emulator unless
    // the client explicitly asked to terminate the debuggee.
    bool (*holds_run_state)(void);
    // A deferred x16/step finished. Takes ownership of `body`.
    void (*step_done)(cJSON *body);
} dbg_frontend_t;

void debug_server_set_frontend(const dbg_frontend_t *frontend);

// Runs one allowlisted command on behalf of the other front end. Session and
// breakpoint-table requests are refused: those belong to a DAP client. Events
// still go to an attached DAP client, so an editor stays truthful about a
// machine someone else moved. Returns the handler's "mode changed".
int debug_server_call(const char *command, cJSON *args, dbg_reply_t *reply);

// The other front end has gone. Let go of everything it armed, as a DAP session
// ending does, and resume a stopped machine if nobody is left who could.
void debug_server_release_frontend(void);

// Adds a "stop" object describing the access behind the current stop, when the
// machine is paused because a watch fired. Otherwise adds nothing.
void debug_server_add_stop_info(cJSON *body);

#endif
