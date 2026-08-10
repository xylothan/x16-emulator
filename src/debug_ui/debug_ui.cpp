// Commander X16 Emulator — in-process Dear ImGui debugger implementation.
//
// Owns the ImGui context and the SDL2 + SDLRenderer2 backends. Each frame it
// builds a full-window dock space and iterates the self-registered panel
// registry (debug_ui_panels.h), so panels are added purely by dropping a file
// under panels/.
#include "debug_ui.h"
#include "debug_ui_panels.h"
#include "debug_ui_bridge.h" // regs + execution-control (DEBUGContinue/…/DEBUGIsRunning)

#include "imgui.h"
#include "imgui_internal.h"   // DockBuilder* for the first-run default layout
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"

#include <cstdio>

static SDL_Window   *s_window      = nullptr;
static SDL_Renderer *s_renderer    = nullptr;
static bool          s_initialized = false;
static bool          s_enabled     = false;
static bool          s_build_default_layout = false; // install a dock layout on first run
static bool          s_show_demo    = false;         // Help → ImGui Demo (vendored imgui_demo.cpp)

static void debug_ui_latch_goto(void); // defined below; called from debug_ui_render

extern "C" void
debug_ui_init(SDL_Window *win, SDL_Renderer *ren)
{
    if (s_initialized) {
        return;
    }
    s_window   = win;
    s_renderer = ren;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;      // dockable panels
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // keyboard nav

    ImGui::StyleColorsDark();

    // Detect first run (no imgui.ini yet) so we install a sensible default dock
    // layout once. After that ImGui persists the user's own arrangement and we
    // leave it alone.
    s_build_default_layout = true;
    if (const char *ini = io.IniFilename) {
        if (FILE *f = fopen(ini, "rb")) {
            fclose(f);
            s_build_default_layout = false;
        }
    }

    ImGui_ImplSDL2_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer2_Init(ren);

    s_initialized = true;
    s_enabled     = true;
}

extern "C" void
debug_ui_process_event(const SDL_Event *ev)
{
    if (!s_initialized || ev == nullptr) {
        return;
    }
    ImGui_ImplSDL2_ProcessEvent(ev);
}

// Top main menu bar: View/Layout/Help menus plus the execution-control toolbar
// (Continue/Pause/Step*, a run-state indicator, and the live PC:bank). Buttons
// drive the shared debugger execution control (debugger.c) — the same
// transitions F5/F10/F11 and DAP use.
static void
debug_ui_draw_control_bar(void)
{
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }

    // View: toggle each registered panel. This is the ONLY way to reopen a
    // panel after its window has been closed (the [x]), so it doubles as the
    // panel visibility manager. Iterates the self-registered registry, so new
    // panels appear here automatically.
    if (ImGui::BeginMenu("View")) {
        for (DebugPanel &panel : debug_ui_panels()) {
            ImGui::MenuItem(panel.name, nullptr, &panel.open);
        }
        ImGui::EndMenu();
    }

    // Layout: rebuild the default dock arrangement on demand (recovers from a
    // messy imgui.ini or a panel dragged somewhere unhelpful).
    if (ImGui::BeginMenu("Layout")) {
        if (ImGui::MenuItem("Reset to Default Layout")) {
            for (DebugPanel &panel : debug_ui_panels()) {
                panel.open = true;             // reopen everything so the reset is visible
            }
            s_build_default_layout = true;     // applied after DockSpaceOverViewport() this frame
        }
        ImGui::EndMenu();
    }

    // System: machine-level controls. Reset defers to the emulator loop via the
    // SMC reset flag (safe, == Ctrl-R). IRQ/NMI are edge-triggered.
    if (ImGui::BeginMenu("System")) {
        if (ImGui::MenuItem("Reset")) {
            smc_requested_reset = true;
        }
        if (ImGui::MenuItem("Trigger IRQ")) {
            irq6502();
        }
        if (ImGui::MenuItem("Trigger NMI")) {
            machine_nmi();
        }
        ImGui::EndMenu();
    }

    // Help: the vendored ImGui demo is handy while building/inspecting panels.
    if (ImGui::BeginMenu("Help")) {
        ImGui::MenuItem("Show ImGui Demo", nullptr, &s_show_demo);
        ImGui::Separator();
        ImGui::TextDisabled("Dear ImGui %s", IMGUI_VERSION);
        ImGui::EndMenu();
    }

    ImGui::Separator();

    const bool running = DEBUGIsRunning();
    const bool paused  = DEBUGIsPaused();

    ImGui::BeginDisabled(!paused);
    if (ImGui::Button("Continue")) DEBUGContinue();
    ImGui::EndDisabled();

    ImGui::BeginDisabled(!running);
    ImGui::SameLine();
    if (ImGui::Button("Pause")) DEBUGPause();
    ImGui::EndDisabled();

    ImGui::BeginDisabled(!paused);
    ImGui::SameLine();
    if (ImGui::Button("Step Into")) DEBUGStepInto();
    ImGui::SameLine();
    if (ImGui::Button("Step Over")) DEBUGStepOver();
    ImGui::SameLine();
    if (ImGui::Button("Step Out"))  DEBUGStepOut();
    ImGui::EndDisabled();

    ImGui::Separator();
    if (running) {
        ImGui::TextColored(ImVec4(0.40f, 0.85f, 0.40f, 1.0f), "RUNNING");
    } else if (paused) {
        ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.30f, 1.0f), "PAUSED");
    } else {
        ImGui::TextUnformatted("-");
    }

    ImGui::Separator();
    ImGui::Text("PC %02X:%04X", (unsigned)regs.k, (unsigned)regs.pc);

    ImGui::Separator();
    ImGui::Text("cyc %u  ins %d", (unsigned)clockticks6502, instruction_counter);

    ImGui::EndMainMenuBar();
}

