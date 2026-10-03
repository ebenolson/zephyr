/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Final report: one plain-language block per run, then one TBSUM line
 * that scripts/summarize.py collects into a table across runs.
 */

#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "tb.h"

enum verdict {
	GOOD,
	NOT_GLITCH_FREE,
	BROKEN,
	INCOMPLETE,
};

static const char *const verdict_text[] = {
	[GOOD] = "GOOD - every update took effect, and updates are glitch-free",
	[NOT_GLITCH_FREE] = "WORKS, NOT GLITCH-FREE - every update took effect, "
			    "but updates disturb the PWM cycle",
	[BROKEN] = "BROKEN - some updates did not take effect",
	[INCOMPLETE] = "INCOMPLETE - a test did not finish; see the ztest output",
};

static const char *const verdict_tag[] = {
	[GOOD] = "GOOD",
	[NOT_GLITCH_FREE] = "NOT_GLITCH_FREE",
	[BROKEN] = "BROKEN",
	[INCOMPLETE] = "INCOMPLETE",
};

static uint32_t sweep_lost_total(void)
{
	uint32_t total = 0U;

	for (uint32_t i = 0; i < tb_res.sweep_count; i++) {
		total += tb_res.sweep_lost[i];
	}
	return total;
}

static bool glitch_ok(void)
{
	return tb_res.glitch_delta_cpu <= tb_res.glitch_limit_cpu &&
	       tb_res.glitch_delta_cpu >= -tb_res.glitch_limit_cpu;
}

static enum verdict get_verdict(void)
{
	if ((tb_res.boot_ran && !tb_res.boot_ok) ||
	    (tb_res.sweep_ran && sweep_lost_total() != 0U) ||
	    (tb_res.shrink_ran && tb_res.shrink_failed != 0U) ||
	    (tb_res.duty_ran && tb_res.duty_lost != 0U)) {
		return BROKEN;
	}
	if (!tb_res.boot_ran || !tb_res.sweep_ran || !tb_res.shrink_ran || !tb_res.glitch_ran ||
	    !tb_res.duty_ran) {
		return INCOMPLETE;
	}
	return (glitch_ok() && tb_res.duty_skipped == 0U) ? GOOD : NOT_GLITCH_FREE;
}

static const char *yes_no(bool ran, bool ok)
{
	return !ran ? "not run" : (ok ? "yes" : "NO");
}

void tb_report(void)
{
	const uint32_t sweep_total = tb_res.sweep_count * tb_res.sweep_trials;
	const uint32_t cpu_mhz = (uint32_t)(tb_cpu_hz() / 1000000U);
	enum verdict v = get_verdict();

	printk("\n================ RESULT ================\n");
	printk("Board:      %s\n", CONFIG_BOARD_TARGET);
	printk("Driver:     %s\n", tb_driver_name());
	printk("Prescaler:  %u (target period %u ticks)\n", TB_PRESCALER, tb_target_period());
	printk("\n");

	printk("Works after boot:      %s\n", yes_no(tb_res.boot_ran, tb_res.boot_ok));

	if (tb_res.sweep_ran) {
		printk("Period changes lost:   %u of %u\n", sweep_lost_total(), sweep_total);
		printk("  by starting PER:    ");
		for (uint32_t i = 0; i < tb_res.sweep_count; i++) {
			printk(" %u:%u", tb_res.sweep_start[i], tb_res.sweep_lost[i]);
		}
		printk("  (of %u each)\n", tb_res.sweep_trials);
	} else {
		printk("Period changes lost:   not run\n");
	}

	if (tb_res.shrink_ran) {
		printk("Shorter period works:  %-8s (starts after %u us, limit %u us)\n",
		       tb_res.shrink_failed == 0U ? "yes" : "NO", tb_res.shrink_worst_us,
		       tb_res.shrink_limit_us);
	} else {
		printk("Shorter period works:  not run\n");
	}

	if (tb_res.glitch_ran) {
		uint32_t mag = (uint32_t)abs(tb_res.glitch_delta_cpu);

		printk("Glitch-free updates:   %-8s (each update stretches the PWM cycle by "
		       "%d CPU cycles, %s%u.%02u us)\n",
		       glitch_ok() ? "yes" : "NO", tb_res.glitch_delta_cpu,
		       tb_res.glitch_delta_cpu < 0 ? "-" : "", mag / cpu_mhz,
		       (mag % cpu_mhz) * 100U / cpu_mhz);
	} else {
		printk("Glitch-free updates:   not run\n");
	}

	if (tb_res.duty_ran) {
		printk("Duty changes lost:     %u of %u (", tb_res.duty_lost, tb_res.duty_trials);
		if (tb_res.duty_period_short < tb_res.duty_period_normal) {
			printk("periods %u and %u ticks", tb_res.duty_period_normal,
			       tb_res.duty_period_short);
		} else {
			printk("period %u ticks", tb_res.duty_period_normal);
		}
		printk("; %u trials not judged)\n", tb_res.duty_overran);
		printk("Skipped matches:       %u of %u (cycles with the output on for the "
		       "whole cycle)\n", tb_res.duty_skipped, tb_res.duty_trials);
	} else {
		printk("Duty changes lost:     not run\n");
	}

	printk("\nVERDICT: %s\n", verdict_text[v]);
	printk("========================================\n");

	/* One line for scripts/summarize.py */
	printk("TBSUM board=%s prescaler=%u driver=%s boot=%s period_lost=%u/%u "
	       "shrink_us=%u glitch_cpu=%d duty_lost=%u/%u skipped=%u/%u verdict=%s\n",
	       CONFIG_BOARD_TARGET, TB_PRESCALER, tb_driver_name(),
	       !tb_res.boot_ran ? "-" : (tb_res.boot_ok ? "ok" : "FAIL"), sweep_lost_total(),
	       sweep_total, tb_res.shrink_worst_us, tb_res.glitch_delta_cpu, tb_res.duty_lost,
	       tb_res.duty_trials, tb_res.duty_skipped, tb_res.duty_trials, verdict_tag[v]);
}
