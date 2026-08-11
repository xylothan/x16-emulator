// Commander X16 Emulator
// Copyright (c) 2019,2022 Michael Steil
// All rights reserved. License: 2-clause BSD

// Feature-test macros for glibc-style libcs only. macOS hides BSD extensions
// this code uses when they are set, and MSVC's CRT ignores them entirely.
#if !defined(__APPLE__) && !defined(_MSC_VER)
#define _XOPEN_SOURCE   600
#define _POSIX_C_SOURCE 1
#endif
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "compat.h"
#include <limits.h>
#ifdef __MINGW32__
#include <ctype.h>
#endif
#include "cpu/fake6502.h"
#include "timing.h"
#include "disasm.h"
#include "files.h"
#include "memory.h"
#include "video.h"
#include "via.h"
#include "serial.h"
#include "i2c.h"
#include "rtc.h"
#include "smc.h"
#include "vera_spi.h"
#include "sdcard.h"
#include "ieee.h"
#include "glue.h"
#include "debugger.h"
#include "dbg_info.h"
#include "dbg_load.h"
#include "debug_server.h"
#include "source_view.h"
#include "code_map.h"
#include "utf8.h"
#include "iso_8859_15.h"
#include "joystick.h"
#include "rom_symbols.h"
#include "ymglue.h"
#include "audio.h"
#include "version.h"
#include "wav_recorder.h"
#include "testbench.h"
#include "cartridge.h"
#include "midi.h"
#include "git_rev.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <pthread.h>
#endif

void *emulator_loop(void *param);
void emscripten_main_loop(void);

// This must match the KERNAL's set!
char *keymaps[] = {
	"en-us",
	"en-us-int",
	"en-gb",
	"sv",
	"de",
	"da",
	"it",
	"pl",
	"nb",
	"hu",
	"es",
	"fi",
	"pt-br",
	"cz",
	"jp",
	"fr",
	"de-ch",
	"en-us-dvo",
	"et",
	"fr-be",
	"fr-ca",
	"is",
	"pt",
	"hr",
	"sk",
	"sl",
	"lv",
	"lt",
};

#ifdef PERFSTAT
uint32_t stat[65536];
#endif

bool debugger_enabled = false;
// True when a local interactive debugger was asked for (-debug/-bp/-wp), as
// distinct from the machinery merely being switched on. A DAP client uses this
// to decide whether anyone is left to resume the machine if it disconnects.
bool debug_window_enabled = false;

// -imgui: the graphical debugger window. Independent of debugger_enabled, so
// the two UIs can be used together or separately. Always defined, even in a
// build without the UI, so the emulator loop needs no #ifdef.
bool imgui_debugger_enabled = false;
char *paste_text = NULL;
char *clipboard_buffer = NULL;
char paste_text_data[65536];
bool pasting_bas = false;

uint16_t num_banks = 1;
uint16_t num_ram_banks = 64; // 512 KB default

bool log_video = false;
bool log_speed = false;
bool log_keyboard = false;
bool dump_cpu = false;
bool dump_ram = true;
bool dump_bank = true;
bool dump_vram = false;
bool warp_mode = false;
bool warp_pastes = false;
bool grab_mouse = false;
echo_mode_t echo_mode;
bool save_on_exit = true;
bool disable_emu_cmd_keys = false;
bool set_system_time = false;
bool has_serial = false;
bool no_ieee_intercept = false;
bool has_via2 = false;
gif_recorder_state_t record_gif = RECORD_GIF_DISABLED;
char *gif_path = NULL;
char *wav_path = NULL;
uint8_t *fsroot_path = NULL;
uint8_t *startin_path = NULL;
uint8_t keymap = 0; // KERNAL's default
int window_scale = 1;
float screen_x_scale = 1.0;
float window_opacity = 1.0;
char *scale_quality = "best";
bool test_init_complete=false;
bool headless = false;
bool fullscreen = false;
bool testbench = false;
bool enable_midline = false;
bool ym2151_irq_support = false;
char *cartridge_path = NULL;

bool has_midi_card = false;
uint16_t midi_card_addr;

bool using_hostfs = true;

// Per-instruction clock reference. File-scope so emulator_step_during_move()
// (run from the Windows modal move/resize loop) advances the peripheral clocks
// in lock-step with emulator_loop() and neither double-counts the other's ticks.
static uint32_t old_clockticks6502 = 0;

uint8_t MHZ = 8;

#ifdef TRACE
bool trace_mode = false;
uint16_t trace_address = 0;
#endif

int instruction_counter;
SDL_RWops *prg_file;
bool prg_finished_loading;
int prg_override_start = -1;
bool run_after_load = false;

char *nvram_path = NULL;

bool pwr_long_press=false;
bool is_gen2 = false;

#ifdef TRACE
#include "rom_labels.h"
#include "rom_lst.h"
char *
label_for_address(uint16_t address)
{
	uint16_t *addresses;
	char **labels;
	int count;
	switch (memory_get_rom_bank()) {
		case 0:
			addresses = addresses_bank0;
			labels = labels_bank0;
			count = sizeof(addresses_bank0) / sizeof(uint16_t);
			break;
		case 1:
			addresses = addresses_bank1;
			labels = labels_bank1;
			count = sizeof(addresses_bank1) / sizeof(uint16_t);
			break;
		case 2:
			addresses = addresses_bank2;
			labels = labels_bank2;
			count = sizeof(addresses_bank2) / sizeof(uint16_t);
			break;
		case 3:
			addresses = addresses_bank3;
			labels = labels_bank3;
			count = sizeof(addresses_bank3) / sizeof(uint16_t);
			break;
		case 4:
			addresses = addresses_bank4;
			labels = labels_bank4;
			count = sizeof(addresses_bank4) / sizeof(uint16_t);
			break;
		case 5:
			addresses = addresses_bank5;
			labels = labels_bank5;
			count = sizeof(addresses_bank5) / sizeof(uint16_t);
			break;
		case 6:
			addresses = addresses_bank6;
			labels = labels_bank6;
			count = sizeof(addresses_bank6) / sizeof(uint16_t);
			break;
		case 10:
			addresses = addresses_bankA;
			labels = labels_bankA;
			count = sizeof(addresses_bankA) / sizeof(uint16_t);
			break;
		case 11:
			addresses = addresses_bankB;
			labels = labels_bankB;
			count = sizeof(addresses_bankB) / sizeof(uint16_t);
			break;
		case 12:
			addresses = addresses_bankC;
			labels = labels_bankC;
			count = sizeof(addresses_bankC) / sizeof(uint16_t);
			break;
		case 13:
			addresses = addresses_bankD;
			labels = labels_bankD;
			count = sizeof(addresses_bankD) / sizeof(uint16_t);
			break;
		case 14:
			addresses = addresses_bankE;
			labels = labels_bankE;
			count = sizeof(addresses_bankE) / sizeof(uint16_t);
			break;
		case 15:
			addresses = addresses_bankF;
			labels = labels_bankF;
			count = sizeof(addresses_bankF) / sizeof(uint16_t);
			break;
		default:
			addresses = NULL;
			labels = NULL;
	}

	if (!addresses) {
		return NULL;
	}

	for (int i = 0; i < count; i++) {
		if (address == addresses[i]) {
			return labels[i];
		}
	}
	return NULL;
}

char *
lst_for_address(uint16_t address)
{
	if (address < 0xc000) {
		return NULL;
	}

	char **lst;
	switch (memory_get_rom_bank()) {
		case 0: lst = lst_bank0; break;
		case 2: lst = lst_bank2; break;
		case 3: lst = lst_bank3; break;
		case 4: lst = lst_bank4; break;
		case 5: lst = lst_bank5; break;
		case 7: lst = lst_bank7; break;
		case 8: lst = lst_bank8; break;
		case 9: lst = lst_bank9; break;
		case 10: lst = lst_bankA; break;
		case 11: lst = lst_bankB; break;
		case 12: lst = lst_bankC; break;
		case 13: lst = lst_bankD; break;
		case 14: lst = lst_bankE; break;
		case 15: lst = lst_bankF; break;
		default:
			return NULL;
	}
	return lst[address - 0xc000];
}
#endif

void
machine_dump(const char* reason)
{
	printf("Dumping system memory. Reason: %s\n", reason);
	int index = 0;
	char filename[22];
	for (;;) {
		if (!index) {
			strcpy(filename, "dump.bin");
		} else {
			sprintf(filename, "dump-%i.bin", index);
		}
		if (access(filename, F_OK) == -1) {
			break;
		}
		index++;
	}
	SDL_RWops *f = SDL_RWFromFile(filename, "wb");
	if (!f) {
		printf("Cannot write to %s!\n", filename);
		return;
	}

	if (dump_cpu) {
		SDL_RWwrite(f, &regs.a, sizeof(uint8_t), 1);
		SDL_RWwrite(f, &regs.xl, sizeof(uint8_t), 1);
		SDL_RWwrite(f, &regs.yl, sizeof(uint8_t), 1);
		SDL_RWwrite(f, &regs.sp, sizeof(uint8_t), 1);
		SDL_RWwrite(f, &regs.status, sizeof(uint8_t), 1);
		SDL_RWwrite(f, &regs.pc, sizeof(uint16_t), 1);
	}
	memory_save(f, dump_ram, dump_bank);

	if (dump_vram) {
		video_save(f);
	}

	SDL_RWclose(f);
	printf("Dumped system to %s.\n", filename);
}

void mouse_state_init(void)
{
	kernal_mouse_enabled = 0;
	SDL_ShowCursor((mouse_grabbed || kernal_mouse_enabled) ? SDL_DISABLE : SDL_ENABLE);
}

void
machine_reset()
{
	i2c_reset_state();
	ieee_init();
	memory_reset();
	vera_spi_init();
	via1_init();
	if (has_via2) {
		via2_init();
	}
	video_reset();
	mouse_state_init();
	reset6502(regs.is65c816);
	midi_serial_init();
	code_map_reset();  // discard stale disassembly anchors so post-reset code re-aligns
	dbg_load_reset();  // and any load that was mid-flight
}

void
machine_nmi()
{
	nmi6502();
}

