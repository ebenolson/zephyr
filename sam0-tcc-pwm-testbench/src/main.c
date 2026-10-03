/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * SAM0 TCC PWM testbench. See README.md.
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <zephyr/drivers/uart.h>

#include "tb.h"

#define CONSOLE_NODE DT_CHOSEN(zephyr_console)

/*
 * A USB CDC ACM console drops output until the host opens the port. After
 * the port opens, wait 2 s more. A flash script that opened the port to
 * send 'b' then reboots this image before it starts a suite, so twister
 * does not record results from an image it is about to replace.
 */
static void wait_for_console(void)
{
#if DT_NODE_HAS_COMPAT(CONSOLE_NODE, zephyr_cdc_acm_uart) && defined(CONFIG_UART_LINE_CTRL)
	const struct device *cons = DEVICE_DT_GET(CONSOLE_NODE);
	uint32_t dtr = 0U;

	for (int i = 0; i < 300 && dtr == 0U; i++) {
		(void)uart_line_ctrl_get(cons, UART_LINE_CTRL_DTR, &dtr);
		k_msleep(100);
	}
	k_msleep(2000);
#endif
}

void test_main(void)
{
	wait_for_console();

	TC_PRINT("TB board=%s driver=%s prescaler=%u\n", CONFIG_BOARD_TARGET,
		 tb_driver_name(), TB_PRESCALER);
	TC_PRINT("TB %s\n", TB_FAMILY);
	TC_PRINT("TB tcc=0x%08x channel=%u counter_size=%u tcc_hz=%u cpu_hz=%u target=%u ticks\n",
		 (uint32_t)(uintptr_t)tb_tcc, tb_spec.channel, TB_COUNTER_SIZE,
		 (uint32_t)tb_tcc_hz(), (uint32_t)tb_cpu_hz(), tb_target_period());

	ztest_run_all(NULL, false, 1, 1);
	tb_report();
	ztest_verify_all_test_suites_ran();
}
