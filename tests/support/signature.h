// A hash of everything a scenario can observe, so a test can say "do this, and
// nothing changed" without listing what "nothing" covers.
//
// The point is to catch the accidental. Asserting on named fields only finds
// what the author thought to name, and the failures worth catching here are the
// ones nobody predicted -- a debugger feature that quietly advances a device,
// spends a cycle, or perturbs memory it only meant to read.
//
// Everything the fixtures can reach is folded in: CPU registers, the cycle
// counter, RAM, the bank registers, and each fake device's access counts and
// state. If a test wants to allow one specific change, it compares the fields
// it cares about instead.

#ifndef X16_TEST_SIGNATURE_H
#define X16_TEST_SIGNATURE_H

#include <stdint.h>

// A signature is deliberately opaque: comparing two is the only useful thing to
// do with one, and a numeric value invites reading meaning into a hash.
typedef struct {
	uint64_t value;
} machine_sig_t;

machine_sig_t machine_signature(void);

// Equal signatures mean nothing observable moved.
int machine_sig_equal(machine_sig_t a, machine_sig_t b);

#endif // X16_TEST_SIGNATURE_H
