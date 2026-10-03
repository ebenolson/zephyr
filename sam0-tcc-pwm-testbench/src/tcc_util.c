/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Register access and measurement helpers. All chip details come from
 * devicetree and the HAL headers, so the same code runs on every TCC
 * revision and at every prescaler.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "tb.h"

const struct pwm_dt_spec tb_spec = PWM_DT_SPEC_GET(TB_USER_NODE);
Tcc *const tb_tcc = (Tcc *)DT_REG_ADDR(TB_TCC_NODE);

static uint32_t rand_state = 0x2545F491U;

const char *tb_driver_name(void)
{
	if (IS_ENABLED(CONFIG_TB_DRIVER_MAIN)) {
		return "main";
	} else if (IS_ENABLED(CONFIG_TB_DRIVER_LUPD)) {
		return "lupd";
	} else if (IS_ENABLED(CONFIG_TB_DRIVER_STOP)) {
		return "stop";
	} else if (IS_ENABLED(CONFIG_TB_DRIVER_DIRECT)) {
		return "direct";
	} else if (IS_ENABLED(CONFIG_TB_DRIVER_HYBRID)) {
		return "hybrid";
	}
	return "in_tree";
}

uint64_t tb_cpu_hz(void)
{
	return sys_clock_hw_cycles_per_sec();
}

uint64_t tb_tcc_hz(void)
{
	uint64_t hz = 0U;

	(void)pwm_get_cycles_per_sec(tb_spec.dev, tb_spec.channel, &hz);
	__ASSERT_NO_MSG(hz != 0U);
	return hz;
}

uint64_t tb_ticks_to_cpu(uint64_t ticks)
{
	uint64_t tcc_hz = tb_tcc_hz();

	return tcc_hz != 0U ? ticks * tb_cpu_hz() / tcc_hz : 0U;
}

uint32_t tb_target_period(void)
{
	uint64_t cpu_per_tick = MAX(tb_cpu_hz() / tb_tcc_hz(), 1U);

	return MAX(53U, (uint32_t)DIV_ROUND_UP(1920U, cpu_per_tick));
}

uint32_t tb_rand(void)
{
	/* xorshift32 */
	rand_state ^= rand_state << 13;
	rand_state ^= rand_state >> 17;
	rand_state ^= rand_state << 5;
	return rand_state;
}

void tb_rand_seed(uint32_t seed)
{
	rand_state = (seed != 0U) ? seed : 1U;
}

static void spin(uint32_t n)
{
	for (volatile uint32_t i = 0; i < n; i++) {
	}
}

void tb_dither(uint32_t span_cpu)
{
	uint32_t r = tb_rand();

	/* One loop pass takes more than 4 CPU cycles, so this covers span_cpu */
	spin((r >> 4) % (span_cpu / 4U + 1U));

	/* Enter the NOP run at a random point: 0 to 15 single-cycle steps */
	switch (r & 0xFU) {
	case 15: arch_nop(); __fallthrough;
	case 14: arch_nop(); __fallthrough;
	case 13: arch_nop(); __fallthrough;
	case 12: arch_nop(); __fallthrough;
	case 11: arch_nop(); __fallthrough;
	case 10: arch_nop(); __fallthrough;
	case 9: arch_nop(); __fallthrough;
	case 8: arch_nop(); __fallthrough;
	case 7: arch_nop(); __fallthrough;
	case 6: arch_nop(); __fallthrough;
	case 5: arch_nop(); __fallthrough;
	case 4: arch_nop(); __fallthrough;
	case 3: arch_nop(); __fallthrough;
	case 2: arch_nop(); __fallthrough;
	case 1: arch_nop(); __fallthrough;
	default:
		break;
	}
}

void tb_wait_sync(void)
{
	while (tb_tcc->SYNCBUSY.reg != 0U) {
	}
}

void tb_force_period(uint32_t per)
{
	tb_tcc->CTRLA.bit.ENABLE = 0;
	tb_wait_sync();
	tb_tcc->PER.reg = per;
	tb_tcc->CC[tb_spec.channel].reg = per / 2U;
	tb_tcc->COUNT.reg = 0U;
	tb_wait_sync();
	tb_tcc->STATUS.reg = TB_STATUS_PERBV | TB_STATUS_CCBV(tb_spec.channel);
	tb_tcc->CTRLA.bit.ENABLE = 1;
	tb_wait_sync();
}

uint64_t tb_wait_flag(uint32_t mask, bool clear_first, uint64_t timeout_cpu)
{
	uint64_t deadline = k_cycle_get_64() + timeout_cpu;
	uint32_t n = 0U;

	if (clear_first) {
		tb_tcc->INTFLAG.reg = mask;
	}
	while ((tb_tcc->INTFLAG.reg & mask) == 0U) {
		/* Read the clock only now and then, to keep the poll tight */
		if ((++n & 0xFFU) == 0U && k_cycle_get_64() > deadline) {
			return 0U;
		}
	}
	tb_tcc->INTFLAG.reg = mask;
	return k_cycle_get_64();
}

uint64_t tb_wait_ovf(bool clear_first, uint64_t timeout_cpu)
{
	return tb_wait_flag(TCC_INTFLAG_OVF, clear_first, timeout_cpu);
}

uint64_t tb_measure_period(uint32_t n)
{
	/* Under the SysTick wrap (about 349 ms), so the lock stays safe */
	uint64_t timeout = tb_cpu_hz() / 4U;
	uint64_t t0;
	uint64_t t1 = 0U;
	unsigned int key;

	/* An ISR at t0 or t1 would skew the result; keep them out */
	key = irq_lock();

	/* Discard any stale flag, then start timing on a fresh overflow */
	t0 = tb_wait_ovf(true, timeout);
	for (uint32_t i = 0; i < n && t0 != 0U; i++) {
		t1 = tb_wait_ovf(false, timeout);
		if (t1 == 0U) {
			t0 = 0U;
		}
	}

	irq_unlock(key);
	return (t0 != 0U) ? (t1 - t0) / n : 0U;
}

bool tb_period_ok(uint64_t measured_cpu, uint32_t period_ticks)
{
	uint64_t expected = tb_ticks_to_cpu((uint64_t)period_ticks + 1U);
	uint64_t tol = expected / 50U;

	return measured_cpu + tol >= expected && measured_cpu <= expected + tol;
}
