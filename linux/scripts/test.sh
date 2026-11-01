#!/bin/bash
# Load the gxfp5130 module, probe the sensor and sweep the wiring/mode
# variants until the firmware answers.  Must run as root:
#
#   sudo ./scripts/test.sh
#
# Output is also kept in /tmp/gxfp-test.log.

set -u
cd "$(dirname "$0")/.." || exit 1

OUT_DIR="$(cd "$(dirname "$0")/../.." && pwd)/out"
mkdir -p "$OUT_DIR"
LOG="$OUT_DIR/gxfp-test.log"
: > "$LOG"
chmod 644 "$LOG"
exec > >(tee -a "$LOG") 2>&1

echo "=== gxfp5130 bring-up test $(date) ==="
echo "kernel: $(uname -r)"

if [ "$(id -u)" -ne 0 ]; then
	echo "ERROR: run with sudo" >&2
	exit 1
fi

if [ ! -f gxfp5130.ko ] || [ ! -x tools/gxfp-tool ]; then
	echo "--- build ---"
	make -s || exit 1
	make -s -C tools || exit 1
fi

echo
echo "--- dmesg before the run (last 80 lines) ---"
dmesg | tail -80

echo
echo "--- ACPI device ---"
ls -d /sys/bus/acpi/devices/GXFP5130* 2>/dev/null || echo "GXFP5130 not found"

unload() { rmmod gxfp5130 2>/dev/null; }

#
# run_variant <label> [module parameters...]
# Returns 0 as soon as the firmware answered the version command.
#
run_variant() {
	local label="$1"; shift
	local rc

	echo
	echo "==============================================================="
	echo "=== variant: $label ${*:-（默认参数）}"
	echo "==============================================================="
	unload
	if ! insmod ./gxfp5130.ko probe_timeout_ms=300 "$@"; then
		echo "insmod failed"
		dmesg | tail -10
		return 1
	fi
	sleep 0.5

	echo "--- dmesg ---"
	dmesg | tail -12
	echo "--- info ---"
	./tools/gxfp-tool info

	for mode in 0 1 2; do
		echo 0 > /sys/module/gxfp5130/parameters/tx_mode 2>/dev/null
		echo "--- cmd a8, tx_mode=$mode ---"
		echo "$mode" > /sys/module/gxfp5130/parameters/tx_mode 2>/dev/null
		if ./tools/gxfp-tool cmd a8 "" 1200; then
			echo ">>> RESPONSE RECEIVED with tx_mode=$mode <<<"
			echo "--- mailbox after response ---"
			./tools/gxfp-tool peek 0x0 16
			./tools/gxfp-tool peek 0x200 32
			return 0
		fi
		echo "--- mailbox tx@0x0 ---"
		./tools/gxfp-tool peek 0x0 16
		echo "--- mailbox rx@0x200 ---"
		./tools/gxfp-tool peek 0x200 16
	done

	return 1
}

found=""
try() {
	local label="$1"; shift
	if [ -z "$found" ] && run_variant "$label" "$@"; then
		found="$label"
	fi
}

try "default (wr=1 rd=2)"
try "swap_done" swap_done=1
try "invert_done" invert_done=1
try "swap+invert" swap_done=1 invert_done=1
try "poll_only" poll_ms=20
try "poll+swap" poll_ms=20 swap_done=1
try "wr=2 only" gpio_wr_index=2 gpio_rd_index=99
try "wr=1 only" gpio_wr_index=1 gpio_rd_index=99
try "indices 2/1" gpio_wr_index=2 gpio_rd_index=1

echo
echo "==============================================================="
if [ -n "$found" ]; then
	echo "SUCCESS: firmware answered with variant: $found"
	echo "--- final info ---"
	./tools/gxfp-tool info
	echo "--- dmesg with frames ---"
	dmesg | tail -40
	echo "--- MCU state ---"
	./tools/gxfp-tool cmd ae 5500000000 800
	echo "--- late frames ---"
	./tools/gxfp-tool listen 4 500
else
	echo "NO RESPONSE in any variant"
fi
echo "--- dmesg (final) ---"
dmesg | tail -30
chmod 644 "$LOG" 2>/dev/null || true
echo "=== done, log saved to $LOG ==="