void
machine_paste(char *s, bool handle_free)
{
	if (s) {
		paste_text = s;
		if (handle_free) {
			clipboard_buffer = s; // so that we can free this later
		}
		pasting_bas = true;
		if (warp_pastes) machine_set_warp(true);
	}
}

// Every warp change re-bases the clock. Leaving warp with a surplus already
// accrued makes the throttle sleep it off, which at a slow target is seconds of
// frozen machine; entering warp with a deficit makes it sprint. The paste paths
// used to set the flag directly and did both.
void
machine_set_warp(bool on)
{
	if (warp_mode == on) {
		return;
	}
	warp_mode = on;
	timing_init();
}

void
machine_toggle_warp()
{
	machine_set_warp(!warp_mode);
}


static bool
is_kernal()
{
	// only for KERNAL
	return (debug_read6502(0xfff6, 0, USE_CURRENT_X16_BANK) == 'M' &&
			debug_read6502(0xfff7, 0, USE_CURRENT_X16_BANK) == 'I' &&
			debug_read6502(0xfff8, 0, USE_CURRENT_X16_BANK) == 'S' &&
			debug_read6502(0xfff9, 0, USE_CURRENT_X16_BANK) == 'T')
		|| (debug_read6502(0xc008, 0, USE_CURRENT_X16_BANK) == 'M' &&
			debug_read6502(0xc009, 0, USE_CURRENT_X16_BANK) == 'I' &&
			debug_read6502(0xc00a, 0, USE_CURRENT_X16_BANK) == 'S' &&
			debug_read6502(0xc00b, 0, USE_CURRENT_X16_BANK) == 'T');
}

// A breakpoint or watch address as written on the command line: a plain
// address, or a banked one as bb:aaaa. The bare bbaaaa form upstream accepted
// for -debug still works.
//
// An address written on its own carries no bank, so it applies to whichever
// bank is mapped -- writing "A000" says nothing about banks, and should not
// silently mean bank 0. Write "05:A000" to mean one particular bank.
// Returns false (and complains) rather than silently arming address 0 for
// input that is not a valid address, which is what a bare strtol() would do
// with a typo.
static bool
parse_breakpoint_arg(const char *arg, struct breakpoint *out)
{
	char         *end = NULL;
	unsigned long first = strtoul(arg, &end, 16);

	if (end == arg) {
		fprintf(stderr, "Not a hex address: %s\n", arg);
		return false;
	}

	if (*end == ':') {
		const char   *addr_str = end + 1;
		char         *end2     = NULL;
		unsigned long addr     = strtoul(addr_str, &end2, 16);
		if (end2 == addr_str || *end2 != '\0' || addr > 0xFFFF) {
			fprintf(stderr, "Not a valid banked address (expected bb:aaaa): %s\n", arg);
			return false;
		}
		if (first > 0xFF) {
			fprintf(stderr, "Bank out of range (00-FF): %s\n", arg);
			return false;
		}
		if (addr < 0xA000) {
			fprintf(stderr, "$%04lX is not in a banked window, so a bank is meaningless: %s\n",
			        addr, arg);
			return false;
		}
		out->pc      = (int)addr;
		out->bank    = 0;
		out->x16Bank = (int)first;
		return true;
	}

	if (*end != '\0') {
		fprintf(stderr, "Not a hex address: %s\n", arg);
		return false;
	}

	if (first > 0xFFFFFF) {
		fprintf(stderr, "Address out of range: %s\n", arg);
		return false;
	}

	out->bank = 0;
	if (first > 0xFFFF) {
		out->pc      = (int)(first & 0xFFFF);
		out->x16Bank = (int)(first >> 16);
	} else {
		out->pc      = (int)first;
		out->x16Bank = DEBUG_BANK_ANY;   // no bank given: whichever is mapped
	}
	return true;
}

static void
usage()
{
	printf("\nCommander X16 Emulator r%s (%s)", VER, VER_NAME);
#ifdef GIT_REV
	printf(", "GIT_REV);
#endif
	printf("\n(C)2019, 2023 Michael Steil et al.\n");
	printf("All rights reserved. License: 2-clause BSD\n\n");
	printf("Usage: x16emu [option] ...\n\n");
	printf("-rom <rom.bin>\n");
	printf("\tOverride KERNAL/BASIC/* ROM file.\n");
	printf("-ram <ramsize>\n");
	printf("\tSpecify banked RAM size in KB (8, 16, 32, ..., 2048).\n");
	printf("\tThe default is 512.\n");
	printf("-nvram <nvram.bin>\n");
	printf("\tSpecify NVRAM image. By default, the machine starts with\n");
	printf("\tempty NVRAM and does not save it to disk.\n");
	printf("-keymap <keymap>\n");
	printf("\tEnable a specific keyboard layout decode table.\n");
	printf("-sdcard <sdcard.img>\n");
	printf("\tSpecify SD card image (partition map + FAT32)\n");
	printf("-cart <crtfile.crt>\n");
	printf("\tLoads a specially-formatted cartridge file.\n");
	printf("-cartbin <romfile.bin>\n");
	printf("\tLoads a raw cartridge file starting at ROM bank 32. After\n");
	printf("\tloading, all of the affected banks will function as RAM.\n");
	printf("-serial\n");
	printf("\tConnect host fs through Serial Bus [experimental]\n");
	printf("-nohostieee / -nohostfs\n");
	printf("\tDisable HostFS through IEEE API interception.\n");
	printf("\tIEEE API HostFS is normally enabled unless -sdcard or\n");
	printf("\t-serial is specified.\n");
	printf("-hostfsdev <unit>\n");
	printf("\tSet the HostFS IEEE device number. Range 8-31. Default: %d.\n", ieee_unit);
	printf("-fsroot <directory>\n");
	printf("\tSpecify the host filesystem directory path which is to\n");
	printf("\tact as the emulated root directory of the Commander X16.\n");
	printf("\tDefault is the current working directory.\n");
	printf("-startin <directory>\n");
	printf("\tSpecify the host filesystem directory path that the\n");
	printf("\temulated filesystem starts in. Default is the current\n");
	printf("\tworking directory if it lies within the hierarchy of fsroot,\n");
	printf("\totherwise it defaults to fsroot itself.\n");
	printf("-noemucmdkeys\n");
	printf("\tDisable emulator command keys.\n");
	printf("-capture\n");
	printf("\tStart emulator with mouse/keyboard captured.\n");
	printf("-nokeyboardcapture\n");
	printf("\tWhile in capture mode, causes the emulator not to intercept\n");
	printf("\tkeyboard combinations which are used by the operating system,\n");
	printf("\tsuch as Alt+Tab.\n");
	printf("-prg <app.prg>[,<load_addr>]\n");
	printf("\tLoad application from the *host filesystem* into RAM,\n");
	printf("\teven if an SD card is attached.\n");
	printf("\tThe override load address is hex without a prefix.\n");
	printf("-bas <app.txt>\n");
	printf("\tInject a BASIC program in ASCII encoding through the\n");
	printf("\tkeyboard.\n");
	printf("-run\n");
	printf("\tStart the -prg/-bas program using RUN\n");
	printf("-warp\n");
	printf("\tEnable warp mode, run emulator as fast as possible.\n");
	printf("-pastewarp\n");
	printf("\tEnable warp mode during pastes and during loading via -bas.\n");
	printf("-echo [{iso|raw}]\n");
	printf("\tPrint all KERNAL output to the host's stdout.\n");
	printf("\tBy default, everything but printable ASCII characters get\n");
	printf("\tescaped. \"iso\" will escape everything but non-printable\n");
	printf("\tISO-8859-15 characters and convert the output to UTF-8.\n");
	printf("\t\"raw\" will not do any substitutions.\n");
	printf("\tWith the BASIC statement \"LIST\", this can be used\n");
	printf("\tto detokenize a BASIC program.\n");
	printf("-log {K|S|V}...\n");
	printf("\tEnable logging of (K)eyboard, (S)peed, (V)ideo.\n");
	printf("\tMultiple characters are possible, e.g. -log KS\n");
	printf("-gif <file.gif>[,wait]\n");
	printf("\tRecord a gif for the video output.\n");
	printf("\tUse ,wait to start paused.\n");
	printf("\tPOKE $9FB5,2 to start recording.\n");
	printf("\tPOKE $9FB5,1 to capture a single frame.\n");
	printf("\tPOKE $9FB5,0 to pause.\n");
	printf("-wav <file.wav>[{,wait|,auto}]\n");
	printf("\tRecord a wav for the audio output.\n");
	printf("\tUse ,wait to start paused, or ,auto to start paused and automatically begin recording on the first non-zero audio signal.\n");
	printf("\tPOKE $9FB6,2 to automatically begin recording on the first non-zero audio signal.\n");
	printf("\tPOKE $9FB6,1 to begin recording immediately.\n");
	printf("\tPOKE $9FB6,0 to pause.\n");
	printf("-scale {1|2|3|4}\n");
	printf("\tScale output to an integer multiple of 640x480\n");
	printf("-quality {nearest|linear|best}\n");
	printf("\tScaling algorithm quality\n");
	printf("-widescreen\n");
	printf("\tStretch output to 16:9 resolution to mimic display of a widescreen monitor.\n");
	printf("-fullscreen\n");
	printf("\tStart up in fullscreen mode instead of in a window.\n");
	printf("-opacity (0.0,...,1.0)\n");
	printf("\tSet the opacity value (0.0 for transparent, 1.0 for opaque) of the window. (default: %.1f)\n", window_opacity);
	printf("-debug [<address>]\n");
	printf("\tEnable debugger. Optionally, set a breakpoint\n");
#ifdef HAS_IMGUI
	printf("-imgui\n");
	printf("\tOpen the graphical debugger in a second window: memory,\n");
	printf("\tdisassembly, CPU, VERA, source, breakpoints, symbols and call\n");
	printf("\tstack. Independent of -debug, and can be combined with it.\n");
#endif
	printf("-bp <address>\n");
	printf("\tSet a breakpoint before the machine starts. Unlike -debug's optional\n");
	printf("\taddress this can be repeated, so several breakpoints can be armed\n");
	printf("\tup front.\n");
	printf("\tA breakpoint needs a debugger to resume from, so this opens -debug\n");
	printf("\tunless -imgui or -debugport is already providing one.\n");
	printf("\tAn address on its own applies whatever bank is mapped. To pin one\n");
	printf("\tbank of the $A000-$FFFF windows, write bb:aaaa, e.g. 05:A000 for\n");
	printf("\t$A000 in RAM bank 5. Below $A000 nothing is banked, so no bank may\n");
	printf("\tbe given.\n");
	printf("-wp <address>[,<length>]\n");
	printf("\tWatch memory: stop when the program writes to this address, or\n");
	printf("\tanywhere in <length> bytes from it. Takes the same address forms as\n");
	printf("\t-bp, can be repeated, and picks a debugger the same way.\n");
	printf("-dbgauto / -no-dbgauto\n");
	printf("\tPick up debug info for programs the running machine loads, not\n");
	printf("\tjust the one named by -dbgfile: when it LOADs a host file, read\n");
	printf("\tthe .dbg sitting beside it. On whenever the debugger is enabled.\n");
	printf("\tUse -no-dbgauto to suppress it: the file read is chosen by the\n");
	printf("\temulated program, and a .dbg can name source paths anywhere on\n");
	printf("\tthe host, so untrusted software is better run without it.\n");
	printf("-dbgfile <path>\n");
	printf("\tLoad a cc65 .dbg file, so addresses can be mapped back to source\n");
	printf("\tfiles and line numbers. Combine with -srcpath if the sources are not\n");
	printf("\tbeside the .dbg file.\n");
	printf("-srcpath <dir>\n");
	printf("\tAdd a directory to search for the source files a .dbg file names.\n");
	printf("\tCan be repeated.\n");
	printf("-debugport [port]\n");
	printf("\tServe the Debug Adapter Protocol on <port> (default %d), so an\n",
	       DEBUG_SERVER_DEFAULT_PORT);
	printf("\teditor can set breakpoints and step through source. The emulator\n");
	printf("\tkeeps running until a client sets a breakpoint that hits.\n");
	printf("-randram\n");
	printf("\t(deprecated, no effect)\n");
	printf("-zeroram\n");
	printf("\tSet all RAM to zero instead of uninitialized random values\n");
	printf("-wuninit\n");
	printf("\tPrints warning to stdout if uninitialized RAM is accessed\n");
	printf("-memorystats <file.txt>\n");
	printf("\tSaves memory access statistics to the given file when emulator exits\n");
	printf("-dump {C|R|B|V}...\n");
	printf("\tConfigure system dump: (C)PU, (R)AM, (B)anked-RAM, (V)RAM\n");
	printf("\tMultiple characters are possible, e.g. -dump CV ; Default: RB\n");
	printf("-joy1\n");
	printf("\tEnable binding a gamepad to SNES controller port 1\n");
	printf("-joy2\n");
	printf("\tEnable binding a gamepad to SNES controller port 2\n");
	printf("-joy3\n");
	printf("\tEnable binding a gamepad to SNES controller port 3\n");
	printf("-joy4\n");
	printf("\tEnable binding a gamepad to SNES controller port 4\n");
	printf("-sound <output device>\n");
	printf("\tSet the output device used for audio emulation\n");
	printf("\tIf output device is 'none', no audio is generated\n");
	printf("-abufs <number of audio buffers>\n");
	printf("\tSet the number of audio buffers used for playback.\n");
	printf("\tIf using HostFS, the default is 32, otherwise 8.\n");
	printf("\tIncreasing this will reduce stutter on slower computers,\n");
	printf("\tbut will increase audio latency.\n");
	printf("-rtc\n");
	printf("\tSet the real-time-clock to the current system time and date.\n");
	printf("-via2\n");
	printf("\tInstall the second VIA chip expansion at $9F10\n");
	printf("-testbench\n");
	printf("\tHeadless mode for unit testing with an external test runner\n");
	printf("-mhz <integer>\n");
	printf("\tRun the emulator with a system clock speed other than the default of\n");
	printf("\t8 MHz. Valid values are in the range of 1-40, inclusive. This option\n");
	printf("\tis meant mainly for benchmarking, and may not reflect accurate\n");
	printf("\thardware behavior.\n");
	printf("-midline-effects\n");
	printf("\tApproximate mid-line raster effects when changing tile, sprite,\n");
	printf("\tand palette data. Requires a fast host CPU.\n");
	printf("-enable-ym2151-irq\n");
	printf("\tConnect the YM2151 IRQ source to the emulated CPU. This option increases\n");
	printf("\tCPU usage as audio render is triggered for every CPU instruction.\n");
	printf("-c02\n");
	printf("\tRun the emulator under an emulated 65C02 (default)\n");
	printf("-c816\n");
	printf("\tRun the emulator under an emulated 65C816\n");
	printf("\tThis option is experimental.\n");
	printf("-rockwell\n");
	printf("\tSuppress warning emitted when encountering a Rockwell extension on the 65C02\n");
	printf("-longpwron\n");
	printf("\tSimulate a long press of the power button at system power-on.\n");
#ifdef HAS_FLUIDSYNTH
	printf("-midicard [<address>]\n");
	printf("\tInstall a serial MIDI card at the specified address, or at $9F60 by default.\n");
	printf("\tThe -sf2 option must be specified along with this option.\n");
	printf("-sf2 <SoundFont filename>\n");
	printf("\tInitialize MIDI synth with the specified SoundFont.\n");
	printf("\tThe -midicard option must be specified along with this option.\n");
	printf("-midi-in\n");
	printf("\tConnect the system MIDI input devices to the input of the first UART\n");
	printf("\tof the emulated MIDI card. The -midicard option is required for this\n");
	printf("\toption to have any effect.\n");
#endif
#ifdef TRACE
	printf("-trace [<address>]\n");
	printf("\tPrint instruction trace. Optionally, a trigger address\n");
	printf("\tcan be specified.\n");
#endif
	printf("-version\n");
	printf("\tPrint additional version information of the emulator and ROM.\n");
	printf("\n");
	exit(1);
}

