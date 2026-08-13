#include "harness.h"

#include <stdio.h>

int x16_failures = 0;
int x16_checks   = 0;

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

int
x16_test_summary(const char *suite)
{
	printf("\n%s: %d checks, %d failed\n", suite, x16_checks, x16_failures);
	return x16_failures ? 1 : 0;
}
