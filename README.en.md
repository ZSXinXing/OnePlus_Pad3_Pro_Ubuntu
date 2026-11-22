# Native Ubuntu on OnePlus Pad 3 Pro

[简体中文](README.md)

An Ubuntu 26.04.1 LTS arm64 port for **OnePlus Pad 3 Pro (OPD2513, SM8850P)**. Native GNOME and an Ubuntu root filesystem on internal UFS have been tested on the development tablet using a temporary RAM boot. Images for other OnePlus tablets are not interchangeable, even if the devices share the same SoC.

This is a development source release. The November 23, 2026 SuperVOOC fix passed a short on-device charging run and the user confirmed normal charging. Standalone cold boot and full-cycle charging still need validation. Keep stock Recovery and its boot dependencies intact; charging in stock Recovery was tested on the development tablet.

## Hardware status

| Component | Status |
| --- | --- |
| Display and touch | Internal display and GNOME Wayland work; touch was used on the desktop. Pen, suspend/resume and individual refresh rates are still unverified. |
| CPU, memory and virtualization | All eight cores work and the CPU name is recognized. The usable-memory gap is unresolved. The tested kernel has no `/dev/kvm`; KVM is not enabled. |
| GPU | Adreno 840 hardware EGL rendering and GNOME Wayland were tested on hardware with matching signed firmware. Some telemetry is not exposed by the driver. |
| Storage and USB | Internal UFS root and USB maintenance access were tested. A working USB data connection does not establish charging. |
| Wi-Fi and Bluetooth | WCN786x drivers and board firmware were adapted in the development environment. BLE scanning was observed; stable reconnection, keyboard pairing and resume still need testing. |
| Internal audio | Microphone recording and speaker playback work. A test signal was detected for each of six amplifier paths; all eight physical speakers have not been verified individually. |
| Front and rear cameras | Integrated with the system camera app; rear frames were captured. Autofocus is implemented, but final sharpness and front-camera image quality need on-device checks. |
| Flashlight | The system-wide GNOME toggle is installed immediately left of Screenshot. Driver readback at the updated brightness passed; physical light output is not yet confirmed. |
| Charging | Original SuperVOOC charger: approximately 100 s, 58.29 W peak / 55.62 W mean at the pumps, +263.301 mAh net battery charge, with a sustained 6.1 A request. User confirmed normal charging. Ordinary USB, standard PD, full-cycle charging and cold boot remain unverified. |

These are development-device results. The baseline rootfs build does not automatically include every experimental change. See the [patch inventory](patches/README.md) for scope and baselines.

## Charging fix — November 23, 2026

The previous policy incorrectly applied a 44°C battery boundary to connector temperature, causing a 6.1 A to 3 A drop after about 14 seconds. The fix uses the existing 55°C connector exit threshold and retains battery temperature, voltage, SOC, overcurrent and pump-fault protections. The charger’s 67 W rating is not a constant-power guarantee. Reported power is pump VBUS times the sum of pump IBUS, rather than adapter input or net battery power.

See the [English charging guide](driver-fixes-20261119/supervooc-opd2513/README.en.md) for persistent rootfs installation, RAM-image packing, hashes, rollback and validation limits, and the [measurement record](driver-fixes-20261119/supervooc-opd2513/validation-20261123.json). Persistence tooling and a RAM image were prepared on the host; neither tablet installation nor restart validation was performed in this publication task. Use `fastboot boot` and preserve stock Recovery. Standalone cold boot remains unverified.

## Build and install

1. **Prepare the host and Recovery.** Use a Linux host with Fastboot, an ARM64 cross-compiler, `qemu-system-aarch64`, `squashfs-tools`, `cpio`, `zstd`, `e2fsprogs`, `device-tree-compiler`, and `ripgrep`. Reserve at least 20 GiB. Back up user data and confirm model OPD2513, working stock Recovery and adequate battery charge before a boot test.
2. **Prepare the inputs.** Verify the official Ubuntu 26.04.1 Desktop arm64 ISO. Extract `casper/minimal.squashfs` and `casper/minimal.standard.squashfs` into one directory and generate a `SHA256SUMS` file for both. Obtain matching firmware from a source you may use. The input paths are listed below; do not substitute another model's calibration data.
3. **Build the reference kernel and rootfs.** Run the [shared build commands](README.md#构建与安装). They check out the pinned kernel, package its modules, and assemble `ubuntu-desktop-root.ext4` in an ARM64 guest. Confirm `OPD2513_DESKTOP_ASSEMBLY_OK` in `assembly.log` and verify `SHA256SUMS`. The ext4 file is a **root filesystem image, not a Fastboot boot image**.
4. **Test with a temporary boot only.** This requires a separately built, verified OPD2513 RAM-boot image and the root filesystem that image expects. Set `FASTBOOT_SERIAL` to the actual device selector and `OPD2513_RAM_BOOT_IMG` to the verified RAM image, then run `fastboot -s "$FASTBOOT_SERIAL" boot "$OPD2513_RAM_BOOT_IMG"`. No ready-made RAM image or generic disk installer is included here. Permanent installation instructions will follow standalone boot, charging, and stock Recovery regression tests. Do not flash this ext4 image to `boot`, `recovery`, or the whole disk.

Local firmware inputs: `local-firmware/novatek/novatek_nt36536_fw.bin`, `local-firmware/qca/{gngbtfw20.mbn,gngbtnv20.bin}`, and `local-firmware/qcom/kaanapali/iceland/{gen80200_zap.mbn,adsp.mbn,adsp_dtb.mbn,cdsp.mbn,cdsp_dtb.mbn}`. The build checks the expected SHA-256 for signed ZAP firmware. Proprietary firmware, partition backups and built images are not distributed here.

On a running Ubuntu installation with `/sys/class/leds/white:flash-1/brightness`, the [system flashlight toggle](driver-fixes-20261119/torch-quick-settings/README.md) can be installed separately. It loads on the next GNOME login and is included by default in newly built root filesystems.