void
usage_keymap()
{
	printf("The following keymaps are supported:\n");
	for (int i = 0; i < sizeof(keymaps)/sizeof(*keymaps); i++) {
		printf("\t%s\n", keymaps[i]);
	}
	exit(1);
}

void no_fluidsynth_warning(void)
{
	static bool already_warned;

	if (!already_warned) {
		fprintf(stderr, "\nWarning: x16emu was built without FluidSynth support,\n");
		fprintf(stderr, "so the MIDI synth will be inoperative.\n\n");
#if defined(__linux__)
		fprintf(stderr, "To build x16emu with fluidsynth support, you distro may\n");
		fprintf(stderr, "have a libfluidsynth-dev or fluidsynth-devel package that\n");
		fprintf(stderr, "needs to be installed before building x16emu.\n\n");
#elif defined(__APPLE__)
		fprintf(stderr, "To build x16emu with fluidsynth support,\n");
		fprintf(stderr, "install the homebrew package fluid-synth before\n");
		fprintf(stderr, "building x16emu.\n\n");
#elif defined(_WIN64)
		fprintf(stderr, "To build x16emu with fluidsynth support under MSYS2,\n");
		fprintf(stderr, "install the mingw-w64-x86_64-fluidsynth package before\n");
		fprintf(stderr, "building x16emu.\n\n");
#elif defined(_WIN32)
		fprintf(stderr, "To build x16emu with fluidsynth support under MSYS2,\n");
		fprintf(stderr, "install the mingw-w64-i686-fluidsynth package before\n");
		fprintf(stderr, "building x16emu.\n\n");
#endif
		fprintf(stderr, "Then rebuild x16emu from scratch.\n");
		already_warned = true;
	}
}

