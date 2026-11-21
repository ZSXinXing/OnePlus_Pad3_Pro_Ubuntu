# Native Ubuntu on OnePlus Pad 3 Pro

[简体中文](README.md)

An Ubuntu 26.04.1 LTS arm64 port for **OnePlus Pad 3 Pro (OPD2513, SM8850P)**. Native GNOME and an Ubuntu root filesystem on internal UFS have been tested on the development tablet using a temporary RAM boot. Images for other OnePlus tablets are not interchangeable, even if the devices share the same SoC.

This is a development source release, not a production installer. Standalone cold boot and charging remain blockers for a supported permanent installation. Keep stock Recovery and its boot dependencies intact; charging in stock Recovery was tested on the development tablet.

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
| Charging | Battery telemetry and some USB/charger negotiation are visible. Reliable net charging via ordinary USB, PD or the original fast charger is not established under Ubuntu. |

These are development-device results. The baseline rootfs build does not automatically include every experimental change. See the [patch inventory](patches/README.md) for scope and baselines.

## Build and install

1. **Prepare the host and Recovery.** Use a Linux host with Fastboot, an ARM64 cross-compiler, `qemu-system-aarch64`, `squashfs-tools`, `cpio`, `zstd`, `e2fsprogs`, `device-tree-compiler`, and `ripgrep`. Reserve at least 20 GiB. Back up user data and confirm model OPD2513, working stock Recovery and adequate battery charge before a boot test.
2. **Prepare the inputs.** Verify the official Ubuntu 26.04.1 Desktop arm64 ISO. Extract `casper/minimal.squashfs` and `casper/minimal.standard.squashfs` into one directory and generate a `SHA256SUMS` file for both. Obtain matching firmware from a source you may use. The input paths are listed below; do not substitute another model's calibration data.
3. **Build the reference kernel and rootfs.** Run the [shared build commands](README.md#构建与安装). They check out the pinned kernel, package its modules, and assemble `ubuntu-desktop-root.ext4` in an ARM64 guest. Confirm `OPD2513_DESKTOP_ASSEMBLY_OK` in `assembly.log` and verify `SHA256SUMS`. The ext4 file is a **root filesystem image, not a Fastboot boot image**.
4. **Test with a temporary boot only.** This requires a separately built, verified OPD2513 RAM-boot image and the root filesystem that image expects. Set `FASTBOOT_SERIAL` to the actual device selector and `OPD2513_RAM_BOOT_IMG` to the verified RAM image, then run `fastboot -s "$FASTBOOT_SERIAL" boot "$OPD2513_RAM_BOOT_IMG"`. No ready-made RAM image or generic disk installer is included here. Permanent installation instructions will follow standalone boot, charging, and stock Recovery regression tests. Do not flash this ext4 image to `boot`, `recovery`, or the whole disk.

Local firmware inputs: `local-firmware/novatek/novatek_nt36536_fw.bin`, `local-firmware/qca/{gngbtfw20.mbn,gngbtnv20.bin}`, and `local-firmware/qcom/kaanapali/iceland/{gen80200_zap.mbn,adsp.mbn,adsp_dtb.mbn,cdsp.mbn,cdsp_dtb.mbn}`. The build checks the expected SHA-256 for signed ZAP firmware. Proprietary firmware, partition backups and built images are not distributed here.

On a running Ubuntu installation with `/sys/class/leds/white:flash-1/brightness`, the [system flashlight toggle](driver-fixes-20261119/torch-quick-settings/README.md) can be installed separately. It loads on the next GNOME login and is included by default in newly built root filesystems.
