// Checks for the host key's bookkeeping (src/hostkey.c).

#include "hostkey.h"
#include "support/harness.h"

#define HOST   71  // Scroll Lock
#define F3     60
#define KEY_W  26
#define KEY_3  32
#define LCTRL  224
#define ESC    HOSTKEY_ESCAPE

static hostkey_action
press(int sc)
{
	return hostkey_event(true, sc, false);
}

static hostkey_action
release(int sc)
{
	return hostkey_event(false, sc, false);
}

int
main(void)
{
	hostkey_set(HOST); // the default on macOS is Right Command
	check(hostkey_scancode() == HOST, "Scroll Lock is the host key");

	// ── Without the host key everything is the guest's ─────────────────────
	check(press(LCTRL) == HOSTKEY_PASS && press(KEY_3) == HOSTKEY_PASS, "Ctrl+3 goes to the guest");
	check(release(KEY_3) == HOSTKEY_PASS && release(LCTRL) == HOSTKEY_PASS, "and so do their releases");

	// ── Held: one chord ─────────────────────────────────────────────────────
	check(press(HOST) == HOSTKEY_EAT, "the host key never reaches the guest");
	check(hostkey_held(), "it is held");
	check(press(F3) == HOSTKEY_COMMAND, "Host+F3 is a command");
	check(hostkey_event(true, F3, true) == HOSTKEY_EAT, "its auto-repeat is not another command");
	check(release(F3) == HOSTKEY_EAT, "its release does not reach the guest");
	check(release(HOST) == HOSTKEY_EAT, "nor does the host key's");
	check(!hostkey_held() && !hostkey_latched(), "a chord does not latch");
	check(press(F3) == HOSTKEY_PASS && release(F3) == HOSTKEY_PASS, "afterwards F3 is the guest's again");

	// ── Released in the other order ─────────────────────────────────────────
	press(HOST);
	check(press(F3) == HOSTKEY_COMMAND, "a command");
	release(HOST);
	check(hostkey_event(true, F3, true) == HOSTKEY_EAT, "still held after the host key is let go: not the guest's");
	check(release(F3) == HOSTKEY_EAT, "and its release is still ours");
	check(!hostkey_latched(), "and nothing latched");

	// ── A key the guest already has down ────────────────────────────────────
	check(press(KEY_W) == HOSTKEY_PASS, "W down, to the guest");
	press(HOST);
	check(hostkey_event(true, KEY_W, true) == HOSTKEY_EAT, "its auto-repeat while the host key is held is dropped");
	release(HOST);
	check(hostkey_latched(), "(nothing else was pressed, so that was a tap and latched)");
	check(release(KEY_W) == HOSTKEY_PASS, "but its release reaches the guest: no stuck key");
	press(HOST);
	release(HOST);
	check(!hostkey_latched(), "a second tap unlatches");

	// ── Latched ─────────────────────────────────────────────────────────────
	press(HOST);
	release(HOST);
	check(hostkey_latched() && !hostkey_held(), "a tap latches");
	check(press(KEY_3) == HOSTKEY_COMMAND && release(KEY_3) == HOSTKEY_EAT, "3 is a command");
	check(press(F3) == HOSTKEY_COMMAND && release(F3) == HOSTKEY_EAT, "and so is F3, still latched");
	press(HOST);
	check(press(F3) == HOSTKEY_COMMAND, "a chord while latched is a command too");
	release(F3);
	release(HOST);
	check(hostkey_latched(), "and leaves it latched");
	check(press(ESC) == HOSTKEY_EAT && !hostkey_latched(), "Esc unlatches, and is not a command");
	check(release(ESC) == HOSTKEY_EAT, "and its release is not the guest's either");
	check(press(ESC) == HOSTKEY_PASS && release(ESC) == HOSTKEY_PASS, "after that Esc is the guest's");

	press(HOST);
	release(HOST);
	hostkey_unlatch();
	check(!hostkey_latched(), "unlatched from outside");

	// ── Another key ─────────────────────────────────────────────────────────
	hostkey_set(230); // Right Alt
	check(hostkey_scancode() == 230, "the host key can be changed");
	check(press(HOST) == HOSTKEY_PASS && release(HOST) == HOSTKEY_PASS, "Scroll Lock is then the guest's");
	check(press(230) == HOSTKEY_EAT && press(F3) == HOSTKEY_COMMAND, "and the new one works");
	release(F3);
	release(230);
	hostkey_set(0);
	check(hostkey_scancode() == 230, "no key at all is refused");
	check(hostkey_event(true, 9999, false) == HOSTKEY_PASS, "an out-of-range scancode is passed on");

	return x16_test_summary("hostkey");
}