int
main(int argc, char **argv)
{
	char *rom_filename = "rom.bin";
	char rom_path_data[PATH_MAX];

	char *rom_path = rom_path_data;
	char *prg_path = NULL;
	char *bas_path = NULL;
	char *sf2_path = NULL;
	char *sdcard_path = NULL;
	bool run_test = false;
	int test_number = 0;
	int audio_buffers = 8;
	bool zeroram = false;
	bool audio_buffers_set = false;
	bool hostfs_set = false;

	const char *audio_dev_name = NULL;

	run_after_load = false;

	char *base_path = SDL_GetBasePath();

	// This causes the emulator to load ROM data from the executable's directory when
	// no ROM file is specified on the command line.
	memcpy(rom_path, base_path, strlen(base_path) + 1);
	strncpy(rom_path + strlen(rom_path), rom_filename, PATH_MAX - strlen(rom_path));
	memory_randomize_ram(true);

	argc--;
	argv++;

	// Unset until an explicit flag says otherwise, so the default below can
	// follow the debugger rather than the order of the arguments.
	int dbg_auto_opt = -1;

	// Whether -bp or -wp asked for something to be armed before the machine
	// starts. Which front end ends up driving it is decided after parsing, so
	// the answer does not depend on the order the flags were written in.
	bool cli_stops_requested = false;

	while (argc > 0) {
		if (!strcmp(argv[0], "-rom")) {
			argc--;
			argv++;
			if (!argc || argv[0][0] == '-') {
				usage();
			}
			rom_path = argv[0];
			argc--;
			argv++;
		} else if (!strcmp(argv[0], "-ram")) {
			argc--;
			argv++;
			if (!argc || argv[0][0] == '-') {
				usage();
			}
			int kb = atoi(argv[0]);
			if (!((kb & 7)==0) || kb < 8 || kb > 2048) {
				printf("-ram value must be a multiple of 8 in the range of 8-2048.\n");
				exit(1);
			}
			num_ram_banks = kb /8;
			argc--;
			argv++;
		} else if (!strcmp(argv[0], "-keymap")) {
			argc--;
			argv++;
			if (!argc || argv[0][0] == '-') {
				usage_keymap();
			}
			bool found = false;
			for (int i = 0; i < sizeof(keymaps)/sizeof(*keymaps); i++) {
				if (!strcmp(argv[0], keymaps[i])) {
					found = true;
					keymap = i;
				}
			}
			if (!found) {
				usage_keymap();
			}
			argc--;
			argv++;
		} else if (!strcmp(argv[0], "-prg")) {
			argc--;
			argv++;
			if (!argc || argv[0][0] == '-') {
				usage();
			}
			prg_path = argv[0];
			argc--;
			argv++;
		} else if (!strcmp(argv[0], "-midicard")) {
#ifndef HAS_FLUIDSYNTH
			no_fluidsynth_warning();
#endif
			argc--;
			argv++;
			has_midi_card = true;
			if (argc && argv[0][0] != '-') {
				midi_card_addr = 0x9f00 | ((uint16_t)strtol(argv[0], NULL, 16) & 0xff);
				midi_card_addr &= 0xfff0;
				argc--;
				argv++;
			} else {
				midi_card_addr = 0x9f60;
			}
		} else if (!strcmp(argv[0], "-sf2")) {
#ifndef HAS_FLUIDSYNTH
			no_fluidsynth_warning();
#endif
			argc--;
			argv++;
			if (!argc || argv[0][0] == '-') {
				usage();
			}
			sf2_path = argv[0];
			argc--;
			argv++;
		} else if (!strcmp(argv[0], "-midi-in")) {
#ifndef HAS_FLUIDSYNTH
			no_fluidsynth_warning();
#endif
			argc--;
			argv++;
			fs_midi_in_connect = true;
		} else if (!strcmp(argv[0], "-run")) {
			argc--;
			argv++;
			run_after_load = true;
		} else if (!strcmp(argv[0], "-bas")) {
			argc--;
			argv++;
			if (!argc || argv[0][0] == '-') {
				usage();
			}
			bas_path = argv[0];
			argc--;
			argv++;
		} else if (!strcmp(argv[0], "-test")) {
			argc--;
			argv++;
			if (!argc || argv[0][0] == '-') {
				usage();
			}
			test_number = atoi(argv[0]);
			run_test = true;
			argc--;
			argv++;
		} else if (!strcmp(argv[0], "-nvram")) {
			argc--;
			argv++;
			if (!argc || argv[0][0] == '-') {
				usage();
			}
			nvram_path = argv[0];
			argc--;
			argv++;
		} else if (!strcmp(argv[0], "-sdcard")) {
			argc--;
			argv++;
			if (!argc || argv[0][0] == '-') {
				usage();
			}
			sdcard_path = argv[0];
			argc--;
			argv++;
		} else if (!strcmp(argv[0], "-cart")) {
			argc--;
			argv++;
			if (!argc || argv[0][0] == '-') {
				usage();
			}
			cartridge_path = argv[0];
			argc--;
			argv++;
		} else if (!strcmp(argv[0], "-cartbin")) {
			argc--;
			argv++;
			if (!argc || argv[0][0] == '-') {
				usage();
			}
			cartridge_new();
			cartridge_define_bank_range(32, 255, CART_BANK_UNINITIALIZED_RAM);
			cartridge_import_files(argv, 1, 32, CART_BANK_INITIALIZED_RAM, 0);
			argc--;
			argv++;
		} else if (!strcmp(argv[0], "-warp")) {
			argc--;
			argv++;
			warp_mode = true;
		} else if (!strcmp(argv[0], "-pastewarp")) {
			argc--;
			argv++;
			warp_pastes = true;
		} else if (!strcmp(argv[0], "-echo")) {
			argc--;
			argv++;
			if (argc && argv[0][0] != '-') {
				if (!strcmp(argv[0], "raw")) {
					echo_mode = ECHO_MODE_RAW;
				} else if (!strcmp(argv[0], "iso")) {
						echo_mode = ECHO_MODE_ISO;
				} else {
					usage();
				}
				argc--;
				argv++;
			} else {
				echo_mode = ECHO_MODE_COOKED;
			}
		} else if (!strcmp(argv[0], "-log")) {
			argc--;
			argv++;
			if (!argc || argv[0][0] == '-') {
				usage();
			}
			for (char *p = argv[0]; *p; p++) {
				switch (tolower(*p)) {
					case 'k':
						log_keyboard = true;
						break;
					case 's':
						log_speed = true;
						break;
					case 'v':
						log_video = true;
						break;
					default:
						usage();
				}
			}
			argc--;
			argv++;
		} else if (!strcmp(argv[0], "-dump")) {
			argc--;
			argv++;
			if (!argc || argv[0][0] == '-') {
				usage();
			}
			dump_cpu = false;
			dump_ram = false;
			dump_bank = false;
			dump_vram = false;
			for (char *p = argv[0]; *p; p++) {
				switch (tolower(*p)) {
					case 'c':
						dump_cpu = true;
						break;
					case 'r':
						dump_ram = true;
						break;
					case 'b':
						dump_bank = true;
						break;
					case 'v':
						dump_vram = true;
						break;
					default:
						usage();
				}
			}
			argc--;
			argv++;
		} else if (!strcmp(argv[0], "-gif")) {
			argc--;
			argv++;
			// set up for recording
			record_gif = RECORD_GIF_PAUSED;
			if (!argc || argv[0][0] == '-') {
				usage();
			}
			gif_path = argv[0];
			argv++;
			argc--;
		} else if (!strcmp(argv[0], "-wav")) {
			argc--;
			argv++;
			// set up for recording
			if (!argc || argv[0][0] == '-') {
				usage();
			}
			wav_path = argv[0];
			argv++;
			argc--;
		} else if (!strcmp(argv[0], "-debug")) {
			argc--;
			argv++;
			debugger_enabled = true;
			debug_window_enabled = true;
			if (argc && argv[0][0] != '-') {
				// Adds rather than replaces, so it composes with -bp instead of
				// depending on which came first on the command line.
				struct breakpoint bp = { 0 };
				if (!parse_breakpoint_arg(argv[0], &bp)) {
					usage();
				}
				if (debug_bp_add_for(bp, DEBUG_OWNER_CLI) == DEBUG_ADD_FULL) {
					fprintf(stderr, "Out of memory adding breakpoint: %s\n", argv[0]);
					usage();
				}
				argc--;
				argv++;
			}
		} else if (!strcmp(argv[0], "-imgui")) {
			argc--;
			argv++;
#ifdef HAS_IMGUI
			imgui_debugger_enabled = true;
#else
			// Fail loudly rather than starting with a flag that silently does
			// nothing: this build was configured with -DENABLE_IMGUI=OFF.
			printf("This build has no graphical debugger; -imgui is unavailable.\n");
			exit(1);
#endif
		} else if (!strcmp(argv[0], "-bp")) {
			argc--;
			argv++;
			debugger_enabled = true;
			cli_stops_requested = true;
			if (!argc || argv[0][0] == '-') {
				usage();
			}
			{
				struct breakpoint bp = { 0 };
				if (!parse_breakpoint_arg(argv[0], &bp)) {
					usage();
				}
				if (debug_bp_add_for(bp, DEBUG_OWNER_CLI) == DEBUG_ADD_FULL) {
					fprintf(stderr, "Out of memory adding breakpoint: %s\n", argv[0]);
					usage();
				}
			}
			argc--;
			argv++;
		} else if (!strcmp(argv[0], "-wp")) {
			argc--;
			argv++;
			debugger_enabled = true;
			cli_stops_requested = true;
			if (!argc || argv[0][0] == '-') {
				usage();
			}
			{
				// address[,length] -- the length is optional and defaults to a
				// single byte.
				char spec[64];
				if (strlen(argv[0]) >= sizeof(spec)) {
					// Truncating would parse a different address than the one
					// that was typed, and say nothing about it.
					fprintf(stderr, "Not a valid watch spec: %s\n", argv[0]);
					usage();
				}
				strcpy(spec, argv[0]);

				unsigned long len   = 1;
				char         *comma = strchr(spec, ',');
				if (comma) {
					*comma = '\0';
					char *lend = NULL;
					len = strtoul(comma + 1, &lend, 0);
					if (lend == comma + 1 || *lend != '\0' || len == 0 || len > 0xFFFF) {
						fprintf(stderr, "Not a valid watch length (1-65535): %s\n", comma + 1);
						usage();
					}
				}

				struct breakpoint wp = { 0 };
				if (!parse_breakpoint_arg(spec, &wp)) {
					usage();
				}
				// A duplicate and a full table used to be one answer, so the
				// message had to guess at both. They are distinct now.
				if (debug_wp_add_for((uint16_t)wp.pc, (uint16_t)len, wp.x16Bank,
				                     DEBUG_OWNER_CLI) == DEBUG_ADD_FULL) {
					fprintf(stderr, "Could not add watchpoint (no more than %d): %s\n",
					        MAX_WATCHPOINTS, argv[0]);
					usage();
				}
			}
			argc--;
			argv++;
		} else if (!strcmp(argv[0], "-dbgauto")) {
			argc--;
			argv++;
			dbg_auto_opt = 1;
		} else if (!strcmp(argv[0], "-no-dbgauto")) {
			argc--;
			argv++;
			dbg_auto_opt = 0;
		} else if (!strcmp(argv[0], "-debugport")) {
			argc--;
			argv++;
			// The breakpoint machinery lives behind debugger_enabled, and a DAP
			// client needs it whether or not the SDL debug window is up. It is
			// deliberately NOT debug_window_enabled: there is no local UI here,
			// which is what lets the server resume the machine when its client
			// disconnects rather than leaving it halted with nobody to drive it.
			debugger_enabled = true;
			int port = DEBUG_SERVER_DEFAULT_PORT;
			if (argc && argv[0][0] != '-') {
				port = atoi(argv[0]);
				argc--;
				argv++;
			}
#ifdef HAS_DAP
			// The CPU keeps running; a client attaches when it is ready, and
			// breakpoints it sets are what stop execution.
			if (debug_server_init(port) != 0) {
				fprintf(stderr, "Failed to start debug server on port %d\n", port);
				return 1;
			}
#else
			(void)port;
			fprintf(stderr, "-debugport requires a build with the DAP server "
			                "(cJSON was not available at configure time)\n");
			return 1;
#endif
		} else if (!strcmp(argv[0], "-dbgfile")) {
			argc--;
			argv++;
			if (!argc || argv[0][0] == '-') {
				fprintf(stderr, "-dbgfile requires a file path\n");
				return 1;
			}
			if (dbg_info_load(argv[0]) != 0) {
				fprintf(stderr, "Warning: failed to load debug info from '%s'\n", argv[0]);
			} else {
				// Make the .dbg's own directory a source-search root.
				source_view_add_path(dbg_info_get_dbg_dir());
			}
			argc--;
			argv++;
		} else if (!strcmp(argv[0], "-srcpath")) {
			argc--;
			argv++;
			if (!argc || argv[0][0] == '-') {
				fprintf(stderr, "-srcpath requires a directory path\n");
				return 1;
			}
			source_view_add_path(argv[0]);
			argc--;
			argv++;
		} else if (!strcmp(argv[0], "-randram")) {
			/* this operation has no effect anymore, randomizing the Ram is now default */
			argc--;
			argv++;
		} else if (!strcmp(argv[0], "-zeroram")) {
			argc--;
			argv++;
			memory_randomize_ram(false);
			zeroram = true;
		} else if (!strcmp(argv[0], "-wuninit")) {
			argc--;
			argv++;
			memory_report_uninitialized_access(true);
		} else if (!strcmp(argv[0], "-memorystats")) {
			argc--;
			argv++;
			if (!argc || argv[0][0] == '-') {
				usage();
			}
			memory_report_usage_statistics(argv[0]);
			argv++;
			argc--;
		} else if (!strcmp(argv[0], "-joy1")) {
			argc--;
			argv++;
			Joystick_slots_enabled[0] = true;
		} else if (!strcmp(argv[0], "-joy2")){
			argc--;
			argv++;
			Joystick_slots_enabled[1] = true;
		} else if (!strcmp(argv[0], "-joy3")) {
			argc--;
			argv++;
			Joystick_slots_enabled[2] = true;
		} else if (!strcmp(argv[0], "-joy4")) {
			argc--;
			argv++;
			Joystick_slots_enabled[3] = true;
#ifdef TRACE
		} else if (!strcmp(argv[0], "-trace")) {
			argc--;
			argv++;
			if (argc && argv[0][0] != '-') {
				trace_mode = false;
				trace_address = (uint16_t)strtol(argv[0], NULL, 16);
				argc--;
				argv++;
			} else {
				trace_mode = true;
				trace_address = 0;
			}
#endif
		} else if (!strcmp(argv[0], "-scale")) {
			argc--;
			argv++;
			if(!argc || argv[0][0] == '-') {
				usage();
			}
			for(char *p = argv[0]; *p; p++) {
				switch(tolower(*p)) {
				case '1':
					window_scale = 1;
					break;
				case '2':
					window_scale = 2;
					break;
				case '3':
					window_scale = 3;
					break;
				case '4':
					window_scale = 4;
					break;
				default:
					usage();
				}
			}
			argc--;
			argv++;
		} else if (!strcmp(argv[0], "-quality")) {
			argc--;
			argv++;
			if(!argc || argv[0][0] == '-') {
				usage();
			}
			if (!strcmp(argv[0], "nearest") ||
				!strcmp(argv[0], "linear") ||
				!strcmp(argv[0], "best")) {
				scale_quality = argv[0];
			} else {
				usage();
			}
			argc--;
			argv++;
		} else if (!strcmp(argv[0], "-widescreen")) {
			argc--;
			argv++;
			screen_x_scale = 4.0f/3.0f;
		} else if (!strcmp(argv[0], "-fullscreen")) {
			argc--;
			argv++;
			fullscreen = true;
		} else if (!strcmp(argv[0], "-opacity")) {
			argc--;
			argv++;
			if (!argc || argv[0][0] == '-') {
				usage();
			}
			window_opacity = strtof(argv[0], NULL);
			argc--;
			argv++;
		} else if (!strcmp(argv[0], "-sound")) {
			argc--;
			argv++;
			if (!argc || argv[0][0] == '-') {
				audio_usage();
			}
			audio_dev_name = argv[0];
			argc--;
			argv++;
		} else if (!strcmp(argv[0], "-abufs")) {
			argc--;
			argv++;
			if (!argc || argv[0][0] == '-') {
				usage();
			}
			audio_buffers = (int)strtol(argv[0], NULL, 10);
			audio_buffers_set = true;
			argc--;
			argv++;
		} else if (!strcmp(argv[0], "-rtc")) {
			argc--;
			argv++;
			set_system_time = true;
		} else if (!strcmp(argv[0], "-serial")) {
			argc--;
			argv++;
			has_serial = true;
		} else if (!strcmp(argv[0], "-nohostieee") || !strcmp(argv[0], "-nohostfs")) {
			argc--;
			argv++;
			no_ieee_intercept = true;
			hostfs_set = false;
			using_hostfs = false;
		} else if (!strcmp(argv[0], "-hostfsdev")){
			argc--;
			argv++;
			if (!argc || argv[0][0] == '-') {
				usage();
			}
			ieee_unit = (uint8_t)strtol(argv[0], NULL, 10);
			if (ieee_unit < 8 || ieee_unit > 31) {
				usage();
			}
			hostfs_set = true;
			using_hostfs = true;
			argc--;
			argv++;
		} else if (!strcmp(argv[0], "-fsroot")) {
			argc--;
			argv++;
			if (!argc || argv[0][0] == '-') {
				usage();
			}
			fsroot_path = (uint8_t *)argv[0];
			argc--;
			argv++;
		} else if (!strcmp(argv[0], "-startin")) {
			argc--;
			argv++;
			if (!argc || argv[0][0] == '-') {
				usage();
			}
			startin_path = (uint8_t *)argv[0];
			argc--;
			argv++;
		} else if (!strcmp(argv[0], "-noemucmdkeys")) {
			argc--;
			argv++;
			disable_emu_cmd_keys = true;
		} else if (!strcmp(argv[0], "-capture")) {
			argc--;
			argv++;
			grab_mouse = true;
		} else if (!strcmp(argv[0], "-longpwron")) {
			argc--;
			argv++;
			pwr_long_press = true;
		} else if (!strcmp(argv[0], "-nokeyboardcapture")) {
			argc--;
			argv++;
			no_keyboard_capture = true;
		} else if (!strcmp(argv[0], "-via2")) {
			argc--;
			argv++;
			has_via2 = true;
		} else if (!strcmp(argv[0], "-version")){
			printf("%s", VER_INFO);
#ifdef GIT_REV
			printf(" "GIT_REV"\n");
#else
			printf("\n");
#endif
			argc--;
			argv++;
			exit(0);
		} else if (!strcmp(argv[0], "-testbench")){
			printf("Testbench mode...\n");
			fflush(stdout);
			argc--;
			argv++;
			testbench=true;
			headless=true;
		} else if (!strcmp(argv[0], "-mhz")){
			argc--;
			argv++;
			if (!argc || argv[0][0] == '-') {
				usage();
			}
			MHZ = (uint8_t)strtol(argv[0], NULL, 10);
			if (MHZ < 1 || MHZ > 40) {
				usage();
			}
			argc--;
			argv++;
		} else if (!strcmp(argv[0], "-midline-effects")){
			argc--;
			argv++;
			enable_midline = true;
		} else if (!strcmp(argv[0], "-enable-ym2151-irq")){
			argc--;
			argv++;
			ym2151_irq_support = true;
		} else if (!strcmp(argv[0], "-c816")){
			argc--;
			argv++;
			regs.is65c816 = true;
			is_gen2 = false;
		} else if (!strcmp(argv[0], "-c02")){
			argc--;
			argv++;
			regs.is65c816 = false;
			is_gen2 = false;
		} else if (!strcmp(argv[0], "-gs")) {
			argc--;
			argv++;
			regs.is65c816 = true;
			is_gen2 = true;
		}
		else if (!strcmp(argv[0], "-rockwell")){
			argc--;
			argv++;
			warn_rockwell = false;
		} else {
			usage();
		}
	}

	// A breakpoint needs a front end that can resume the machine. Without one
	// the first stop is the last: the emulator halts and nothing is listening
	// for a key or a client to start it again.
	//
	// So -bp and -wp *require* a debugger rather than choosing one. They used
	// to switch the text debugger on directly, which was right when it was the
	// only one -- but it meant `-imgui -bp` opened the text overlay over the
	// graphical debugger the user actually asked for, and `-debugport -bp`
	// opened a window on a run that was meant to be headless. Deciding here
	// rather than in the flag handler also makes it independent of the order
	// the flags were written in.
	if (cli_stops_requested && !debug_window_enabled && !imgui_debugger_enabled
	    && !debug_server_is_enabled()) {
		debug_window_enabled = true;
	}

	// Follows the debugger unless asked otherwise, which is where it sat before
	// it became a flag: someone debugging wants symbols for what they load, and
	// someone just running a game gains nothing from reading .dbg files. A
	// policy rather than a snapshot, because the running machine can switch the
	// debugger on itself (emu_write register 0).
	//
	// Either debugger counts. The graphical one's Source and Symbols panels are
	// exactly the things that need this, and gating on -debug alone left them
	// permanently empty under -imgui.
	dbg_load_note_debugger(debugger_enabled || imgui_debugger_enabled);
	dbg_load_set_policy(dbg_auto_opt);

	if (is_gen2) {
		num_banks = NUM_MAX_BANKS;
		num_ram_banks = NUM_MAX_RAM_BANKS;
	}

	SDL_RWops *f = SDL_RWFromFile(rom_path, "rb");
	if (!f) {
		printf("Cannot open %s!\n", rom_path);
		exit(1);
	}
	size_t rom_size = SDL_RWread(f, ROM, ROM_SIZE, 1);
	(void)rom_size;
	SDL_RWclose(f);

	if (nvram_path) {
		SDL_RWops *f = SDL_RWFromFile(nvram_path, "rb");
		if (f) {
			SDL_RWread(f, nvram, 1, sizeof(nvram));
			SDL_RWclose(f);
		}
	}

	if (sdcard_path) {
		sdcard_set_path(sdcard_path);
		if (!hostfs_set) {
			using_hostfs = false;
		}
	}

	if (using_hostfs && !audio_buffers_set) {
#ifdef __EMSCRIPTEN__
		audio_buffers = 8; // wasm has larger buffers in audio.c, so we keep it 8 even w/ HostFS
#else
		audio_buffers = 32;
#endif
	}

	if (sf2_path && has_midi_card) {
		if (midi_card_addr < 0x9f60) {
			fprintf(stderr, "Warning: Serial MIDI card address must be in the range of 9F60-9FF0\n");
		} else {
			midi_init();
			midi_load_sf2((uint8_t *)sf2_path);
		}
	} else if (sf2_path || has_midi_card) {
		fprintf(stderr, "Warning: -sf2 and -midicard must be specified together in order to enable the MIDI synth.\n");
		has_midi_card = false;
	}

	if (cartridge_path) {
		if (!cartridge_load(cartridge_path, !zeroram)) {
			printf("Cannot open %s!\n", cartridge_path);
			exit(1);
		}
	}

	prg_override_start = -1;
	if (prg_path) {
		char *comma = strchr(prg_path, ',');
		if (comma) {
			prg_override_start = (uint16_t)strtol(comma + 1, NULL, 16);
			*comma = 0;
		}

		prg_file = SDL_RWFromFile(prg_path, "rb");
		if (!prg_file) {
			printf("Cannot open %s!\n", prg_path);
			exit(1);
		}
		// Recorded here because the file layer never resolves a name for this
		// one: it is handed the open handle instead. The comma-separated load
		// address, if there was one, has already been stripped above.
		dbg_load_set_prg_path(prg_path);
	}

	if (bas_path) {
		SDL_RWops *bas_file = SDL_RWFromFile(bas_path, "r");
		if (!bas_file) {
			printf("Cannot open %s!\n", bas_path);
			exit(1);
		}
		paste_text = paste_text_data;
		size_t paste_size = SDL_RWread(bas_file, paste_text, 1, sizeof(paste_text_data) - 1);
		if (run_after_load) {
			strncpy(paste_text + paste_size, "\rRUN\r", sizeof(paste_text_data) - paste_size);
		} else {
			paste_text[paste_size] = 0;
		}
		SDL_RWclose(bas_file);
	}

	if (run_test) {
		paste_text = paste_text_data;
		snprintf(paste_text, sizeof(paste_text_data), "TEST %d\r", test_number);
	}

#ifdef SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR
	// Don't disable compositing (on KDE for example)
	// Available since SDL 2.0.8
	SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
#endif
#ifdef __EMSCRIPTEN__
	emscripten_set_main_loop(emscripten_main_loop, 0, 0);
#endif
	// The graphical debugger needs a window, and headless mode never opens one:
	// video_init() below is skipped entirely, so the self-disable inside it
	// never runs. Clearing the flag here keeps the invariant the rest of the
	// code relies on -- the flag is set only if the window exists -- true on
	// this path as well as on the creation-failure path. Left set, the emulator
	// loop routes through DEBUGGetCurrentStatus(), and a breakpoint parks the
	// machine in DMODE_STOP with no window, no events and nothing able to
	// resume it: a silent spin at 100% of a core. (STP is handled separately --
	// stop6502() reports it to the testbench harness instead of stopping.)
	if (headless && imgui_debugger_enabled) {
		imgui_debugger_enabled = false;
		printf("The graphical debugger needs a window; -imgui is ignored in headless mode.\n");
		// Re-state the .dbg policy: it was noted above from a flag that has
		// just been cleared, and without this a headless run would keep loading
		// debug info for a debugger that is not there.
		dbg_load_note_debugger(debugger_enabled || imgui_debugger_enabled);
	}

	if (!headless) {
		// Shows up in the power management area of Linux desktops of applications inhibiting the screensaver
		// As well as the audio mixer
		// Unless hinted, defaults are "My SDL application" and "Playing a game"
#ifdef SDL_HINT_AUDIO_DEVICE_APP_NAME
		SDL_SetHint(SDL_HINT_AUDIO_DEVICE_APP_NAME, "Commander X16 Emulator");
#endif
#ifdef SDL_HINT_APP_NAME
		SDL_SetHint(SDL_HINT_APP_NAME, "Commander X16 Emulator");
#endif
#ifdef SDL_HINT_SCREENSAVER_INHIBIT_ACTIVITY_NAME
		SDL_SetHint(SDL_HINT_SCREENSAVER_INHIBIT_ACTIVITY_NAME, "Emulating modern retro awesomeness");
#endif
		int e = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO | SDL_INIT_TIMER);
		if (e < 0) {
			fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
			exit(-1);
		}
		audio_init(audio_dev_name, audio_buffers);
		video_init(window_scale, screen_x_scale, scale_quality, fullscreen, window_opacity);
	}

	wav_recorder_set_path(wav_path);

	memory_init();

	joystick_init();

	rtc_init(set_system_time);

	machine_reset();

	timing_init();

	instruction_counter = 0;

