// Commander X16 Emulator — names and purposes for the I/O page registers.
//
// See io_reg_info.h. Everything here is transcribed from the emulator's own
// decode: VERA from video_read/video_write in video.c, the VIA registers and
// pin assignments from via.c, the emulator registers from emu_read/emu_write
// in memory.c, and the SMC/RTC maps from smc.c and rtc.c.

#include "io_reg_info.h"

#include <stddef.h>

namespace {

struct RegInfo {
    const char *name;
    const char *purpose;
};

const char *const UNKNOWN_NAME    = "-";
const char *const UNKNOWN_PURPOSE = "Not decoded by this emulator. Reads return the open-bus value $9F.";

// --- VIA, $9F00-$9F0F (VIA1) and $9F10-$9F1F (VIA2) ------------------------
// Standard 65C22 layout; both VIAs use it.
const RegInfo VIA_REGS[16] = {
    {"ORB/IRB", "Port B data. Reading clears the CB1/CB2 interrupt flags unless ACR latching is on."},
    {"ORA/IRA", "Port A data. Reading clears the CA1/CA2 interrupt flags unless ACR latching is on."},
    {"DDRB", "Port B direction. A 1 bit makes that pin an output, a 0 bit an input."},
    {"DDRA", "Port A direction. A 1 bit makes that pin an output, a 0 bit an input."},
    {"T1C_L", "Timer 1 counter low. Reading returns the live counter and clears the T1 interrupt flag; writing loads the latch low byte without starting the timer."},
    {"T1C_H", "Timer 1 counter high. Writing loads the latch high byte, clears the T1 flag, copies the latch into the counter and STARTS the timer."},
    {"T1L_L", "Timer 1 latch low. Reaches the latch directly, without touching the running counter or the interrupt flag."},
    {"T1L_H", "Timer 1 latch high. Writing loads the latch and clears the T1 flag but does NOT restart the counter -- that is what T1C_H is for."},
    {"T2C_L", "Timer 2 counter low. Reading returns the live counter and clears the T2 interrupt flag; writing loads the latch low byte."},
    {"T2C_H", "Timer 2 counter high. Writing loads the latch, clears the T2 flag and starts the timer. T2 is one-shot only."},
    {"SR", "Shift register. Mode comes from ACR bits 4-2. Note: shift register operation is not fully implemented in this emulator."},
    {"ACR", "Auxiliary control. b7 T1 drives PB7, b6 T1 free-run vs one-shot, b5 T2 counts PB6 pulses vs time, b4-2 shift mode, b1/b0 port latching."},
    {"PCR", "Peripheral control. b7 CB1 edge, b6-4 CB2 mode, b3 CA1 edge, b2-0 CA2 mode. Decides which handshake edges set interrupt flags."},
    {"IFR", "Interrupt flags. b7 reads as the OR of every enabled flag, so it says whether an IRQ is pending. Write a 1 to a bit to clear that flag."},
    {"IER", "Interrupt enable. Reads with b7 always set. Writing with b7=1 ENABLES the named bits; writing with b7=0 DISABLES them."},
    {"ORA/IRA", "Port A data without handshake. In this emulator identical to offset $1, including clearing the CA flags on read."},
};

// --- VERA, $9F20-$9F3F -----------------------------------------------------
const RegInfo VERA_REGS[32] = {
    {"ADDRx_L", "VRAM address bits 7-0 for whichever data port ADDRSEL selects."},
    {"ADDRx_M", "VRAM address bits 15-8 for the selected data port."},
    {"ADDRx_H", "VRAM address bit 16 (b0) plus the auto-increment step in bits 7-3, and the FX nibble controls in b2/b1."},
    {"DATA0", "Auto-incrementing VRAM data port 0. Every access moves the address on, so this is the busiest register in the machine -- and why VERA capture is off by default."},
    {"DATA1", "Auto-incrementing VRAM data port 1. Same as DATA0 but uses the second address register."},
    {"CTRL", "b0 ADDRSEL picks which address port ADDRx_* refers to; b6-1 DCSEL banks the four registers at $9F29-$9F2C; b7 soft-resets VERA."},
    {"IEN", "Interrupt enable: b0 VSYNC, b1 LINE, b2 sprite collision, b3 PCM FIFO almost empty. b7 carries bit 8 of the raster compare line."},
    {"ISR", "Interrupt status, same bit order as IEN. Write a 1 to a bit to acknowledge and clear it."},
    {"IRQLINE_L", "Bits 7-0 of the scanline that raises a LINE interrupt. Bit 8 lives in IEN b7."},
    {"DC_VIDEO*", "DCSEL-banked. DCSEL=0 selects video output mode and chroma; other DCSEL values make this the display window start, or an FX register. Which one applies depends on CTRL at the time."},
    {"DC_HSCALE*", "DCSEL-banked. DCSEL=0 is the horizontal scale factor (128 = 1:1); other DCSEL values select display-window or FX registers."},
    {"DC_VSCALE*", "DCSEL-banked. DCSEL=0 is the vertical scale factor (128 = 1:1); other DCSEL values select display-window or FX registers."},
    {"DC_BORDER*", "DCSEL-banked. DCSEL=0 is the border colour index; other DCSEL values select display-window or FX registers."},
    {"L0_CONFIG", "Layer 0: b1-0 colour depth, b2 bitmap mode, b3 256-colour text, b5-4 map width, b7-6 map height."},
    {"L0_MAPBASE", "Layer 0 tilemap base address, bits 16-9 (the value times 512)."},
    {"L0_TILEBASE", "Layer 0 tile data base in b7-2, plus tile height (b1) and tile width (b0)."},
    {"L0_HSCROLL_L", "Layer 0 horizontal scroll, bits 7-0."},
    {"L0_HSCROLL_H", "Layer 0 horizontal scroll, bits 11-8."},
    {"L0_VSCROLL_L", "Layer 0 vertical scroll, bits 7-0."},
    {"L0_VSCROLL_H", "Layer 0 vertical scroll, bits 11-8."},
    {"L1_CONFIG", "Layer 1 configuration; same bit layout as L0_CONFIG."},
    {"L1_MAPBASE", "Layer 1 tilemap base address, bits 16-9."},
    {"L1_TILEBASE", "Layer 1 tile data base, tile height and tile width."},
    {"L1_HSCROLL_L", "Layer 1 horizontal scroll, bits 7-0."},
    {"L1_HSCROLL_H", "Layer 1 horizontal scroll, bits 11-8."},
    {"L1_VSCROLL_L", "Layer 1 vertical scroll, bits 7-0."},
    {"L1_VSCROLL_H", "Layer 1 vertical scroll, bits 11-8."},
    {"AUDIO_CTRL", "PCM audio: b3-0 volume, b4 stereo, b5 16-bit samples. Reading gives FIFO full in b7 and FIFO empty in b6; writing b7 resets the FIFO."},
    {"AUDIO_RATE", "PCM playback rate. Zero stops playback; otherwise the rate is the value times 25MHz/65536."},
    {"AUDIO_DATA", "PCM FIFO write port. Write-only -- reads always return 0."},
    {"SPI_DATA", "SPI data to and from the SD card. This is the ONLY path to the card, which is why it is traced as its own device rather than as part of VERA."},
    {"SPI_CTRL", "SPI control. Read: b7 busy, b2 auto-transmit, b0 slave select. Write: b0 selects or deselects the card, b2 enables auto-transmit."},
};

// --- Emulator registers, $9FB0-$9FBF ---------------------------------------
const RegInfo EMU_REGS[16] = {
    {"DBG_EN", "Reads 1 when the text debugger is enabled. Writing non-zero enables it."},
    {"LOG_VIDEO", "Reads 1 when VRAM access logging is on. Writing non-zero logs every VRAM read and write to stdout."},
    {"LOG_KBD", "Reads 1 when keyboard logging is on."},
    {"ECHO", "Echo mode: controls how KERNAL character output is mirrored to the host terminal."},
    {"SAVE_EXIT", "Reads 1 when the emulator will save state on exit."},
    {"GIF_REC", "GIF recorder state. Writing sends a start/stop command."},
    {"WAV_REC", "WAV recorder state. Writing sends a start/stop command."},
    {"CMD_KEYS", "Reads 1 when the emulator's own hotkeys are disabled."},
    {"CLK_SNAP_L", "Reading LATCHES the 32-bit cycle counter and returns bits 7-0. Read this first, then the other three, to get a coherent value. Writing resets the count."},
    {"CLK_SNAP_ML", "Bits 15-8 of the cycle count latched by reading CLK_SNAP_L. Writing prints a user debug byte to stdout."},
    {"CLK_SNAP_MH", "Bits 23-16 of the latched cycle count. Writing prints a second user debug byte to stdout."},
    {"CLK_SNAP_H", "Bits 31-24 of the latched cycle count. Writing sends a character to the host's stdout."},
    {"-", "Not implemented; warns on access."},
    {"KEYMAP", "The current keyboard layout index."},
    {"DETECT_1", "Reads ASCII '1'. With the next register it spells \"16\", which is how a program detects it is running under the emulator."},
    {"DETECT_6", "Reads ASCII '6'. See DETECT_1."},
};

const char *const VIA1_PA[8] = {
    "I2CDATA -- I2C data line (SDA) to the SMC and RTC",
    "I2CCLK -- I2C clock line (SCL)",
    "NESLATCH -- asserted to latch the controllers' button state",
    "NESCLK -- shifts the next button bit out of every controller",
    "NESDAT3 -- serial data in from controller slot 3",
    "NESDAT2 -- serial data in from controller slot 2",
    "NESDAT1 -- serial data in from controller slot 1",
    "NESDAT0 -- serial data in from controller slot 0",
};

const char *const VIA1_PB[8] = {
    "unused",
    "unused",
    "unused",
    "IECATN_OUT -- IEC serial attention out",
    "IECCLK_OUT -- IEC serial clock out",
    "IECDAT_OUT -- IEC serial data out",
    "IECCLK_IN -- IEC serial clock in",
    "IECDAT_IN -- IEC serial data in; also the T1 square-wave output when ACR b7 is set",
};

const char *const VIA_IRQ_BITS[8] = {
    "CA2 -- not wired on the X16",
    "CA1 -- not wired on the X16",
    "SR -- shift register finished eight shifts",
    "CB2 -- not wired on the X16",
    "CB1 -- IEC service request",
    "T2 -- timer 2 underflowed",
    "T1 -- timer 1 underflowed",
    "IRQ -- read-only: set when any enabled flag above is pending",
};

struct SmcReg {
    uint8_t     reg;
    const char *name;
    const char *purpose;
};

const SmcReg SMC_TABLE[] = {
    {0x01, "PWR_CTRL", "Write 0 to power the machine off, 1 to hard reboot it."},
    {0x02, "RESET_BTN", "Write 0 to act as though the reset button was pressed."},
    {0x03, "NMI_BTN", "Write 0 to act as though the NMI button was pressed."},
    {0x04, "PWR_LED", "Power LED brightness. Accepted but ignored by this emulator."},
    {0x05, "ACT_LED", "Activity LED. The emulator treats it as on/off at a threshold of 128, not as a brightness."},
    {0x07, "KBD_BUF", "Reading takes the next byte from the 16-byte keyboard ring buffer, or 0 when it is empty."},
    {0x09, "PWR_LONG_PRESS", "Reads 1 if the machine was started by a long power-button press. Reading clears it."},
    {0x20, "MOUSE_DEV", "Sets the mouse device id: 0 for none, 3 for a standard mouse. Flushes the mouse buffer."},
    {0x21, "MSE_BUF", "Reading takes the next byte of a mouse movement packet, or 0 when no complete packet is waiting."},
    {0x22, "MOUSE_DEV_ID", "Reads back the current mouse device id."},
    {0x30, "VER_MAJOR", "SMC firmware version, major."},
    {0x31, "VER_MINOR", "SMC firmware version, minor."},
    {0x32, "VER_PATCH", "SMC firmware version, patch."},
    {0x40, "DEFAULT_READ_OP", "Sets which register a bare I2C read -- one with no register select in front of it -- will return."},
    {0x41, "KBD_BUF_DEF", "Reads one keyboard byte, the same as $07. This is the power-on default for bare reads."},
    {0x42, "MSE_ONLY_BUF", "Reads mouse bytes only."},
    {0x43, "KBD_MSE_COMBO", "Reads a keyboard byte, then cycles through the mouse packet on subsequent reads."},
};

const SmcReg *
find_smc(uint8_t reg)
{
    for (size_t i = 0; i < sizeof(SMC_TABLE) / sizeof(SMC_TABLE[0]); i++) {
        if (SMC_TABLE[i].reg == reg) {
            return &SMC_TABLE[i];
        }
    }
    return NULL;
}

} // namespace

