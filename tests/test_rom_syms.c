// Checks for ROM symbol loading (src/rom_syms.c): which VICE labels are kept,
// and as which bank.

#include "rom_syms.h"
#include "support/harness.h"

#include <string.h>

static const rom_sym_t *
find(const char *name)
{
	for (int i = 0; i < rom_syms_count(); i++)
		if (!strcmp(rom_syms_at(i)->name, name))
			return rom_syms_at(i);
	return NULL;
}

int
main(void)
{
	check_eq((uint32_t)rom_syms_bank_of_file("kernal.sym"), 0, "kernal is bank 0");
	check_eq((uint32_t)rom_syms_bank_of_file("basic.sym"), 4, "basic is bank 4");
	check_eq((uint32_t)rom_syms_bank_of_file("audio.sym"), 10, "audio is bank 10");
	check_eq((uint32_t)rom_syms_bank_of_file("bannex.sym"), 12, "bannex is bank 12");
	check(rom_syms_bank_of_file("game.sym") < 0, "anything else is not a ROM bank");

	const char *kernal = "al 00FFD2 .bsout\n"
	                     "al 00DC4E .nbsout\r\n"            // CRLF too
	                     "al 00038B .__irq\n"               // KERNAL code in low RAM
	                     "al 0002C4 .__KRAMJFAR_RUN__\n"    // segment bookkeeping
	                     "al 0000CA .argexp\n"              // zero page: a variable
	                     "al 000800 .somewhere\n"           // program space
	                     "al 00ADF8 .bank_var\n"            // banked RAM
	                     "junk line\n"
	                     "al 00C000 .rom_signature";        // no trailing newline
	const int kept = rom_syms_add_text(kernal, 0);
	check_eq((uint32_t)kept, 4, "four labels kept");
	const rom_sym_t *s = find("bsout");
	check(s && s->addr == 0xFFD2 && s->bank == 0, "bsout, bank 0, leading dot dropped");
	s = find("nbsout");
	check(s && s->addr == 0xDC4E, "CRLF line ends parse");
	s = find("__irq");
	check(s && s->addr == 0x038B && s->bank == -1, "KERNAL low-RAM code kept, bank -1");
	check(find("__KRAMJFAR_RUN__") == NULL, "linker segment symbols dropped");
	check(find("argexp") == NULL, "zero page dropped");
	check(find("somewhere") == NULL, "program space dropped");
	check(find("bank_var") == NULL, "banked RAM dropped");
	check(find("rom_signature") != NULL, "last line without a newline");

	rom_syms_add_text("al 00C123 .basic_thing\nal 000300 .not_kernal\n", 4);
	s = find("basic_thing");
	check(s && s->bank == 4, "another file, another bank");
	check(find("not_kernal") == NULL, "low RAM only from the KERNAL's file");

	rom_syms_clear();
	check_eq((uint32_t)rom_syms_count(), 0, "clear empties it");
	check_eq((uint32_t)rom_syms_load_dir("no/such/dir"), 0, "a missing folder loads nothing");
	return x16_test_summary("rom_syms");
}
