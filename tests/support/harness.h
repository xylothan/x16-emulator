// Shared test harness: assertion counting and the exit-code convention.
//
// The six existing tests each carry their own copy of this. New tests share
// one so that a test file is assertions and nothing else -- with many scenario
// files planned, a private harness in each would be a great deal of duplicated
// bookkeeping. (Converting the existing six is a separate tidy-up; this does
// not touch them.)
//
// The counters have external linkage and live in harness.c rather than being
// static in this header. A header-static counter gives every translation unit
// its own copy, so the moment a test spans two files -- a shared assertion
// helper under tests/support, say -- failures raised in one file would not be
// counted by the summary in the other, and the suite would report success
// while printing FAIL lines nobody reads on a green run.
//
// check_eq() exists because "FAIL: ADC sets carry" is not enough to debug a
// conformance failure. Knowing it produced 0x81 where the datasheet says 0x80
// is the whole point, so the value-comparing form is the one to reach for.

#ifndef X16_TEST_HARNESS_H
#define X16_TEST_HARNESS_H

#include <stdbool.h>
#include <stdint.h>

extern int x16_failures;
extern int x16_checks;

void check(bool cond, const char *what);

// Compare two values, reporting both when they differ.
void check_eq(uint32_t got, uint32_t want, const char *what);

// Print the tally and return the process exit status. Never returns the raw
// failure count: exit status is masked to 8 bits, so exactly 256 failures
// would report success.
int x16_test_summary(const char *suite);

#endif // X16_TEST_HARNESS_H