#ifdef __EMSCRIPTEN__
	emscripten_cancel_main_loop();
	emscripten_set_main_loop(emscripten_main_loop, 0, 1);
#else
	emulator_loop(NULL);
#endif

	debug_server_shutdown();
	source_view_free();
	dbg_info_free();
	main_shutdown();
	memory_dump_usage_counts();
	return 0;
}

void main_shutdown() {
	if (!headless){
		wav_recorder_shutdown();
		audio_close();
		video_end();
		SDL_Quit();
	}
	if(cartridge_path) {
		cartridge_save_nvram();
		cartridge_unload();
	}
	files_shutdown();

#ifdef PERFSTAT
	for (int pc = 0xc000; pc < sizeof(stat)/sizeof(*stat); pc++) {
		if (stat[pc] == 0) {
			continue;
		}
		char *label = label_for_address(pc);
		if (!label) {
			continue;
		}
		char *original_label = label;
		uint16_t pc2 = pc;
		if (label[0] == '@') {
			label = NULL;
			while (!label || label[0] == '@') {
				pc2--;
				label = label_for_address(pc2);
			}
		}
		printf("%d\t $%04X %s+%d", stat[pc], pc, label, pc-pc2);
		if (pc-pc2 != 0) {
			printf(" (%s)", original_label);
		}
		printf("\n");
	}
#endif

}

