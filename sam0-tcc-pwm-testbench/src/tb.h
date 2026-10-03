/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared definitions for the SAM0 TCC PWM testbench.
 */

#ifndef TB_H_
#define TB_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/pwm.h>
#include <soc.h>

#define TB_USER_NODE DT_PATH(zephyr_user)

#if !DT_NODE_HAS_PROP(TB_USER_NODE, pwms)
#error "Add a zephyr,user node with a pwms property (see README.md)"
#endif

#define TB_TCC_NODE     DT_PWMS_CTLR(TB_USER_NODE)
#define TB_PRESCALER    DT_PROP(TB_TCC_NODE, prescaler)
#define TB_COUNTER_SIZE DT_PROP(TB_TCC_NODE, counter_size)

/* Register name differences between the SAMD21 TCC and later revisions */
#ifdef TCC_PERBUF_PERBUF
#define TB_STATUS_PERBV     TCC_STATUS_PERBUFV
#define TB_STATUS_CCBV(ch)  TCC_STATUS_CCBUFV(BIT(ch))
#define TB_FAMILY           "PERBUF/CCBUF branch (SAMC2x/SAML2x/SAMD5x/SAME5x/SAMR3x)"
#else
#define TB_STATUS_PERBV     TCC_STATUS_PERBV
#define TB_STATUS_CCBV(ch)  TCC_STATUS_CCBV(BIT(ch))
#define TB_FAMILY           "PERB/CCB branch (SAMD21/SAMR21)"
#endif

extern const struct pwm_dt_spec tb_spec;
extern Tcc *const tb_tcc;

const char *tb_driver_name(void);

/* CPU cycle clock (k_cycle_get_64() units) and TCC tick rate, in Hz */
uint64_t tb_cpu_hz(void);
uint64_t tb_tcc_hz(void);

/* Convert TCC ticks to CPU cycles */
uint64_t tb_ticks_to_cpu(uint64_t ticks);

/*
 * Period that the tests request, in TCC ticks. At least 53 ticks, and at
 * least 1920 CPU cycles, so the overflow poll can time it accurately:
 * 1920 ticks (40 us) at prescaler 1, 53 ticks (1.1 ms) at prescaler 1024.
 */
uint32_t tb_target_period(void);

/* Deterministic pseudo-random numbers, so a failing run can be repeated */
uint32_t tb_rand(void);
void tb_rand_seed(uint32_t seed);

/*
 * Random delay of 0 to at least span_cpu CPU cycles, in single-cycle steps.
 * The CPU and the TCC can share a clock, so a delay loop alone reaches
 * only phases that are multiples of its iteration time. A run of 0 to 15
 * NOPs fills in the gaps.
 */
void tb_dither(uint32_t span_cpu);

void tb_wait_sync(void);

/*
 * Put the TCC in a known state without the driver: stop the counter,
 * write PER directly, clear COUNT and stale buffer-valid flags, restart.
 * The driver cannot reach a short period reliably, which is the bug
 * under test, so the tests use this instead.
 */
void tb_force_period(uint32_t per);

/*
 * Average period in CPU cycles over n overflows, timed from INTFLAG.OVF.
 * Returns 0 if an overflow takes more than 250 ms. Interrupts are locked
 * during the measurement.
 */
uint64_t tb_measure_period(uint32_t n);

/*
 * True if a measured period matches period_ticks within 2%. NPWM counts
 * PER + 1 ticks; the driver writes PER = period_ticks.
 */
bool tb_period_ok(uint64_t measured_cpu, uint32_t period_ticks);

/*
 * Wait for an INTFLAG flag. Returns the time in CPU cycles, or 0 on a
 * timeout. Clears the flag first if clear_first is true, and always
 * clears it when it sets. The poll is the same for every flag, so two
 * timestamps have the same delay.
 */
uint64_t tb_wait_flag(uint32_t mask, bool clear_first, uint64_t timeout_cpu);

/* tb_wait_flag() for the overflow flag INTFLAG.OVF */
uint64_t tb_wait_ovf(bool clear_first, uint64_t timeout_cpu);

/* Results that the tests record for the final report (src/report.c) */
#define TB_MAX_STARTS 10

struct tb_results {
	bool boot_ran;
	bool boot_ok;

	bool sweep_ran;
	uint32_t sweep_count;
	uint32_t sweep_start[TB_MAX_STARTS];
	uint32_t sweep_lost[TB_MAX_STARTS];
	uint32_t sweep_trials; /* for each start */

	bool shrink_ran;
	uint32_t shrink_worst_us;
	uint32_t shrink_limit_us;
	uint32_t shrink_failed;
	uint32_t shrink_trials;

	bool glitch_ran;
	int32_t glitch_delta_cpu; /* added to each period by an update */
	int32_t glitch_limit_cpu;

	bool duty_ran;
	uint32_t duty_period_normal; /* ticks */
	uint32_t duty_period_short;  /* ticks */
	uint32_t duty_overran;       /* trials the test could not judge */
	uint32_t duty_trials;        /* trials judged */
	uint32_t duty_lost;    /* the new pulse width did not take effect */
	uint32_t duty_skipped; /* a cycle had no compare match: output on all cycle */
};

extern struct tb_results tb_res;

/* Print the summary block. test_main() calls it after all suites run. */
void tb_report(void);

#endif /* TB_H_ */
