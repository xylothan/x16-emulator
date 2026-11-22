// Shared test harness: assertion counting and the exit-code convention.
//
// Shared by the test files so each is assertions only.
//
// Counters have external linkage in harness.c so a test spanning two files shares one tally.

#ifndef X16_TEST_HARNESS_H
#define X16_TEST_HARNESS_H

#include <stdbool.h>
#include <stdint.h>

extern int x16_failures;
extern int x16_checks;
extern int x16_divergences;

void check(bool cond, const char *what);

// Compare two values, reporting both when they differ.
void check_eq(uint32_t got, uint32_t want, const char *what);

// For a difference from the hardware that is known, understood and not being
// fixed yet. Prints loudly and is counted, but does not fail the build, so
// coverage can land before the fix does.
// Unexpectedly passing is a failure: delete the marker and make it an ordinary check.
void check_divergent(bool passed, const char *what, const char *why);

// Print the tally and return the process exit status. Never returns the raw
// failure count: exit status is masked to 8 bits, so exactly 256 failures
// would report success.
int x16_test_summary(const char *suite);

#endif // X16_TEST_HARNESS_H