bool
set_kernal_status(uint8_t s)
{
	// There is no KERNAL API to write the STATUS variable.
	// But there is code to read it, READST, which should
	// always look like this:
	// 00:.,d6a0 ad 89 02 lda $0289
	// 00:.,d6a3 0d 89 02 ora $0289
	// 00:.,d6a6 8d 89 02 sta $0289
	// We can extract the location of the STATUS variable
	// from it.

	// JMP in the KERNAL API vectors
	if (debug_read6502(0xffb7, 0, 0) != 0x4c) {
		return false;
	}
	// target of KERNAL API vector JMP
	uint16_t readst = debug_read6502(0xffb8, 0, 0) | debug_read6502(0xffb9, 0, 0) << 8;
	if (readst < 0xc000) {
		return false;
	}
	// ad 89 02 lda $0289
	if (debug_read6502(readst, 0, 0) != 0xad) {
		return false;
	}
	// ad 89 02 lda $0289
	if (debug_read6502(readst + 3, 0, 0) != 0x0d) {
		return false;
	}
	// ad 89 02 lda $0289
	if (debug_read6502(readst + 6, 0, 0) != 0x8d) {
		return false;
	}
	uint16_t status0 = debug_read6502(readst+1, 0, 0) | debug_read6502(readst+2, 0, 0) << 8;
	uint16_t status1 = debug_read6502(readst+4, 0, 0) | debug_read6502(readst+5, 0, 0) << 8;
	uint16_t status2 = debug_read6502(readst+7, 0, 0) | debug_read6502(readst+8, 0, 0) << 8;
	// all three addresses must be the same
	if (status0 != status1 || status0 != status2) {
		return false;
	}

	// everything okay, write the status!
	RAM[status0] = s;
	return true;
}

