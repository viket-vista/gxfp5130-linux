#!/bin/bash
# Integrate the GXFP5130 with fprintd:
#   - build the userspace stack (userspace/) and the libfprint fork
#   - install libfprint to /usr/local (distro package stays untouched)
#   - allow /dev/gxfp in the fprintd systemd sandbox
#   - restart fprintd
#
#   sudo ./scripts/install-fprintd.sh
#
# Prerequisites (see README): cmake, meson, ninja, libmbedtls-dev,
# libgusb-dev, libudev-dev, libopencv-dev, doctest-dev.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
USERSPACE="$ROOT/userspace"
LIBFP="$USERSPACE/libfprint"

if [ "$(id -u)" -ne 0 ]; then
	echo "ERROR: run with sudo" >&2
	exit 1
fi

echo "--- userspace library + demos ---"
cmake -S "$USERSPACE" -B "$USERSPACE/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$USERSPACE/build" -j"$(nproc)" >/dev/null

echo "--- libfprint fork (gxfp driver only) ---"
if [ ! -f "$LIBFP/build-lf/build.ninja" ]; then
	meson setup "$LIBFP/build-lf" -Ddrivers=gxfp -Dintrospection=false \
		-Ddoc=false --prefix=/usr/local -Dlibdir=lib
fi
ninja -C "$LIBFP/build-lf" >/dev/null
meson install -C "$LIBFP/build-lf" >/dev/null
ldconfig

echo "--- fprintd sandbox: allow /dev/gxfp ---"
mkdir -p /etc/systemd/system/fprintd.service.d
cat > /etc/systemd/system/fprintd.service.d/10-gxfp.conf <<'CONF'
[Service]
DeviceAllow=/dev/gxfp rw
CONF
systemctl daemon-reload
systemctl restart fprintd
sleep 1

echo
echo "libfprint in use:"
ldd /usr/libexec/fprintd 2>/dev/null | grep -i fprint || true
echo
fprintd-list "${SUDO_USER:-$USER}" 2>&1 | head -5 || true
echo
echo "Next: place a finger on the sensor and run"
echo "  fprintd-enroll -f right-index-finger \$USER"
