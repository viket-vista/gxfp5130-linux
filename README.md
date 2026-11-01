# gxfp5130-linux

Linux support for the **Goodix GXFP5130** fingerprint sensor found in Huawei
MateBook laptops (ACPI `GXFP5130`). The sensor is not on a normal SPI/I2C bus:
the Nuvoton EC exposes a 4 KiB eSPI mailbox in host MMIO plus two strobe lines
and a doorbell interrupt, and the fingerprint MCU sits behind the EC.

This repository contains everything needed end-to-end:

```
kernel module (eSPI mailbox transport, /dev/gxfp)
        │  raw eSPI frames / 0xE00-byte RX window records
        ▼
userspace stack (gxfpmoc: TLS-PSK session, capture, enrolment)
        │  FpImage / FpPrint
        ▼
libfprint fork (GXFP driver + SIGFM matcher)  →  fprintd / PAM
```

## Status

Verified on a Huawei MateBook with ACPI `GXFP5130:00`:

| Item | Result |
| --- | --- |
| Mailbox | `0xFE800000`, 4096 bytes, TX `0x000` / RX `0x200` |
| write-done GPIO | ACPI GpioIo #1, pad 387 (gpio 899) |
| read-done GPIO | ACPI GpioIo #2, pad 171 (gpio 683) |
| Interrupt | ACPI GpioInt, pad 133 (IRQ 162/183) |
| Firmware | `GF_GCC_EC_20068` (chip id `0x2504`) |
| TLS | PSK session established, `tls_connected=1` |
| fprintd | device enumerated, enrol/verify working |
| sudo | fingerprint authentication working |
| KDE lock screen | working via the `kde` PAM stack (see caveats) |

Detailed reverse-engineering notes (frame formats, timing, ACPI/DSTD evidence,
command table) are in [`linux/docs/REVERSE-ENGINEERING.md`](linux/docs/REVERSE-ENGINEERING.md).

## Repository layout

```
linux/                 kernel module + diagnostics + docs
  gxfp5130.c             eSPI mailbox transport driver
  gxfp5130_uapi.h        /dev/gxfp ABI
  gxfp5130_proto.h       frame codec (shared with host tests)
  dkms.conf, Makefile, scripts/, tools/, tests/, docs/
userspace/             GPL-2.0 fork of Void755/gxfpmoc
  src/, include/, demos/ TLS-PSK session, capture, PSK provisioning
  kernel/                reference kernel ABI this userspace expects
  libfprint/             fork of libfprint with the GXFP driver + SIGFM
```

## Build and install

Kernel module (DKMS, loaded at boot):

```bash
cd linux
sudo ./scripts/install-boot.sh
```

Userspace library and demos:

```bash
sudo apt install cmake libmbedtls-dev
cd userspace
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
sudo ./build/gxfp_info          # chip id / firmware / MCU state
```

libfprint fork and fprintd integration (Debian/Ubuntu):

```bash
sudo apt install meson ninja-build libgusb-dev libudev-dev systemd-dev \
                 libopencv-dev doctest-dev
sudo ../linux/scripts/install-fprintd.sh
```

Then enrol and verify:

```bash
fprintd-enroll -f right-index-finger "$USER"
fprintd-verify "$USER"
```

## PSK

The MCU uses TLS 1.2 with a 32-byte pre-shared key. **No key is included in this
repository.** Two supported ways to obtain one:

1. **Keep the Windows pairing** — run `userspace/tools/unseal_bb010002.py` on
   Windows; the cache at `C:\ProgramData\Goodix\Goodix_Cache.bin` is protected
   with machine-scope DPAPI, so the script can unseal it while running as that
   machine's administrator. It writes `psk_raw32.bin`.
2. **Re-provision from Linux** — `gxfp_psk_tool --build-bb010002` then
   `--upload-bb010002`. This replaces the key on the sensor; Windows re-pairs
   automatically the next time it uses the sensor.

Install the key:

```bash
sudo install -m 600 -o root -g root psk_raw32.bin /var/lib/fprintd/gxfp/psk_raw32.bin
```

## KDE / PAM notes

The stack is configured so that a failed fingerprint attempt always falls back
to the password quickly:

```
# /etc/pam.d/sudo
auth  sufficient  pam_fprintd.so timeout=8 max-tries=1

# /etc/pam.d/kde
auth    sufficient      pam_fprintd.so timeout=8 max-tries=3
@include common-auth
```

KDE's `kscreenlocker` also ships its own `kde-fingerprint` PAM service. On this
machine that stack shows a second, non-functional fingerprint hint and competes
for the device, so it is disabled:

```bash
sudo mv /etc/pam.d/kde-fingerprint /etc/pam.d/kde-fingerprint.disabled-gxfp
```

## Credits and prior art

- Userspace (`userspace/src`, `userspace/include`, `userspace/demos`) and the
  kernel ABI it expects (`userspace/kernel`) come from
  [Void755/gxfpmoc](https://github.com/Void755/gxfpmoc) (GPL-2.0).
- `userspace/libfprint` is a fork of [libfprint](https://gitlab.freedesktop.org/libfprint/libfprint)
  with the GXFP driver and SIGFM matcher (LGPL-2.1+), with a local fix in
  `fpi-ssm.c` to cancel pending delayed state changes on completion.
- The kernel driver in `linux/` is an independent implementation derived from
  protocol reverse engineering of the vendor's Windows UMDF driver
  (`gfspi.dll`, version 1.1.18.25). The same protocol was independently
  described in the public GXFP5130 LKML patch series and the
  [gxfp5130-linux](https://github.com/Han-Haocheng/gxfp5130-linux) packaging
  repository; those were used as cross-checks, not copied.

## License

- `linux/` — GPL-2.0
- `userspace/` — GPL-2.0 (see `userspace/LICENSE`)
- `userspace/libfprint/` — LGPL-2.1+ (see `userspace/libfprint/COPYING`)

## Disclaimer

Experimental, no warranty. The sensor is shared with the EC and with Windows on
dual-boot machines; re-provisioning the PSK changes the pairing. Keep a password
login path available when enabling fingerprint authentication for `sudo` or the
lock screen.