bool
handle_ieee_intercept()
{
	if (no_ieee_intercept) {
		return false;
	}

	if (regs.pc < 0xFEA8 || (is_gen2 && regs.k != 0) || !is_kernal()) {
		return false;
	}

	if (has_serial) {
		// if we do bit-level serial bus emulation, we don't
		// do high-level KERNAL IEEE API interception
		return false;
	}

	if (sdcard_attached && !prg_file && !using_hostfs) {
		// if should emulate an SD card (and don't need to
		// hack a PRG into RAM), we skip HostFS if it uses unit 8
		return false;
	}

	if (sdcard_attached && prg_file && prg_finished_loading && !using_hostfs) {
		// also skip if we should do SD card and we're done
		// with the PRG hack if HostFS uses unit 8
		return false;
	}

	uint64_t base_ticks = SDL_GetPerformanceCounter();

	static int count_unlistn = 0;
	bool handled = true;
	int s = -1;
	switch(regs.pc) {
		case 0xFEA8: { // EXTAPI16
			if (!regs.is65c816 || (regs.status & 0x20)) { // don't handle if not '816 or if m=1
				handled = false; // punt to kernal's 65C816 check and will break if m=1
				break;
			}
			switch(regs.c) {
				case 5: { // XMACPTR
					s=XMACPTR(regs.status & 0x01);
					if (s == -2) {
						handled = false;
					} else if (s == -3) {
						regs.status |= 1; // SEC (unsupported, or in this case, no open context)
						regs.status &= ~0x30; // REP #$30
					} else {
						regs.status &= ~0x31; // REP #$30 and CLC -> supported
					}
					break;
				}
				case 6: { // XMCIOUT
					s=XMCIOUT(regs.status & 0x01);
					if (s == -2) {
						handled = false;
					} else if (s == -3) {
						regs.status |= 1; // SEC (unsupported, or in this case, no open context)
						regs.status &= ~0x30; // REP #$30
					} else {
						regs.status &= ~0x31; // REP #$30 and CLC -> supported
					}
					break;
				}
				default:
					handled = false;
					break;
			}
			break;
		}
		case 0xFEB1: {
			uint16_t count = regs.a;
			s=MCIOUT(regs.yl << 8 | regs.xl, &count, regs.status & 0x01);
			if (s == -2) {
				handled = false;
			} else if (s == -3) {
				regs.status = (regs.status | 1); // SEC (unsupported, or in this case, no open context)
			} else {
				regs.x = count & 0xff;
				regs.y = count >> 8;
				regs.status &= 0xfe; // clear C -> supported
			}
			break;
		}
		case 0xFF44: {
			uint16_t count = regs.a;
			s=MACPTR(regs.yl << 8 | regs.xl, &count, regs.status & 0x01);
			if (s == -2) {
				handled = false;
			} else if (s == -3) {
				regs.status = (regs.status | 1); // SEC (unsupported, or in this case, no open context)
			} else {
				regs.x = count & 0xff;
				regs.y = count >> 8;
				regs.status &= 0xfe; // clear C -> supported
			}
			break;
		}
		case 0xFF93:
			s=SECOND(regs.a);
			if (s == -2) {
				handled = false;
			}
			break;
		case 0xFF96:
			s=TKSA(regs.a);
			if (s == -2) {
				handled = false;
			}
			break;
		case 0xFFA5:
			s=ACPTR(&regs.a);
			if (s == -2) {
				handled = false;
			} else {
				regs.status = (regs.status & ~3) | (!regs.a << 1); // unconditional CLC, and set zero flag based on byte read
			}
			break;
		case 0xFFA8:
			s=CIOUT(regs.a);
			if (s == -2) {
				handled = false;
			} else {
				regs.status = (regs.status & ~1); // unconditonal CLC
			}
			break;
		case 0xFFAB:
			s=UNTLK();
			if (s == -2) {
				handled = false;
			}
			break;
		case 0xFFAE:
			s=UNLSN();
			if (s == -2) {
				handled = false;
			}
			if (prg_file && sdcard_path_is_set() && ++count_unlistn == 4) {
				// after auto-loading a PRG from the host fs,
				// switch to the SD card if requested
				// 4x UNLISTEN:
				//    2x for LOAD"AUTOBOOT.X16*"
				//    2x for LOAD":*"
				prg_finished_loading = true;
				sdcard_attach();
			}
			break;
		case 0xFFB1:
			s=LISTEN(regs.a);
			if (s == -2) {
				handled = false;
			} else {
				regs.status = (regs.status & ~1); // unconditonal CLC
			}
			break;
		case 0xFFB4:
			s=TALK(regs.a);
			if (s == -2) {
				handled = false;
			} else {
				regs.status = (regs.status & ~1); // unconditonal CLC
			}
			break;
		default:
			handled = false;
			break;
	}

	if (handled) {
		// Add the number CPU cycles equivalent to the amount of time that the operation actually took
		// to prevent the emu from warping after a hostfs load
		uint64_t perf_diff = SDL_GetPerformanceCounter() - base_ticks;
		uint32_t missed_ticks = (uint64_t)(perf_diff * 1000000ULL * MHZ) / SDL_GetPerformanceFrequency();
		clockticks6502 += missed_ticks;
		if (s >= 0) {
			if (!set_kernal_status(s)) {
				printf("Warning: Could not set STATUS!\n");
			}
		}

		increment_wrap_at_page_boundary(&regs.sp);
		uint8_t low = debug_read6502(regs.sp, 0, USE_CURRENT_X16_BANK);
		increment_wrap_at_page_boundary(&regs.sp);
		regs.pc = ((debug_read6502(regs.sp, 0, USE_CURRENT_X16_BANK) << 8) | low) + 1;
	}
	return handled;
}

void
emscripten_main_loop(void) {
	emulator_loop(NULL);
}


// Per-instruction KERNAL snooping and host-side plumbing: the $ffff
// termination check, the -echo hook, mouse-cursor tracking, PRG auto-load and
// feeding pasted text into the keyboard buffer. Shared by emulator_loop() and
// emulator_step_during_move() so a window drag does not silently skip any of
// it -- notably -echo output, which would otherwise be lost for good.
// Returns false when the caller should stop emulating.
static bool
emulator_post_step(void)
{
	if (regs.pc == 0xffff) {
		if (save_on_exit) {
			machine_dump("CPU program counter reached $ffff");
		}
		return false;
	}

	// Change this comparison value if ever additional KERNAL
	// API calls are snooped in this routine.

	if (regs.pc >= 0xff68 && (!is_gen2 || regs.k == 0) && is_kernal()) {
		if (regs.pc == 0xff68) {
			kernal_mouse_enabled = !!regs.a;
			SDL_ShowCursor((mouse_grabbed || kernal_mouse_enabled) ? SDL_DISABLE : SDL_ENABLE);
		}

		if (echo_mode != ECHO_MODE_NONE && regs.pc == 0xffd2) {
			uint8_t c = regs.a;
			if (echo_mode == ECHO_MODE_COOKED) {
				if (c == 0x0d) {
					printf("\n");
				} else if (c == 0x0a) {
					// skip
				} else if (c < 0x20 || c >= 0x80) {
					printf("\\X%02X", c);
				} else {
					printf("%c", c);
				}
			} else if (echo_mode == ECHO_MODE_ISO) {
				if (c == 0x0d) {
					printf("\n");
				} else if (c == 0x0a) {
					// skip
				} else if (c < 0x20 || (c >= 0x80 && c < 0xa0)) {
					printf("\\X%02X", c);
				} else {
					print_iso8859_15_char(c);
				}
			} else {
				printf("%c", c);
			}
			fflush(stdout);
		}

		if (regs.pc == 0xffcf) {
			// as soon as BASIC starts reading a line...
			static bool prg_done = false;

			if (prg_file && !prg_done) {
				int loadlen = 0;
				// LOAD":*" will cause the IEEE library
				// to load from "prg_file"
				if (prg_override_start >= 0) {
					loadlen = snprintf(paste_text_data, sizeof(paste_text_data), "LOAD\":*\",%d,1,$%04X\r", ieee_unit, prg_override_start);
				} else {
					loadlen = snprintf(paste_text_data, sizeof(paste_text_data), "LOAD\":*\",%d,1\r", ieee_unit);
				}
				paste_text = paste_text_data;
				prg_done = true;

				if (run_after_load) {
					if (prg_override_start >= 0) {
						snprintf(paste_text_data + loadlen, sizeof(paste_text_data) - loadlen, "SYS$%04X\r", prg_override_start);
					} else {
						snprintf(paste_text_data + loadlen, sizeof(paste_text_data) - loadlen, "RUN\r");
					}
				}
			}
			else if (testbench && !test_init_complete){
				snprintf(paste_text_data, sizeof(paste_text_data), "SYS65533\r");
				paste_text = paste_text_data;
				test_init_complete=true;
			}

			if (paste_text) {
				// ...paste BASIC code into the keyboard buffer
				pasting_bas = true;
				if (warp_pastes) machine_set_warp(true);
			}
		}

	}
#if 0 // enable this for slow pasting
	if (!(instruction_counter % 100000))
#endif
	while (pasting_bas && BRAM[NDX - 0xa000] < 10 && !(regs.status & 0x04)) {
		uint32_t c;
		int e = 0;

		if (paste_text[0] == '\\' && paste_text[1] == 'X' && paste_text[2] && paste_text[3]) {
			uint8_t hi = strtol((char[]){paste_text[2], 0}, NULL, 16);
			uint8_t lo = strtol((char[]){paste_text[3], 0}, NULL, 16);
			c = hi << 4 | lo;
			paste_text += 4;
		} else {
			paste_text = utf8_decode(paste_text, &c, &e);
			c = iso8859_15_from_unicode(c);
		}
		if (c && !e) {
			BRAM[KEYD - 0xa000 + BRAM[NDX - 0xa000]] = c;
			BRAM[NDX - 0xa000]++;
		} else {
			pasting_bas = false;
			if (warp_pastes) machine_set_warp(false);
			paste_text = NULL;
			if (clipboard_buffer) {
				SDL_free(clipboard_buffer);
				clipboard_buffer = NULL;
			}
		}
	}

	return true;
}

// Set when emulator_post_step() reports that emulation should stop (the guest
// reached $ffff). The drag loop cannot exit the program from inside the OS
// modal loop, so it latches this and stops stepping; emulator_loop() unwinds
// once the drag is over.
static bool emulator_stop_requested = false;

// Longest we will spend emulating inside a single timer tick. The OS cannot
// deliver mouse input to the drag while we are in the window procedure, so a
// long slice is felt directly as a jerky window. A frame that does not finish
// in time simply continues on the next tick.
#define MOVE_SLICE_BUDGET_MS 6

// Record the instruction about to execute into the disassembly code map. Call
// immediately before step6502() at every execution site so coverage of real
// instruction starts (and the M/X/E flag state that fixes 65C816 operand
// widths) is complete. The emulation-mode width bits are folded into the stored
// status so it can be fed straight into disasm()'s implied_status.
static inline void
code_map_record_current(void)
{
	uint8_t status = regs.status;
	// step6502() forces 8-bit memory/index widths whenever the emulation flag
	// is set (true for the 65C02 always, and the 65C816 in emulation mode), so
	// mirror that here — otherwise a record taken just after a PLP/RTI that
	// pulled the width bits clear would disassemble past addresses too wide.
	if (regs.e)
		status |= (uint8_t)(FLAG_INDEX_WIDTH | FLAG_MEMORY_WIDTH);
	code_map_record(regs.pc, regs.k, memory_get_ram_bank(), memory_get_rom_bank(), status);
}

