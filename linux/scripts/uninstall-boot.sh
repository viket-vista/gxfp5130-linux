#!/bin/bash
# Remove the DKMS installation and the boot autoload entry.
#
#   sudo ./scripts/uninstall-boot.sh
#
set -euo pipefail

PKG=gxfp5130
VER=0.1

if [ "$(id -u)" -ne 0 ]; then
	echo "ERROR: run with sudo" >&2
	exit 1
fi

rm -f /etc/modules-load.d/$PKG.conf
rmmod "$PKG" 2>/dev/null || true
dkms remove -m "$PKG" -v "$VER" --all || true
rm -rf "/usr/src/$PKG-$VER"
depmod -a

echo "removed $PKG (DKMS + autoload entry)"
