// All rights reserved. License: 2-clause BSD

#include <SDL.h>
#include <SDL_syswm.h>

#include <windows.h>
#include <dwmapi.h>

#include "glue.h"

void video_win32_set_rounded_corners(SDL_Window *window)
{
	SDL_SysWMinfo wmInfo;
	SDL_VERSION(&wmInfo.version);
	SDL_GetWindowWMInfo(window, &wmInfo);

	HWND hwnd = wmInfo.info.win.window;
	DWM_WINDOW_CORNER_PREFERENCE preference = DWMWCP_ROUNDSMALL;
	DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &preference, sizeof(preference));
}

// --- Keep the emulator running while the window is dragged/resized ----------
//
// Moving or resizing a window on Windows spins the OS in a modal message loop
// (WM_ENTERSIZEMOVE..WM_EXITSIZEMOVE) that never returns to our main loop, so
// emulation and painting would otherwise freeze until the drag ends.
//
// SDL2's SDL_SetWindowsMessageHook is invoked from WIN_PumpEvents (SDL's own
// PeekMessage loop). That loop does NOT run while the OS owns the thread inside
// the modal move/resize loop, so the hook never sees WM_ENTERSIZEMOVE/WM_TIMER
// and cannot drive stepping. Instead we subclass the window procedure directly
// (SetWindowLongPtrW / GWLP_WNDPROC): the OS dispatches messages straight to our
// proc even inside the modal loop. We arm a WM_TIMER for the duration of that
// loop and advance the emulator one frame per tick, chaining every other message
// to SDL's original proc so SDL keeps working exactly as before.

#define X16_MOVE_TIMER_ID 0xB16

static WNDPROC video_win32_orig_wndproc = NULL;
static bool    win32_in_modal_move      = false;

static LRESULT CALLBACK
video_win32_subclass_proc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message) {
		case WM_ENTERSIZEMOVE:
			win32_in_modal_move = true;
			// ~15 ms ≈ one video frame; WM_TIMER fires even when the mouse is
			// held still, so emulation keeps ticking during a stationary hold.
			SetTimer(hWnd, X16_MOVE_TIMER_ID, 15, NULL);
			break;
		case WM_EXITSIZEMOVE:
			if (win32_in_modal_move) {
				KillTimer(hWnd, X16_MOVE_TIMER_ID);
				win32_in_modal_move = false;
			}
			break;
		case WM_TIMER:
			if (win32_in_modal_move && wParam == X16_MOVE_TIMER_ID) {
				// Advance the emulator one frame and repaint from inside the
				// modal loop. video_present_no_input() renders without pumping
				// events, so there is no recursion back into this proc.
				emulator_step_during_move();
				return 0; // handled; don't hand our private timer to SDL
			}
			break;
	}
	if (video_win32_orig_wndproc) {
		return CallWindowProcW(video_win32_orig_wndproc, hWnd, message, wParam, lParam);
	}
	return DefWindowProcW(hWnd, message, wParam, lParam);
}

static HWND
video_win32_hwnd(SDL_Window *window)
{
	SDL_SysWMinfo wmInfo;
	SDL_VERSION(&wmInfo.version);
	if (!SDL_GetWindowWMInfo(window, &wmInfo)) {
		return NULL;
	}
	return wmInfo.info.win.window;
}

void video_win32_install_move_hook(SDL_Window *window)
{
	HWND hwnd = video_win32_hwnd(window);
	if (hwnd == NULL) {
		return;
	}
	// Don't double-subclass the same window (our proc would chain to itself).
	// All SDL windows share the same WIN_WindowProc, so one saved original is
	// enough for chaining regardless of how many windows we subclass.
	WNDPROC prev = (WNDPROC)(LONG_PTR)GetWindowLongPtrW(hwnd, GWLP_WNDPROC);
	if (prev == video_win32_subclass_proc) {
		return; // already installed on this window
	}
	SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)video_win32_subclass_proc);
	if (video_win32_orig_wndproc == NULL) {
		video_win32_orig_wndproc = prev;
	}
}

void video_win32_remove_move_hook(SDL_Window *window)
{
	if (video_win32_orig_wndproc == NULL) {
		return;
	}
	HWND hwnd = video_win32_hwnd(window);
	if (hwnd == NULL) {
		return;
	}
	WNDPROC cur = (WNDPROC)(LONG_PTR)GetWindowLongPtrW(hwnd, GWLP_WNDPROC);
	if (cur == video_win32_subclass_proc) {
		SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)video_win32_orig_wndproc);
	}
}

