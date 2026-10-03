/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Driver tests. These call only the PWM API, and judge the result by
 * timing the TCC's own overflow flag. README.md, "How the defect is
 * detected", describes each test step by step. All periods and delays scale with
 * the TCC tick, so the tests run at any prescaler. Each test records its
 * numbers in tb_res for the final report (src/report.c).
 *
 * ztest runs suites in name order, so the boot test runs before anything
 * else touches the timer.
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "tb.h"

#define SHRINK_TRIALS 10U

/* Longest time with interrupts locked in one measurement */
#define MAX_LOCK_MS 50U

struct tb_results tb_res;

static int set_period(uint32_t period, uint32_t pulse)
{
	return pwm_set_cycles(tb_spec.dev, tb_spec.channel, period, pulse, 0);
}

/* Number of periods that fit in MAX_LOCK_MS, within [lo, hi] */
static uint32_t periods_in_lock(uint32_t period_ticks, uint32_t lo, uint32_t hi)
{
	uint64_t period_cpu = tb_ticks_to_cpu(period_ticks + 1U);
	uint64_t n = tb_cpu_hz() * MAX_LOCK_MS / 1000U / period_cpu;

	return CLAMP((uint32_t)n, lo, hi);
}

/* Wait long enough for a buffered update to land: two periods plus margin */
static void settle(uint32_t period_ticks)
{
	uint64_t us = tb_ticks_to_cpu(2U * (period_ticks + 1U)) * 1000000U / tb_cpu_hz();

	k_busy_wait((uint32_t)us + 20U);
}

/*
 * Tests that act inside each PWM cycle (glitch and duty) must finish their
 * work before the next overflow, or they miss an overflow. They use a
 * period of at least CYCLE_MIN_CPU CPU cycles.
 */
#define CYCLE_MIN_CPU 8000U

static uint32_t slow_period(void)
{
	uint32_t cpu_per_tick = MAX((uint32_t)(tb_cpu_hz() / tb_tcc_hz()), 1U);

	return MAX(tb_target_period(), DIV_ROUND_UP(CYCLE_MIN_CPU, cpu_per_tick));
}

static void *suite_setup(void)
{
	zassert_true(pwm_is_ready_dt(&tb_spec), "PWM device not ready");
	tb_rand_seed(0x5A4D3231U);
	return NULL;
}

/*
 * The first pwm_set() after boot. The unpatched SAMD21 driver starts at
 * PER = 1, where most PERB writes are lost.
 */
ZTEST(tcc_1_boot, test_works_after_boot)
{
	const uint32_t target = tb_target_period();
	uint64_t period;

	zassert_ok(set_period(target, target / 2U));
	settle(target);
	period = tb_measure_period(periods_in_lock(target, 4U, 32U));

	tb_res.boot_ran = true;
	tb_res.boot_ok = tb_period_ok(period, target);

	zassert_true(tb_res.boot_ok, "the first pwm_set() did not set the period");
}

ZTEST_SUITE(tcc_1_boot, NULL, suite_setup, NULL, NULL, NULL);

/*
 * From each starting period, request the target period at a random point
 * in the counter cycle and check that it takes effect. The loss
 * probability falls as the starting period grows.
 */
ZTEST(tcc_2_period, test_period_changes)
{
	static const uint32_t starts[TB_MAX_STARTS] = {1, 2, 3, 4, 5, 8, 16, 32, 256, 1000};
	const uint32_t target = tb_target_period();
	const uint32_t n_meas = periods_in_lock(target, 4U, 16U);
	uint32_t total = 0U;
	uint32_t count = 0U;

	tb_res.sweep_ran = true;
	tb_res.sweep_trials = CONFIG_TB_SWEEP_TRIALS;

	for (size_t s = 0; s < ARRAY_SIZE(starts); s++) {
		/* Keep starts well away from the target, so a loss is visible */
		if (starts[s] > target * 9U / 10U) {
			continue;
		}

		uint32_t span = (uint32_t)tb_ticks_to_cpu(starts[s] + 1U);
		uint32_t lost = 0U;

		for (uint32_t t = 0; t < CONFIG_TB_SWEEP_TRIALS; t++) {
			tb_force_period(starts[s]);
			tb_dither(span);
			zassert_ok(set_period(target, target / 2U));
			settle(starts[s]);
			if (!tb_period_ok(tb_measure_period(n_meas), target)) {
				lost++;
			}
		}
		tb_res.sweep_start[count] = starts[s];
		tb_res.sweep_lost[count] = lost;
		count++;
		total += lost;
	}
	tb_res.sweep_count = count;

	zassert_equal(total, 0U, "%u of %u period changes lost", total,
		      count * CONFIG_TB_SWEEP_TRIALS);
}

ZTEST_SUITE(tcc_2_period, NULL, suite_setup, NULL, NULL, NULL);

/*
 * Change to a shorter period while COUNT is above the new period. A
 * buffered update lands at the end of the current period. A direct PER
 * write that keeps COUNT lets the counter run to the wrap: up to 0.35 s
 * on a 24-bit TCC at prescaler 1.
 */
