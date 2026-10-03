#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Flash the testbench with a SEGGER J-Link over SWD. Works by hand or as a
# twister --flash-command:
#
#   jlink_flash.sh --build-dir DIR [--device NAME] [--serial SN] [--board-id SN]
#
# It writes zephyr.hex. On boards with a UF2 bootloader, such as the XIAO,
# the image starts at 0x2000, so the bootloader stays in place.
#
# The script resets and halts the core before it writes. A core that sleeps
# in WFI can gate the SWD clock and make the erase fail.

set -euo pipefail

build=""
device="ATSAMD21G18A"
serial=""

while [ $# -gt 0 ]; do
	case "$1" in
	--build-dir) build="$2"; shift 2 ;;
	--device) device="$2"; shift 2 ;;
	--serial) serial="$2"; shift 2 ;;
	--board-id)
		# A hardware map can carry the J-Link serial number as the board id
		case "$2" in *[!0-9]*) ;; *) serial="${serial:-$2}" ;; esac
		shift 2 ;;
	*) echo "jlink_flash.sh: unknown argument $1" >&2; exit 2 ;;
	esac
done

hex="$build/zephyr/zephyr.hex"
[ -f "$hex" ] || { echo "jlink_flash.sh: $hex not found" >&2; exit 1; }

script="$(mktemp)"
trap 'rm -f "$script"' EXIT
cat > "$script" <<EOS
r
h
loadfile $hex
r
g
q
EOS

args=(-NoGui 1 -device "$device" -if SWD -speed 4000 -autoconnect 1 -ExitOnError 1
      -CommanderScript "$script")
if [ -n "$serial" ]; then
	args=(-USB "$serial" "${args[@]}")
fi

JLinkExe "${args[@]}"
