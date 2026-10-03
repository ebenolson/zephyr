# SAM0 TCC PWM testbench

This app tests `drivers/pwm/pwm_sam0_tcc.c` on real hardware. It runs on
SAMD21, SAMR21 and SAMC21 boards with no source changes and no instruments.

It supports Zephyr PR
[#115853](https://github.com/zephyrproject-rtos/zephyr/pull/115853) and issue
[#116310](https://github.com/zephyrproject-rtos/zephyr/issues/116310).

## The bug under test

On SAMD21 and SAMR21, a write to the buffered period register `PERB` can be
lost when it overlaps a TCC update condition. The period then does not
change.

The driver init sets `PER = 1`, so an update condition occurs every 2 ticks.
Most writes are then lost. `pwm_set()` returns 0, but the output is wrong.

The loss window scales with the TCC tick, so whether a fix works can depend
on the prescaler. The testbench runs every driver at prescaler 1 and 1024.

### Why SAMC21 is included

SAMC2x, SAML2x, SAMD5x and SAME5x use a newer TCC revision with `PERBUF`
and `CCBUF` registers, and a different branch of the same driver. A fix
for SAMD21/SAMR21 does not change that branch. A run on SAMC21 answers
two questions:

- Do buffered writes on the newer TCC get lost too? If they do, those
  families need a fix as well.
- Does a change to the driver leave the other families working?

A clean SAMC21 run is also a check on the method: the same tests that find
losses on SAMD21 report none on a chip without the defect.

## How the defect is detected

A lost write leaves the old period in use. So each test asks for a new
period through the PWM API, and then checks whether the counter really
runs at that period.

### Measuring the period that is in use

The test cannot read the period from the TCC. An APB read of `PER` returns
the last value the CPU wrote, not the value the counter uses. The test
times the counter instead:

1. Clear the overflow flag `INTFLAG.OVF`.
2. Poll the flag until the counter overflows. Record the time with
   `k_cycle_get_64()`.
3. Repeat for N overflows. The period in use is the elapsed time divided
   by N.

Interrupts are locked during the measurement, so an ISR cannot delay a
timestamp. N is set so that one measurement takes at most 50 ms.

In NPWM mode the counter overflows every `PER + 1` ticks. The driver writes
`PER = period`. So a write that took effect gives a period of `period + 1`
ticks. The test accepts a result within 2% of that value.

### One trial in the period-change test

1. Stop the counter. Write a short period S directly to `PER`, clear
   `COUNT`, and start the counter again. This puts the TCC in a known
   state. The driver cannot do this itself: it cannot reliably reach a
   short period, which is the defect.
2. Wait a random time of up to one period S, in steps of one CPU cycle.
   This puts the next write at a random point in the counter cycle.
3. Call `pwm_set_cycles()` with the target period T.
4. Wait for two periods S. A buffered write lands at the next update, so
   it has had time to take effect.
5. Measure the period in use.
   - About T + 1 ticks: the write took effect.
   - About S + 1 ticks: **the write was lost.**

The test runs 100 trials for each start period S. A short S means frequent
updates, so the loss is most likely at S = 1, the driver's init value. A
correct driver loses no write at any S.

### Why the random delay needs single-cycle steps

The loss happens only when the write overlaps an update, which is a short
window in each counter cycle. The CPU and the TCC can run from the same
clock. A plain delay loop then moves the write in steps of one loop pass,
and it reaches only some points in the cycle. Some start periods would
then show too many losses, and others too few. A run of 0 to 15 NOPs after
the loop fills in the gaps.

### The other tests

- **Boot:** the first `pwm_set()` after boot, from the driver's own init
  state. Same measurement. A lost write leaves the period at its init
  value.
- **Shorter period:** start at a long period. When `COUNT` is at 80% of
  it, ask for a short period. Measure the time to the next overflow. A
  buffered update arrives at the end of the current cycle. A driver that
  writes `PER` directly while `COUNT` is above the new value makes the
  counter run to its maximum value first. That takes far longer, and the
  test fails.
- **Glitch-free:** measure the period with no updates. Then call
  `pwm_set()` with unchanged values once per cycle, at a random point in
  the first half of the cycle, and measure again. If the driver stops or
  restarts the counter, each cycle gets longer. A difference of more than
  64 CPU cycles fails. This test uses a period of at least 8000 CPU
  cycles, so each update finishes before the next overflow. If the test
  still misses an overflow, it fails with "test overran" instead of
  reporting a false stretch.

- **Duty:** change only the pulse width, between 3/4 and 1/4 of the
  period, at a random point in the first 75% of the cycle. The test runs
  at a normal period, and at the shortest period the per-cycle
  measurement can follow (8 ticks at prescaler 1024), where update
  conditions are frequent. The output goes
  low at the compare match (`COUNT == CC`). The test uses the match flag
  `INTFLAG.MCx`:
  - No match between the two overflows around the update means the output
    stayed on for the whole cycle. This is a **skipped match**. It happens
    when a driver writes a lower `CC` directly after `COUNT` has passed it.
  - In the next cycle, the time from the overflow to the match gives the
    pulse width in use. If it does not fit the new pulse width, **the write
    was lost**.
  - If the cycle ends while the driver call runs (the overflow flag is
    already set when `pwm_set()` returns), the test cannot judge the
    trial and skips it. The report counts these. The test fails with
    "test overran" if it could not judge at least half of the trials. A
    driver that restarts the counter does not set the overflow flag, so a
    restart is judged normally.

### Prescaler

All periods and delays scale with the TCC tick. The target period is 1920
ticks (40 us) at prescaler 1, and 53 ticks (1.1 ms) at prescaler 1024.

## Tests

ztest runs the suites in name order, so the boot test runs first.

| Test | What it checks | Passes when |
|---|---|---|
| `tcc_1_boot` | The first `pwm_set()` after boot | The period matches the request |
| `tcc_2_period` | Period changes from short starting periods (1 tick and up), 100 each, at random points in the cycle | No change is lost |
| `tcc_3_shrink` | A change to a shorter period while `COUNT` is above it | The new period starts within 25% of the old period. A run to the counter wrap fails. |
| `tcc_4_glitch` | `pwm_set()` with unchanged values once per period, at a random point in the cycle | The PWM cycle stays the same length. A driver that stops or restarts the counter fails. That glitch affects every channel of the TCC. |
| `tcc_5_duty` | Pulse-width changes between 3/4 and 1/4 of the period, 200 times at a normal period and 200 times at a short one, at a random point in the cycle | Every change takes effect, and no cycle skips its compare match |

## Driver variants and prescalers

Each scenario is `sam0_tcc_pwm.<driver>.p<prescaler>`, with prescaler 1 or
1024. A variant replaces the in-tree driver with a fixed copy in
`variants/`, so one checkout can test every version.

| Driver | Source |
|---|---|
| `in_tree` | `drivers/pwm/pwm_sam0_tcc.c` in your `ZEPHYR_BASE` |
| `main` | Unpatched: writes `PERB`/`CCB` on the running counter |
| `lupd` | Holds `CTRLB.LUPD` across the `PERB`/`CCB` writes |
| `stop` | Stops the counter, writes `PER`/`CC` directly, restarts |
| `direct` | Writes `PER`/`CC` directly on the running counter. Restarts the cycle only if `COUNT` is already past the new period. |
| `hybrid` | Writes `PER` directly, and restarts the cycle when the period gets shorter (it compares with the old `PER` value, not `COUNT`). Writes `CC` through `CCB` with `CTRLB.LUPD` held, so the pulse width loads at the cycle boundary. |

The variants change only the SAMD21/SAMR21 branch, so on other families
they would all build the same code. Twister therefore runs them only on
SAMD21 and SAMR21 boards (a `filter` on the SoC series in
`testcase.yaml`). Other boards, such as `samc21n_xpro`, run only `in_tree`,
at both prescalers.

| Board family | Scenarios |
|---|---|
| SAMD21, SAMR21 | all 12: six drivers at prescaler 1 and 1024 |
| SAMC21 and other `PERBUF` boards | 2: `in_tree` at prescaler 1 and 1024 |

## Reading the output

At the end of each run, the testbench prints one result block:

```
================ RESULT ================
Board:      samr21_xpro/samr21g18a
Driver:     stop
Prescaler:  1024 (target period 53 ticks)

Works after boot:      yes
Period changes lost:   0 of 800
  by starting PER:     1:0 2:0 3:0 4:0 5:0 8:0 16:0 32:0  (of 100 each)
Shorter period works:  yes      (starts after 1100 us, limit 7300 us)
Glitch-free updates:   NO       (each update stretches the PWM cycle by ...)

VERDICT: WORKS, NOT GLITCH-FREE - every update took effect, but each update disturbs the PWM cycle
========================================
```

(The numbers above show the format only.)

The verdict is one of:

- `GOOD`: every update took effect, and updates are glitch-free.
- `WORKS, NOT GLITCH-FREE`: every update took effect, but each update
  disturbs the PWM cycle. ztest reports this as a failure.
- `BROKEN`: some updates did not take effect.
- `INCOMPLETE`: a test did not finish. Look at the ztest output above.

The `PROJECT EXECUTION` line from ztest is FAILED for any verdict except
`GOOD`.

To compare runs, collect the `TBSUM` lines into one Markdown table:

```sh
sam0-tcc-pwm-testbench/scripts/summarize.py console.log   # a saved console log
sam0-tcc-pwm-testbench/scripts/summarize.py ../twister-tb # a twister output directory
```

## Supported boards

| Board | TCC/channel | Pin | Console | Flash |
|---|---|---|---|---|
| `seeeduino_xiao` | TCC0/WO0 | PA4 (D1) | USB CDC ACM | UF2 (`scripts/uf2_flash.sh`) or J-Link SWD (`scripts/jlink_flash.sh`) |
| `samd21_xpro` | TCC0/WO0 | PB30 (LED0) | EDBG UART | `west flash` (OpenOCD) |
| `samr21_xpro` | TCC0/WO3 | PA19 (LED0) | EDBG UART | `west flash` (OpenOCD) |
| `samc21n_xpro` | TCC2/WO1 | PC5 (LED0) | EDBG UART | `west flash` (OpenOCD) |
| `arduino_zero` | TCC2/WO1 | PA17 (LED) | EDBG UART | `west flash` (OpenOCD) |

TCC2 is a 16-bit counter. TCC0 and TCC1 are 24-bit counters. No wiring is
needed on any board.

## Run it

Run these commands from the `zephyr` directory of a west workspace. The
commands assume this directory is at `zephyr/sam0-tcc-pwm-testbench`.

### Xplained Pro and Arduino Zero (EDBG)

```sh
west twister -T sam0-tcc-pwm-testbench -p samc21n_xpro \
  --device-testing --device-serial /dev/ttyACM0 -O ../twister-tb
sam0-tcc-pwm-testbench/scripts/summarize.py ../twister-tb
```

Twister builds, flashes and runs every scenario for the board: 2 on
`samc21n_xpro`, 12 on `samd21_xpro`, `samr21_xpro` and `arduino_zero`. Add
`-s sam0_tcc_pwm.in_tree.p1024` to run one scenario.

### Seeeduino XIAO (USB console)

The XIAO console is always USB CDC ACM. `cdc_console.py` reads it and
reopens it after every reset. You can flash with the UF2 bootloader or
with a J-Link on the SWD pads.

Flash with the UF2 bootloader:

```sh
west twister -T sam0-tcc-pwm-testbench -p seeeduino_xiao --device-testing \
  --device-serial-pty sam0-tcc-pwm-testbench/scripts/cdc_console.py \
  --flash-command "sam0-tcc-pwm-testbench/scripts/uf2_flash.sh" \
  -O ../twister-tb
```

- `uf2_flash.sh` sends `b` to the running testbench, which reboots into
  the UF2 bootloader. It finds the console port by its product name at
  that moment, because the full port name can change between
  enumerations. Then the script writes `zephyr.uf2` to the drive.
  If no testbench runs yet, double-tap reset once.
- The UF2 drive label defaults to `Arduino`. Use `--label` if yours
  differs.

Flash with a J-Link over SWD:

```sh
west twister -T sam0-tcc-pwm-testbench -p seeeduino_xiao --device-testing \
  --device-serial-pty sam0-tcc-pwm-testbench/scripts/cdc_console.py \
  --flash-command "sam0-tcc-pwm-testbench/scripts/jlink_flash.sh" \
  -O ../twister-tb
```

- `jlink_flash.sh` writes `zephyr.hex`. The image starts at 0x2000, so
  the UF2 bootloader stays in place.
- Add `,--serial,<SN>` inside the quotes if more than one J-Link is
  connected.
- No double-tap is needed, even for the first flash.

Both methods:

- The testbench waits up to 30 s for the host to open the console, then
  2 s more. The extra wait lets a flash script reboot an old image before
  it starts a suite.
- The app Kconfig lowers the USB driver log level. `udc_sam0.c` logs
  every IN transfer at INFO level, which floods a USB console.

### By hand

```sh
west build -b samd21_xpro sam0-tcc-pwm-testbench -- \
  -DEXTRA_CONF_FILE=variants/stop.conf -DDTS_EXTRA_CPPFLAGS=-DTB_PRESCALER=1024
west flash
```

Leave out `EXTRA_CONF_FILE` for the in-tree driver. On boards other than
SAMD21 and SAMR21, use only the in-tree driver. Leave out
`DTS_EXTRA_CPPFLAGS` for prescaler 1. For the XIAO, add
`-S cdc-acm-console` after the board name.

## Add a board

Add `boards/<board>.overlay`:

```dts
#include <zephyr/dt-bindings/pwm/pwm.h>

#ifndef TB_PRESCALER
#define TB_PRESCALER 1
#endif

/ {
	zephyr,user {
		pwms = <&tcc0 0 PWM_USEC(40)>;      /* TCC instance and channel */
	};
};

&tcc0 {
	prescaler = <TB_PRESCALER>;
};
```

If the board does not enable the TCC, add `pinctrl` and `status` as in
`boards/seeeduino_xiao.overlay`. Then add the board to `platform_allow` in
`testcase.yaml`. The driver variants run on the board only if it is a
SAMD21 or SAMR21.

## Report results

Send the output of `scripts/summarize.py`, or the RESULT block from each
run.
