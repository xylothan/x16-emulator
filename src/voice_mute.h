// Commander X16 Emulator
// All rights reserved. License: 2-clause BSD

#pragma once

// The debug clients that can mute a voice on the host output. Each holds its
// own mask per chip, ORed with the debugger UI's selection, and loses it when
// its session ends, so a client that disappears cannot leave a voice silent.
typedef enum {
	VOICE_MUTE_DAP = 0,
	VOICE_MUTE_MCP,
	VOICE_MUTE_OWNERS,
} voice_mute_owner_t;
