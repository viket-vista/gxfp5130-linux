#!/bin/bash
# Dump ACPI tables to /tmp/gxfp-acpi so the DSDT can be inspected without
# keeping root access.  Run as root:
#
#   sudo ./scripts/dump-acpi.sh
#
set -eu
OUT=${1:-$(cd "$(dirname "$0")/.." && pwd)/../acpi}

if [ "$(id -u)" -ne 0 ]; then
	echo "ERROR: run with sudo" >&2
	exit 1
fi

mkdir -p "$OUT"
cd "$OUT"
rm -f ./*.dat

if command -v acpidump >/dev/null 2>&1; then
	acpidump -b
else
	echo "acpidump not found; install the 'acpica-tools' package" >&2
	exit 1
fi

chmod -R a+rX "$OUT"
if [ -n "${SUDO_UID:-}" ]; then
	chown -R "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" "$OUT"
fi
ls -l "$OUT"
echo
echo "ACPI tables dumped to $OUT"
