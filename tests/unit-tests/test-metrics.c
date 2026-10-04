/*
 * Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <osdp.h>
#include "test.h"

#define METRICS_MAX_STEPS 10000

static void metrics_step(osdp_t *cp, osdp_t *pd)
{
	osdp_cp_refresh(cp);
	osdp_pd_refresh(pd);
	usleep(1000);
}

static bool sc_is_up(osdp_t *ctx)
{
	uint8_t mask = 0;

	osdp_get_sc_status_mask(ctx, &mask);
	return (mask & 0x01) != 0;
}

static bool step_until_sc_up(osdp_t *cp, osdp_t *pd)
{
	int i;

	for (i = 0; i < METRICS_MAX_STEPS; i++) {
		if (sc_is_up(cp) && sc_is_up(pd)) {
			return true;
		}
		metrics_step(cp, pd);
	}
	return false;
}

static bool sc_counts_are(osdp_t *ctx, const char *who, uint32_t handshakes,
			  uint32_t failures)
{
	struct osdp_metrics m;

	if (osdp_get_metrics(ctx, 0, &m)) {
		printf(SUB_2 "%s: osdp_get_metrics failed\n", who);
		return false;
	}
	if (m.sc_handshake_count != handshakes ||
	    m.sc_failure_count != failures) {
		printf(SUB_2 "%s: handshakes %u, failures %u; want %u, %u\n",
		       who, m.sc_handshake_count, m.sc_failure_count,
		       handshakes, failures);
		return false;
	}
	return true;
}

/*
 * A handshake counts once the session is active, and a failure once an
 * active session is torn down -- on both ends of the link.
 */
static bool test_sc_counts_activations_and_teardowns(struct test *t)
{
	osdp_t *cp, *pd;
	bool result = false;
	int i;

	if (test_setup_devices(t, &cp, &pd)) {
		printf(SUB_2 "device setup failed\n");
		return false;
	}
	if (!step_until_sc_up(cp, pd)) {
		printf(SUB_2 "secure channel did not come up\n");
		goto out;
	}
	if (!sc_counts_are(cp, "cp", 1, 0) || !sc_counts_are(pd, "pd", 1, 0)) {
		goto out;
	}

	if (osdp_cp_disable_pd(cp, 0)) {
		printf(SUB_2 "disable failed\n");
		goto out;
	}
	for (i = 0; i < METRICS_MAX_STEPS && osdp_cp_is_pd_enabled(cp, 0);
	     i++) {
		metrics_step(cp, pd);
	}
	if (osdp_cp_enable_pd(cp, 0)) {
		printf(SUB_2 "enable failed\n");
		goto out;
	}
	if (!step_until_sc_up(cp, pd)) {
		printf(SUB_2 "secure channel did not come back\n");
		goto out;
	}
	if (!sc_counts_are(cp, "cp", 1, 1) || !sc_counts_are(pd, "pd", 1, 1)) {
		goto out;
	}
	result = true;
out:
	osdp_cp_teardown(cp);
	osdp_pd_teardown(pd);
	return result;
}

void run_metrics_tests(struct test *t)
{
	printf("\nBegin metrics tests\n");
	TEST_CASE(t, "sc_counts_activations_and_teardowns",
		  test_sc_counts_activations_and_teardowns(t));
	printf("End metrics tests\n\n");
}