const char *
io_reg_name(uint16_t addr)
{
    if (addr >= 0x9f00 && addr < 0x9f10) {
        return VIA_REGS[addr & 0x0f].name;
    }
    if (addr >= 0x9f10 && addr < 0x9f20) {
        return VIA_REGS[addr & 0x0f].name;
    }
    if (addr >= 0x9f20 && addr < 0x9f40) {
        return VERA_REGS[addr & 0x1f].name;
    }
    if (addr >= 0x9f40 && addr < 0x9f60) {
        return (addr & 1) ? "YM_DATA" : "YM_ADDR";
    }
    if (addr >= 0x9fb0 && addr < 0x9fc0) {
        return EMU_REGS[addr & 0x0f].name;
    }
    return UNKNOWN_NAME;
}

const char *
io_reg_purpose(uint16_t addr)
{
    if (addr >= 0x9f00 && addr < 0x9f10) {
        return VIA_REGS[addr & 0x0f].purpose;
    }
    if (addr >= 0x9f10 && addr < 0x9f20) {
        return VIA_REGS[addr & 0x0f].purpose;
    }
    if (addr >= 0x9f20 && addr < 0x9f40) {
        return VERA_REGS[addr & 0x1f].purpose;
    }
    if (addr >= 0x9f40 && addr < 0x9f60) {
        return (addr & 1)
                   ? "YM2151 data port and status. Writing sends a byte to the register YM_ADDR named; reading returns the busy and timer status. Partially decoded across $9F40-$9F5F, so every odd address in that range is this register."
                   : "YM2151 register select. Write the register number here, then the value to YM_DATA. Partially decoded, so every even address in $9F40-$9F5F is this register.";
    }
    if (addr >= 0x9fb0 && addr < 0x9fc0) {
        return EMU_REGS[addr & 0x0f].purpose;
    }
    return UNKNOWN_PURPOSE;
}

