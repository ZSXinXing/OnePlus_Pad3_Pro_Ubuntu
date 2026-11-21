# OPD2513 GNOME flashlight

GNOME Shell 50.x installs one round toggle immediately to the left of the
screenshot button. The button uses the white LED in torch mode at brightness
level 26 of 255 (50 mA nominal total), not the camera strobe. The original
level 1 read back successfully but the user observed no light. The button
reads the driver state when the menu opens and while it is open. The hardware
starts off after a reboot; the button and its default enablement persist.

The root-owned systemd socket is writable by `video` members (including the
tablet's desktop user). The helper accepts only `on` / `off`; its service
writes only the fixed white LED brightness node and verifies the result.
The socket starts independently of driver probe; the extension hides the
button if the driver node is missing.

## Installation and builds

- Existing Ubuntu installation: run `sudo sh install.sh` from this directory.
  It installs system-wide, starts/enables the socket and appends the extension
  to the live user's existing list. An already-started Wayland session may
  only discover the new system extension at the next login.
- New desktop rootfs: `assemble-desktop-rootfs.sh` stages this bundle and
  calls `install.sh --root /target` in the ARM64 assembly guest. The baseline
  has `leds_qcom_flash`; the assembly applies `opd2513-flash.dtso` to the
  reference DTB with `dtc` and `fdtoverlay` (`DTC` and `FDTOVERLAY` can point
  to the host tools in an existing kernel build). For hardware operation the
  Ubuntu boot path must actually use a DT with the flash node enabled. The
  independently assembled RAM loader already uses the matching flash config.
- Updates of an existing rootfs retain the installed files. The update init
  script can also install the bundle when `/payload/torch/` is present; that
  hook does not change the bootloader or the device tree selected at boot.

## Verification and rollback

`python3 -B test_protocol.py` covers the socket protocol. Isolated GNOME 50.1
with a substituted LED verified button order, highlight and state on repeated
panel opens. The user confirmed the button on the tablet. After replacing
the helper, a brief on/off cycle as `jixing` read back `26` then `0`, with no
flash fault reported. Actual light output at the new level is still pending;
the test ended with the light off at 3% battery. No stock or Ubuntu boot
partition was changed.

The initial ARM64 candidate passed assembly, `e2fsck -fn`, and SHA-256
verification. Offline readback
confirmed the extension, helper, socket and service, socket enablement,
compiled GNOME default, and flash-enabled reference DTB. This is a rootfs
artifact, not a fastboot image; it was not flashed or booted on the tablet.
It predates the brightness correction; rebuild to include the new helper.

To disable the feature for the current user, switch the light off with the
button (or `/usr/local/libexec/opd2513-torch off`), then run
`gnome-extensions disable opd2513-torch@local`. To remove it system-wide,
disable/stop `opd2513-torch.socket`, remove the two `opd2513-torch` unit files,
the helper, the extension directory and `90-opd2513-torch.gschema.override`,
then recompile `/usr/share/glib-2.0/schemas` and reload systemd. The stock
Recovery is not involved.