// First-run only (or on Layout → Reset): arrange the panels into a usable
// layout instead of a pile of overlapping windows. Disassembly + Source tabbed
// in the center, CPU + Breakpoints tabbed on the right, Memory + VERA tabbed
// across the bottom. Window names MUST match the panels' ImGui::Begin() titles.
static void
debug_ui_build_default_layout(ImGuiID dockspace_id)
{
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->WorkSize);

    ImGuiID center = dockspace_id;
    ImGuiID right  = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.28f, nullptr, &center);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.34f, nullptr, &center);

    ImGui::DockBuilderDockWindow("Disassembly", center);
    ImGui::DockBuilderDockWindow("Source", center);
    ImGui::DockBuilderDockWindow("CPU", right);
    ImGui::DockBuilderDockWindow("Breakpoints", right);
    ImGui::DockBuilderDockWindow("Symbols", right);
    ImGui::DockBuilderDockWindow("Call Stack", right);
    ImGui::DockBuilderDockWindow("Memory", bottom);
    ImGui::DockBuilderDockWindow("VERA", bottom);
    ImGui::DockBuilderFinish(dockspace_id);
}

extern "C" void
debug_ui_render(void)
{
    if (!s_initialized) {
        return;
    }

    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    // Publish any cross-panel goto requested last frame so every panel sees it.
    debug_ui_latch_goto();

    // Execution-control bar first: it lives in the main menu bar, which shrinks
    // the viewport work area so the dock space sits neatly below it.
    debug_ui_draw_control_bar();

    // Full-window dock space so every panel is dockable/draggable. On first run
    // (no imgui.ini) install a default layout; afterwards ImGui restores the
    // user's saved arrangement.
    ImGuiID dockspace_id = ImGui::DockSpaceOverViewport();
    if (s_build_default_layout) {
        s_build_default_layout = false;
        debug_ui_build_default_layout(dockspace_id);
    }

    // Iterate the self-registered panel registry. No central list to edit:
    // each panel added a file-static DebugPanelRegistration (see
    // debug_ui_panels.h).
    for (DebugPanel &panel : debug_ui_panels()) {
        if (panel.open && panel.render) {
            panel.render(&panel.open);
        }
    }

    // Optional vendored demo window (Help → Show ImGui Demo).
    if (s_show_demo) {
        ImGui::ShowDemoWindow(&s_show_demo);
    }

    ImGui::Render();

    ImGuiIO &io = ImGui::GetIO();
    SDL_RenderSetScale(s_renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    SDL_SetRenderDrawColor(s_renderer, 24, 24, 24, 255);
    SDL_RenderClear(s_renderer);
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), s_renderer);
    SDL_RenderPresent(s_renderer);
}

extern "C" void
debug_ui_shutdown(void)
{
    if (!s_initialized) {
        return;
    }
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    s_window      = nullptr;
    s_renderer    = nullptr;
    s_initialized = false;
    s_enabled     = false;
}

extern "C" bool
debug_ui_is_enabled(void)
{
    return s_enabled;
}

