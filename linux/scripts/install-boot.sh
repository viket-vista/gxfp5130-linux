#!/bin/bash
# Install the gxfp5130 module through DKMS so it is rebuilt automatically on
# kernel upgrades and loaded at boot.
#
#   sudo ./scripts/install-boot.sh
#
set -euo pipefail

SRC=$(cd "$(dirname "$0")/.." && pwd)
PKG=gxfp5130
VER=0.1
DKMS_SRC=/usr/src/$PKG-$VER

if [ "$(id -u)" -ne 0 ]; then
	echo "ERROR: run with sudo" >&2
	exit 1
fi

echo "--- installing sources to $DKMS_SRC ---"
mkdir -p "$DKMS_SRC"
install -m 644 "$SRC/gxfp5130.c" "$SRC/gxfp5130_proto.h" \
	"$SRC/gxfp5130_uapi.h" "$SRC/Makefile" "$SRC/dkms.conf" "$DKMS_SRC/"

echo "--- dkms add/build/install ---"
if dkms status -m "$PKG" -v "$VER" 2>/dev/null | grep -q .; then
	dkms remove -m "$PKG" -v "$VER" --all || true
fi
dkms add -m "$PKG" -v "$VER"
dkms build -m "$PKG" -v "$VER"
dkms install -m "$PKG" -v "$VER" --force

echo "--- autoload at boot ---"
echo "$PKG" > /etc/modules-load.d/$PKG.conf

echo "--- switch from the workspace build to the DKMS build ---"
rmmod "$PKG" 2>/dev/null || true
modprobe "$PKG"
sleep 0.6

echo
echo "installed module: $(modinfo -F filename "$PKG")"
ls -l /dev/gxfp 2>/dev/null || true
"$SRC/tools/gxfp-tool" info 2>/dev/null || true

echo
echo "DKMS status:"
dkms status -m "$PKG" -v "$VER"
echo
echo "The module will now be loaded automatically at boot via"
echo "/etc/modules-load.d/$PKG.conf and rebuilt by DKMS on kernel upgrades."