ZTEST(tcc_3_shrink, test_shorter_period)
{
	const uint32_t target = tb_target_period();
	const uint32_t big = MIN(target * 16U, (uint32_t)BIT(TB_COUNTER_SIZE) / 2U);
	const uint64_t big_cpu = tb_ticks_to_cpu(big + 1U);
	const uint64_t allowed = big_cpu / 4U + tb_ticks_to_cpu(2U * (target + 1U));
	const uint64_t timeout = tb_cpu_hz() / 4U;
	uint64_t worst = 0U;
	uint32_t failed = 0U;

	for (uint32_t t = 0; t < SHRINK_TRIALS; t++) {
		uint64_t t_ovf;
		uint64_t t_set;
		uint64_t t_first;
		uint64_t latency;
		bool period_ok;

		tb_force_period(big);
		zassert_true(tb_period_ok(tb_measure_period(2), big), "start state");

		/*
		 * Write at 80% of the period, so COUNT is above the new period.
		 * Interrupts are locked, so an ISR or a higher-priority thread
		 * cannot delay the timestamps. The wait for the first overflow
		 * stops at twice the limit; a later overflow fails anyway.
		 */
		unsigned int key = irq_lock();

		t_ovf = tb_wait_ovf(true, timeout);
		while (k_cycle_get_64() < t_ovf + big_cpu * 8U / 10U) {
		}
		tb_tcc->INTFLAG.reg = TCC_INTFLAG_OVF;
		(void)set_period(target, target / 2U);
		t_set = k_cycle_get_64();
		t_first = tb_wait_ovf(false, 2U * allowed);
		irq_unlock(key);

		latency = (t_first != 0U) ? t_first - t_set : 2U * allowed;
		period_ok = tb_period_ok(tb_measure_period(periods_in_lock(target, 4U, 8U)),
					 target);

		worst = MAX(worst, latency);
		if (latency > allowed || !period_ok) {
			failed++;
		}
	}

	tb_res.shrink_ran = true;
	tb_res.shrink_worst_us = (uint32_t)(worst * 1000000U / tb_cpu_hz());
	tb_res.shrink_limit_us = (uint32_t)(allowed * 1000000U / tb_cpu_hz());
	tb_res.shrink_failed = failed;
	tb_res.shrink_trials = SHRINK_TRIALS;

	zassert_equal(failed, 0U, "the new period was late or wrong in %u of %u trials",
		      failed, SHRINK_TRIALS);
}

ZTEST_SUITE(tcc_3_shrink, NULL, suite_setup, NULL, NULL, NULL);

/*
 * Call pwm_set() with unchanged values once per period, at a random point
 * in the first half of the cycle. A glitch-free driver leaves the counter
 * alone, so the average period stays the same. A driver that stops or
 * restarts the counter makes it longer. A restart disturbs every channel
 * of the TCC.
 *
 * The test fails with a clear message if it misses an overflow, so a slow
 * loop cannot show up as a false stretch.
 */
ZTEST(tcc_4_glitch, test_glitch_free)
{
	const uint32_t period = slow_period();
	const uint32_t n = periods_in_lock(period, 16U, 256U);
	const uint32_t period_cpu = (uint32_t)tb_ticks_to_cpu(period + 1U);
	const uint64_t timeout = tb_cpu_hz() / 4U;
	const int64_t limit = 64;
	uint64_t base;
	uint64_t t0;
	uint64_t t;
	uint64_t longest = 0U;
	int64_t delta;
	unsigned int key;

	tb_force_period(period);
	base = tb_measure_period(n);
	zassert_true(tb_period_ok(base, period), "start state");

	key = irq_lock();
	t0 = tb_wait_ovf(true, timeout);
	t = t0;
	for (uint32_t i = 0; i < n && t != 0U; i++) {
		uint64_t prev = t;
		uint64_t at = t + tb_rand() % (period_cpu / 2U);

		while (k_cycle_get_64() < at) {
		}
		(void)set_period(period, period / 2U);
		t = tb_wait_ovf(false, timeout);
		if (t != 0U) {
			longest = MAX(longest, t - prev);
		}
	}
	irq_unlock(key);
	zassert_true(t0 != 0U && t != 0U, "counter stopped");

	/* A restart adds less than one period; 1.8 periods means a missed overflow */
	zassert_true(longest < base * 18U / 10U,
		     "test overran: missed an overflow (longest gap %u CPU cycles, period %u)",
		     (uint32_t)longest, (uint32_t)base);

	delta = (int64_t)((t - t0) / n) - (int64_t)base;

	tb_res.glitch_ran = true;
	tb_res.glitch_delta_cpu = (int32_t)delta;
	tb_res.glitch_limit_cpu = (int32_t)limit;

	zassert_true(delta <= limit && delta >= -limit,
		     "each update stretches the PWM cycle by %d CPU cycles", (int32_t)delta);
}

ZTEST_SUITE(tcc_4_glitch, NULL, suite_setup, NULL, NULL, NULL);

