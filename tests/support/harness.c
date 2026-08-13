#include "harness.h"

#include <stdio.h>

int x16_failures = 0;
int x16_checks   = 0;
int x16_divergences = 0;

void
check(bool cond, const char *what)
{
	x16_checks++;
	if (cond) {
		printf("ok  : %s\n", what);
	} else {
		x16_failures++;
		printf("FAIL: %s\n", what);
	}
}

void
check_eq(uint32_t got, uint32_t want, const char *what)
{
	x16_checks++;
	if (got == want) {
		printf("ok  : %s\n", what);
	} else {
		x16_failures++;
		printf("FAIL: %s (got 0x%X, want 0x%X)\n", what, got, want);
	}
}

void
check_divergent(bool passed, const char *what, const char *why)
{
	x16_checks++;
	if (passed) {
		// Better than expected, and still wrong: the marker says this differs
		// from the hardware, and it no longer does.
		x16_failures++;
		printf("FIXED: %s -- this passes now; drop the divergence marker\n", what);
	} else {
		x16_divergences++;
		printf("DIVERGE: %s\n         %s\n", what, why);
	}
}

int
x16_test_summary(const char *suite)
{
	printf("\n%s: %d checks, %d failed", suite, x16_checks, x16_failures);
	if (x16_divergences > 0) {
		printf(", %d known divergence%s",
		       x16_divergences, x16_divergences == 1 ? "" : "s");
	}
	printf("\n");

	// Said plainly, because a divergence does not fail the run and would
	// otherwise be invisible to anyone reading a green result.
	if (x16_divergences > 0) {
		printf("WARNING: %s differs from real hardware in %d known place%s. "
		       "See the DIVERGE line%s above.\n",
		       suite, x16_divergences,
		       x16_divergences == 1 ? "" : "s",
		       x16_divergences == 1 ? "" : "s");
	}

	return x16_failures ? 1 : 0;
}
