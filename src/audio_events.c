// Commander X16 Emulator — sound events for x16/step. See audio_events.h.

#include "audio_events.h"

#include "ymglue.h"

#define PCM_MAX 64

bool audio_events_on = false;

static bool           want_offs;
static uint32_t       frame;
static audio_ym_event ym[AUDIO_EVENTS_MAX];
static int            ym_n;
static uint32_t       ym_dropped;
static uint32_t       pcm[PCM_MAX];
static int            pcm_n;

void
audio_events_begin(bool key_offs)
{
	want_offs       = key_offs;
	frame           = 0;
	ym_n            = 0;
	ym_dropped      = 0;
	pcm_n           = 0;
	audio_events_on = true;
}

void
audio_events_end(void)
{
	audio_events_on = false;
}

void
audio_events_frame_end(void)
{
	if (audio_events_on)
		frame++;
}

uint32_t
audio_events_voice(int channel)
{
	static const uint8_t op_regs[] = {0x40, 0x60, 0x80, 0xA0, 0xC0, 0xE0};
	uint32_t             h         = 2166136261u;
#define FNV(b) (h = (h ^ (uint8_t)(b)) * 16777619u)
	FNV(YM_debug_get_reg((uint8_t)(0x20 + channel)));
	for (int op = 0; op < 4; op++)
		for (unsigned r = 0; r < sizeof op_regs; r++)
			FNV(YM_debug_get_reg((uint8_t)(op_regs[r] + op * 8 + channel)));
#undef FNV
	return h;
}

void
audio_events_ym_write(uint8_t reg, uint8_t val)
{
	if (!audio_events_on || reg != 0x08)
		return;
	const uint8_t ops = (val >> 3) & 0x0F;
	if (!ops && !want_offs)
		return;
	if (ym_n == AUDIO_EVENTS_MAX) {
		ym_dropped++;
		return;
	}
	audio_ym_event *e = &ym[ym_n++];
	e->frame          = frame;
	e->channel        = val & 7;
	e->ops            = ops;
	e->off            = ops == 0;
	if (e->off) {
		e->kc = e->kf = 0;
		e->voice      = 0;
	} else {
		e->kc    = YM_debug_get_reg((uint8_t)(0x28 + e->channel));
		e->kf    = YM_debug_get_reg((uint8_t)(0x30 + e->channel)) >> 2;
		e->voice = audio_events_voice(e->channel);
	}
}

void
audio_events_pcm_start(void)
{
	if (!audio_events_on)
		return;
	if (pcm_n > 0 && pcm[pcm_n - 1] == frame)
		return; // once a frame is enough
	if (pcm_n < PCM_MAX)
		pcm[pcm_n++] = frame;
}

int
audio_events_ym_count(void)
{
	return ym_n;
}

const audio_ym_event *
audio_events_ym(int i)
{
	return i >= 0 && i < ym_n ? &ym[i] : 0;
}

uint32_t
audio_events_ym_dropped(void)
{
	return ym_dropped;
}

int
audio_events_pcm_count(void)
{
	return pcm_n;
}

uint32_t
audio_events_pcm_frame(int i)
{
	return i >= 0 && i < pcm_n ? pcm[i] : 0;
}