extern "C" void
debug_ui_set_enabled(bool enabled)
{
    s_enabled = enabled;
}

extern "C" SDL_Renderer *
debug_ui_get_renderer(void)
{
    return s_renderer;
}

extern "C" bool
debug_ui_want_capture_keyboard(void)
{
    if (!s_initialized) {
        return false;
    }
    return ImGui::GetIO().WantCaptureKeyboard;
}

// True only while an ImGui text field is actively accepting text (an InputText
// is focused). Unlike WantCaptureKeyboard — which is true whenever the debugger
// window merely has keyboard-nav focus — this stays false when the user is just
// looking at panels, so the F5/F9/F10/F11 debug shortcuts (which ImGui never
// uses for text or nav) still fire. Only genuine text entry (goto/search boxes)
// suppresses them.
extern "C" bool
debug_ui_want_text_input(void)
{
    if (!s_initialized) {
        return false;
    }
    return ImGui::GetIO().WantTextInput;
}

// ── Run-to-cursor + PC breakpoint toggle (keyboard shortcuts) ───────────────
// The Disassembly panel publishes the instruction under the mouse here every
// frame; the Ctrl+F10 shortcut consumes it. These live here (not in a panel) so
// the C event pump in video.c can reach them through the plain C ABI, and so
// they share the same breakpoint/run-to plumbing every panel uses (the bridge).
static uint16_t s_cursor_addr  = 0;
static uint8_t  s_cursor_bank  = 0;
static bool     s_cursor_valid = false;

extern "C" void
debug_ui_set_cursor(uint16_t addr, uint8_t bank, bool valid)
{
    s_cursor_addr  = addr;
    s_cursor_bank  = bank;
    s_cursor_valid = valid;
}

extern "C" bool
debug_ui_run_to_cursor(void)
{
    if (!s_cursor_valid || !DEBUGIsPaused()) {
        return false;
    }
    DEBUGRunTo(s_cursor_addr, s_cursor_bank);
    return true;
}

extern "C" void
debug_ui_toggle_breakpoint_at_pc(void)
{
    // Toggle: remove one at (pc,bank) if present, else add one there.
    if (!DEBUGRemoveBreakPoint((int)regs.pc, regs.k)) {
        struct breakpoint bp;
        bp.pc   = (int)regs.pc;
        bp.bank = regs.k;
        int x16 = -1;
        // Match getCurrentBank()/x16bank_for(): a banked window only applies in
        // program bank 0 (getCurrentBank returns -1 for any k != 0), so guarding
        // on regs.k == 0 keeps the breakpoint matchable instead of dead.
        if (regs.pc >= 0xA000 && regs.k == 0) {
            x16 = (regs.pc < 0xC000) ? (int)memory_get_ram_bank()
                                     : (int)memory_get_rom_bank();
        }
        bp.x16Bank = x16;
        DEBUGAddBreakPoint(bp);
    }
}

// ── Cross-panel "goto" service ──────────────────────────────────────────────
// A one-shot navigation target published by e.g. the Symbols or Call Stack
// panels (debug_ui_request_goto) and consumed by the Disassembly + Memory
// panels (debug_ui_peek_goto). A request made during frame N is latched at the
// top of frame N+1 and stays visible to EVERY panel for that whole frame, so
// panel render order doesn't matter.
static uint16_t s_goto_req_addr    = 0;
static uint8_t  s_goto_req_bank    = 0;
static bool     s_goto_req_pending = false;
static uint16_t s_goto_addr        = 0;
static uint8_t  s_goto_bank        = 0;
static bool     s_goto_active      = false;

extern "C" void
debug_ui_request_goto(uint16_t addr, uint8_t bank)
{
    s_goto_req_addr    = addr;
    s_goto_req_bank    = bank;
    s_goto_req_pending = true;
}

extern "C" bool
debug_ui_peek_goto(uint16_t *addr, uint8_t *bank)
{
    if (!s_goto_active) {
        return false;
    }
    if (addr) *addr = s_goto_addr;
    if (bank) *bank = s_goto_bank;
    return true;
}

// Latch a pending goto request into the frame-visible slot. Called once at the
// top of debug_ui_render before any panel runs.
static void
debug_ui_latch_goto(void)
{
    s_goto_active = s_goto_req_pending;
    if (s_goto_req_pending) {
        s_goto_addr        = s_goto_req_addr;
        s_goto_bank        = s_goto_req_bank;
        s_goto_req_pending = false;
    }
}
