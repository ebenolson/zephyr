#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Flash a UF2-bootloader board (Seeeduino XIAO) with the testbench.
# Works by hand or as a twister --flash-command:
#
#   uf2_flash.sh --build-dir DIR [--port GLOB] [--label LABEL] [--board-id ID]
#
# --port is a path or a glob. The script resolves it when it sends 'b',
# because the port name can change between enumerations. The default
# matches the Zephyr USB console on any board.
#
# Steps: send 'b' to the running testbench (CONFIG_TB_UF2_REBOOT), which
# reboots into the bootloader. Then wait for the UF2 drive, mount it, and
# write zephyr.uf2. If no testbench runs, double-tap reset by hand.

set -euo pipefail

build=""
port="/dev/serial/by-id/usb-*CDC_ACM_serial_backend*"
label="Arduino"
# Stay under twister's --device-flash-timeout (default 60 s)
timeout_s=50

while [ $# -gt 0 ]; do
	case "$1" in
	--build-dir) build="$2"; shift 2 ;;
	--port) port="$2"; shift 2 ;;
	--label) label="$2"; shift 2 ;;
	--board-id)
		# A hardware map can carry the console path as the board id
		case "$2" in /dev/*) port="$2" ;; esac
		shift 2 ;;
	*) echo "uf2_flash.sh: unknown argument $1" >&2; exit 2 ;;
	esac
done

uf2="$build/zephyr/zephyr.uf2"
[ -f "$uf2" ] || { echo "uf2_flash.sh: $uf2 not found" >&2; exit 1; }

dev="/dev/disk/by-label/$label"

if [ ! -e "$dev" ]; then
	# shellcheck disable=SC2086 # $port may be a glob
	tty="$(ls $port 2>/dev/null | head -1 || true)"
	if [ -n "$tty" ]; then
		echo "uf2_flash.sh: sending 'b' to $tty" >&2
		stty -F "$tty" 115200 raw -echo 2>/dev/null || true
		printf 'b' > "$tty" || true
	else
		echo "uf2_flash.sh: no console port matches $port" >&2
	fi
fi

echo "uf2_flash.sh: waiting up to ${timeout_s}s for $dev" >&2
for _ in $(seq 1 $((timeout_s * 5))); do
	[ -e "$dev" ] && break
	sleep 0.2
done
[ -e "$dev" ] || { echo "uf2_flash.sh: no UF2 drive; double-tap reset" >&2; exit 1; }

mnt="$(findmnt -n -o TARGET --source "$dev" || true)"
if [ -z "$mnt" ]; then
	udisksctl mount --no-user-interaction -b "$dev" >/dev/null
	mnt="$(findmnt -n -o TARGET --source "$dev")"
fi

# A plain cp can wedge the host USB stack; dd with oflag=sync does not
# The bootloader can reset before dd returns, so a dd error alone is not a
# failure. The check below, that the drive goes away, decides.
if ! dd if="$uf2" of="$mnt/flash.uf2" bs=4096 oflag=sync conv=fsync status=none; then
	echo "uf2_flash.sh: dd reported an error; checking for a reset" >&2
fi

# The bootloader resets into the new image and the drive goes away
for _ in $(seq 1 50); do
	[ -e "$dev" ] || exit 0
	sleep 0.2
done
echo "uf2_flash.sh: the UF2 drive is still present after the write" >&2
exit 1