// Advance the emulator from inside the OS modal window move/resize loop (see
// video_win32.c). That loop blocks emulator_loop() entirely, so without this
// the machine would freeze while the window is dragged.
//
// Two things matter for the drag to stay smooth: never sleep here, and never
// run for long. We pace by checking how far ahead of the wall clock we are and
// skipping the tick if we are ahead, rather than sleeping the deficit away, and
// we cap how much emulation one tick may do. Shares old_clockticks6502 and the
// timing accumulators with emulator_loop(), so emulation resumes seamlessly
// when the drag ends.
void
emulator_step_during_move(void)
{
	if (headless || emulator_stop_requested) {
		return;
	}
	// Presenting can dispatch messages, which can land back here. Emulating
	// re-entrantly would corrupt the clock accounting.
	static bool in_step = false;
	if (in_step) {
		return;
	}
	// Leave the machine alone whenever any debugger is in play. Breakpoints are
	// honoured by DEBUGGetCurrentStatus(), which also drives the debugger's own
	// window and event pump and so cannot be called from inside the OS modal
	// loop; stepping here without it would run straight through breakpoints.
	// Dragging with -debug therefore behaves as it always has -- and the same
	// has to hold for -imgui, whose window is move-hooked too, so a drag of
	// either window cannot run the CPU behind the UI's back.
	//
	// The DAP server counts too, and for the same reason -- a client's
	// breakpoints are the same breakpoints. The guest can clear
	// debugger_enabled by writing $9FB0, so testing that alone would let a drag
	// step past a breakpoint a client is waiting on.
	if (debugger_enabled || imgui_debugger_enabled || debug_server_is_enabled()) {
		return;
	}

	// Already ahead of real time: do nothing at all. This is the common case
	// now that a fast drag calls us on every mouse message, and it has to stay
	// cheap -- no emulation, and no present either.
	if (!warp_mode && timing_lead_us() > 0) {
		return;
	}

	in_step = true;

	bool new_frame = false;
	uint32_t steps = 0;
	const uint32_t slice_start = SDL_GetTicks();
	do {
		if (handle_ieee_intercept()) {
			// Polled here too: this loop can run many intercepts without ever
			// returning to the main loop, and only one completed load is held
			// at a time, so a second would overwrite the first. Reachable only
			// via -dbgauto without -debug, since dragging bails out above when
			// the debugger is enabled.
			dbg_load_poll();
			continue;
		}
		instruction_counter += waiting ^ 0x1;
		step6502();
		uint32_t clocks = clockticks6502 - old_clockticks6502;
		old_clockticks6502 = clockticks6502;
		via1_step(clocks);
		vera_spi_step(MHZ, clocks);
		if (has_serial) {
			serial_step(clocks);
		}
		if (has_via2) {
			via2_step(clocks);
		}
		new_frame |= video_step(MHZ, clocks, false);
		for (uint32_t i = 0; i < clocks; i++) {
			i2c_step();
		}
		rtc_step(clocks);
		audio_step(clocks);
		midi_serial_step(clocks);
		if (ym2151_irq_support) {
			audio_render();
		}
		if (video_get_irq_out() || via1_irq() || (has_via2 && via2_irq()) || (ym2151_irq_support && YM_irq()) || (has_midi_card && midi_serial_irq())) {
			irq6502();
		}
		if (!emulator_post_step()) {
			emulator_stop_requested = true;
			break;
		}
		// Reading the clock every instruction would cost more than it saves.
		if ((++steps & 0x3FF) == 0 && SDL_GetTicks() - slice_start >= MOVE_SLICE_BUDGET_MS) {
			break;
		}
	} while (!new_frame && steps < 4000000);

	// Only worth presenting once the frame is actually finished; a partial
	// frame looks no different from the one already on screen.
	if (new_frame) {
		video_present_no_input();
	}
	timing_update_no_sleep();

	in_step = false;
}

void *
emulator_loop(void *param)
{
	old_clockticks6502 = clockticks6502;
	for (;;) {
		// A drag may have run the guest to $ffff while the OS held us in its
		// modal loop; finish unwinding now that we are back in control.
		if (emulator_stop_requested) break;

		if (smc_requested_reset) machine_reset();

		if (testbench && regs.pc == 0xfffd){
			testbench_init();
		}

		// The guest can clear debugger_enabled by writing $9FB0. With a DAP
		// client attached that must not take the debugger down with it: nothing
		// would poll the socket again, breakpoints would stop being tested, and
		// the client would hang with no way back. Running the status hook here
		// rather than polling separately also keeps the socket work behind the
		// 1-in-1000 throttle inside it -- a bare poll in this loop is one
		// syscall per emulated instruction.
		//
		// The graphical debugger needs this too: DEBUGGetCurrentStatus() is what
		// detects breakpoint arrival and completes a step, and it delegates the
		// pause loop to whichever UI is up. Tested against the window rather
		// than the flag alone, so that no path can park the machine in a pause
		// loop that has no UI to resume it.
		if (debugger_enabled || debug_server_is_enabled()
		    || (imgui_debugger_enabled && video_debug_ui_available())) {
			int dbgCmd = DEBUGGetCurrentStatus();
			if (dbgCmd > 0) continue;
			if (dbgCmd < 0) break;
		}

#ifdef PERFSTAT

//		if (memory_get_rom_bank() == 3) {
//			stat[pc]++;
//		}
		if (memory_get_rom_bank() == 3) {
			static uint8_t old_sp;
			static uint16_t base_pc;
			if (regs.sp < old_sp) {
				base_pc = pc;
			}
			old_sp = regs.sp;
			stat[base_pc]++;
		}
#endif

#ifdef TRACE
		if (regs.pc == trace_address && trace_address != 0) {
			trace_mode = true;
		}
		if (trace_mode && !waiting) {
			char *lst = lst_for_address(regs.pc);
			if (lst) {
				char *lf;
				while ((lf = strchr(lst, '\n'))) {
					for (int i = 0; i < 120; i++) {
						printf(" ");
					}
					if (regs.is65c816) {
						printf("        "); // 8 extra width
					}
					for (char *c = lst; c < lf; c++) {
						printf("%c", *c);
					}
					printf("\n");
					lst = lf + 1;
				}
			}

			printf("[%8d] ", instruction_counter);

			int32_t eff_addr;

			char *label = label_for_address(regs.pc);
			int label_len = label ? strlen(label) : 0;
			if (label) {
				printf("%s", label);
			}
			for (int i = 0; i < 20 - label_len; i++) {
				printf(" ");
			}

			if (regs.pc >= 0xc000 && regs.k == 0) {
				printf (" %02x", memory_get_rom_bank());
			} else if (regs.pc >= 0xa000 && regs.k == 0) {
				printf (" %02x", memory_get_ram_bank());
			} else {
				printf (" --");
			}

			printf(":.,%04x ", regs.pc);

			char disasm_line[15];
			int len = disasm(regs.pc, regs.k, RAM, disasm_line, sizeof(disasm_line), -1, regs.status, &eff_addr);
			for (int i = 0; i < len; i++) {
				printf("%02x ", debug_read6502(regs.pc + i, regs.k, USE_CURRENT_X16_BANK));
			}
			for (int i = 0; i < 9 - 3 * len; i++) {
				printf(" ");
			}
			printf("%s", disasm_line);
			for (int i = 0; i < 15 - strlen(disasm_line); i++) {
				printf(" ");
			}
			if (regs.is65c816) {
				printf("C=$%04x X=$%04x Y=$%04x S=$%04x P=", regs.c, regs.x, regs.y, regs.sp);
				for (int i = 7; i >= 0; i--) {
					printf("%c", (regs.status & (1 << i)) ? "czidxmvn"[i] : '-');
				}

				putchar(regs.e ? 'e' : '-');
			} else {
				printf("A=$%02x X=$%02x Y=$%02x S=$%02x P=", regs.a, regs.xl, regs.yl, regs.sp & 0xff);
				for (int i = 7; i >= 0; i--) {
					printf("%c", (regs.status & (1 << i)) ? "czidb-vn"[i] : '-');
				}
			}

			if (eff_addr == 0x9f23 && regs.k == 0) {
				printf(" VRAM=$%05x ", video_get_address(0));
			} else if (eff_addr == 0x9f24 && regs.k == 0) {
				printf(" VRAM=$%05x ", video_get_address(1));
			} else if (eff_addr >= 0xc000 && regs.k == 0) {
				printf(" EA=$%02x:%04x ", memory_get_rom_bank(), eff_addr);
			} else if (eff_addr >= 0xa000 && regs.k == 0) {
				printf(" EA=$%02x:%04x ", memory_get_ram_bank(), eff_addr);
			} else if (eff_addr >= 0) {
				printf(" EA=$--:%04x ", eff_addr);
			} else {
				printf("             ");
			}

			if (lst) {
				printf("%s      %s", regs.is65c816 ? "" : " ", lst);
			}

			printf("\n");
		}
#endif

		if (handle_ieee_intercept()) {
			// The only place a load can complete, so the only place worth
			// looking. Polling here rather than once per instruction also
			// makes the single pending slot self-evidently safe: an intercept
			// finishes at most one channel, and it is drained before the next
			// one can run.
			dbg_load_poll();
			continue;
		}

		instruction_counter += waiting ^ 0x1;

		// The code map is what the disassembly views anchor on: without it every
		// line above the PC is decoded from a mid-instruction byte. The ImGui
		// Disassembly panel is built entirely on it, so it has to be recorded
		// for -imgui too, not just -debug.
		if ((debugger_enabled || imgui_debugger_enabled) && !waiting) {
			code_map_record_current();
		}
		step6502();
		uint32_t clocks = clockticks6502 - old_clockticks6502;
		old_clockticks6502 = clockticks6502;
		bool new_frame = false;
		via1_step(clocks);
		vera_spi_step(MHZ, clocks);
		if (has_serial) {
			serial_step(clocks);
		}
		if (has_via2) {
			via2_step(clocks);
		}
		if (!headless) {
			new_frame |= video_step(MHZ, clocks, false);
		}

		for (uint32_t i = 0; i < clocks; i++) {
			i2c_step();
		}
		rtc_step(clocks);

		if (!headless) {
			audio_step(clocks);
		}

		midi_serial_step(clocks);

		if (!headless && new_frame) {
			if (nvram_dirty && nvram_path) {
				SDL_RWops *f = SDL_RWFromFile(nvram_path, "wb");
				if (f) {
					SDL_RWwrite(f, nvram, 1, sizeof(nvram));
					SDL_RWclose(f);
				}
				nvram_dirty = false;
			}

			if (!video_update()) {
				break;
			}

			timing_update();
#ifdef __EMSCRIPTEN__
			// After completing a frame we yield back control to the browser to stay responsive
			return 0;
#endif
		}

		// The optimization from the opportunistic batching of audio rendering
		// is lost if we need to track the YM2151 IRQ, so it has been made a
		// command-line switch that's disabled by default.
		if (ym2151_irq_support) {
			audio_render();
		}

		if (video_get_irq_out() || via1_irq() || (has_via2 && via2_irq()) || (ym2151_irq_support && YM_irq()) || (has_midi_card && midi_serial_irq())) {
//			printf("IRQ!\n");
			irq6502();
		}

		if (!emulator_post_step()) {
			break;
		}
	}

	return 0;
}