const char *
io_via_pin_name(int which, char port, int bit)
{
    if (bit < 0 || bit > 7) {
        return "?";
    }
    if (which != 0) {
        // VIA2 is the user port, and this emulator wires it to nothing.
        return "user port -- not connected in the emulator";
    }
    return (port == 'A') ? VIA1_PA[bit] : VIA1_PB[bit];
}

const char *
io_via_irq_bit_name(int bit)
{
    return (bit >= 0 && bit <= 7) ? VIA_IRQ_BITS[bit] : "?";
}

const char *
io_smc_reg_name(uint8_t reg)
{
    const SmcReg *r = find_smc(reg);
    return (r != NULL) ? r->name : "-";
}

const char *
io_smc_reg_purpose(uint8_t reg)
{
    const SmcReg *r = find_smc(reg);
    return (r != NULL) ? r->purpose : "Not a register this emulator's SMC recognises; reads return $FF.";
}

const char *
io_rtc_reg_name(uint8_t reg)
{
    switch (reg) {
        case 0x00: return "RTCSC";
        case 0x01: return "RTCMN";
        case 0x02: return "RTCHR";
        case 0x03: return "RTCDOW";
        case 0x04: return "RTCDATE";
        case 0x05: return "RTCMTH";
        case 0x06: return "RTCYR";
        default: break;
    }
    if (reg >= 0x20 && reg < 0x60) {
        return "NVRAM";
    }
    return "-";
}