/*
 * Change only the pulse width, at a random point in the first 75% of the
 * cycle. The pulse alternates between 3/4 and 1/4 of the period, so every
 * second change is a decrease. The test runs at two periods: a normal one,
 * and the shortest one the per-cycle measurement can follow, where update
 * conditions are frequent.
 *
 * In NPWM mode the output goes low at the compare match (COUNT == CC).
 * If a driver writes a lower CC directly after COUNT has passed it, but
 * before the old match, that cycle has no match. The output then stays
 * on for the whole cycle. The test detects this with the match flag
 * INTFLAG.MCx: no match between two overflows is a skipped match.
 *
 * The test also times the next full cycle, from overflow to match. If
 * that time does not fit the new pulse width, the write was lost.
 *
 * If the cycle ends while the driver call runs, the test cannot judge
 * that trial and skips it. A driver that restarts the counter does not
 * set the overflow flag, so a restart is judged normally.
 */
struct duty_counts {
	uint32_t judged;
	uint32_t lost;
	uint32_t skipped;
	uint32_t overran;
};

static void duty_trials(uint32_t period, struct duty_counts *c)
{
	const uint32_t hi = period * 3U / 4U;
	const uint32_t lo = period / 4U;
	const uint32_t period_cpu = (uint32_t)tb_ticks_to_cpu(period + 1U);
	/* A lost write leaves the old width, (hi - lo) away */
	const uint64_t tol = tb_ticks_to_cpu((hi - lo) / 4U);
	const uint32_t mc = BIT(TCC_INTFLAG_MC_Pos + tb_spec.channel);
	const uint64_t timeout = tb_cpu_hz() / 4U;
	bool printed = false;

	tb_force_period(period);
	zassert_ok(set_period(period, hi));
	settle(period);

	for (uint32_t t = 0; t < CONFIG_TB_DUTY_TRIALS; t++) {
		const uint32_t pulse = (t & 1U) ? hi : lo;
		uint64_t t_ovf;
		uint64_t t_end;
		uint64_t t_match = 0U;
		unsigned int key;
		bool matched;
		bool late;

		key = irq_lock();

		/* Start of the cycle that the update lands in */
		t_ovf = tb_wait_ovf(true, timeout);
		tb_tcc->INTFLAG.reg = mc;

		/* Update somewhere in the first 75% of the cycle */
		uint64_t at = t_ovf + tb_rand() % (period_cpu * 3U / 4U);

		while (k_cycle_get_64() < at) {
		}
		(void)set_period(period, pulse);

		/* An overflow during the driver call means the cycle ended early */
		late = (tb_tcc->INTFLAG.reg & TCC_INTFLAG_OVF) != 0U;

		/* End of that cycle: was there a compare match in it? */
		t_end = tb_wait_ovf(false, timeout);
		matched = (tb_tcc->INTFLAG.reg & mc) != 0U;
		tb_tcc->INTFLAG.reg = mc;

		/* The next full cycle: time from overflow to match */
		if (t_end != 0U) {
			t_match = tb_wait_flag(mc, false, 2U * period_cpu);
		}

		irq_unlock(key);

		zassert_true(t_ovf != 0U && t_end != 0U, "counter stopped");

		if (late) {
			c->overran++;
			continue;
		}
		c->judged++;

		uint64_t expected = tb_ticks_to_cpu(pulse);
		uint64_t width = (t_match != 0U) ? t_match - t_end : 0U;

		if (!matched) {
			c->skipped++;
		}
		if (t_match == 0U || width + tol < expected || width > expected + tol) {
			c->lost++;
			if (!printed) {
				TC_PRINT("first lost pulse change at period %u: expected %u CPU "
					 "cycles, measured %u\n", period, (uint32_t)expected,
					 (uint32_t)width);
				printed = true;
			}
		}
	}
}

ZTEST(tcc_5_duty, test_duty_changes)
{
	const uint32_t cpu_per_tick = MAX((uint32_t)(tb_cpu_hz() / tb_tcc_hz()), 1U);
	const uint32_t normal = slow_period();
	const uint32_t shortest = MAX(8U, DIV_ROUND_UP(CYCLE_MIN_CPU, cpu_per_tick));
	struct duty_counts c = {0};

	duty_trials(normal, &c);
	if (shortest < normal) {
		duty_trials(shortest, &c);
	}

	tb_res.duty_ran = true;
	tb_res.duty_period_normal = normal;
	tb_res.duty_period_short = MIN(shortest, normal);
	tb_res.duty_trials = c.judged;
	tb_res.duty_lost = c.lost;
	tb_res.duty_skipped = c.skipped;
	tb_res.duty_overran = c.overran;

	zassert_true(c.judged >= c.overran,
		     "test overran: only %u trials could be judged, %u could not",
		     c.judged, c.overran);
	zassert_equal(c.lost, 0U, "%u of %u pulse-width changes lost", c.lost, c.judged);
	zassert_equal(c.skipped, 0U, "%u of %u changes gave a cycle with no compare match",
		      c.skipped, c.judged);
}

ZTEST_SUITE(tcc_5_duty, NULL, suite_setup, NULL, NULL, NULL);
