/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Reboot into the Adafruit UF2 bootloader when 'b' arrives on the
 * console. scripts/uf2_flash.sh uses this to flash without a physical
 * double-tap.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/sys_io.h>

/* Double-tap magic in the last word of a 32 KB SRAM */
#define DBL_TAP_PTR   0x20007FFCUL
#define DBL_TAP_MAGIC 0xF01669EFUL

static void uf2_reboot_thread(void *p1, void *p2, void *p3)
{
	const struct device *cons = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	unsigned char ch;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		while (uart_poll_in(cons, &ch) == 0) {
			if (ch == 'b') {
				sys_write32(DBL_TAP_MAGIC, DBL_TAP_PTR);
				sys_reboot(SYS_REBOOT_COLD);
			}
		}
		k_msleep(50);
	}
}

/*
 * Higher priority than the test thread (CONFIG_ZTEST_THREAD_PRIORITY), so
 * a flash script can stop a running suite. Measurements lock interrupts,
 * so this thread cannot disturb them.
 */
BUILD_ASSERT(CONFIG_ZTEST_THREAD_PRIORITY > 1, "test thread must be preemptible");
K_THREAD_DEFINE(uf2_reboot, 512, uf2_reboot_thread, NULL, NULL, NULL, 1, 0, 0);
