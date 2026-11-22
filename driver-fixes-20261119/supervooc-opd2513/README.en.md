# OPD2513 SuperVOOC charging fix

This driver targets OnePlus Pad 3 Pro OPD2513 with kernel `7.3.0-opd2513-fwzstd+`. See [the Chinese engineering log](README.md) for the investigation, OEM references and historical trials.

## Verified result — November 23, 2026

The previous policy mistakenly applied a 44°C battery-temperature boundary to connector temperature. At battery 27°C and connector 44.236°C it abruptly changed the adapter request from 6.1 A to 3 A. The measured current then exceeded the lower limit and charging stopped. The fix uses the existing 55°C connector guard separately from battery temperature.

After the fix, the original charger ran for approximately 99.503 seconds. Battery SOC rose from 56% to 58%, with a net charge-counter gain of 263.301 mAh. Across 98 active dual-pump samples, measured pump input averaged 55.621 W and peaked at 58.287 W. The last sample still requested 6.1 A, past the previous approximately 14-second failure point. The user confirmed normal charging. See [the measurement summary](validation-20261123.json).

Power is calculated from pump VBUS multiplied by the sum of the two pump IBUS readings. It is distinct from charger AC input, charger rated power, and net battery power. This run does not establish constant 67 W or a full charging cycle. The driver retains connector, battery temperature, voltage, SOC, overcurrent and pump-fault protections, including the 80% SOC exit and 30-minute session limit. Standard PD and ordinary USB charging require separate validation.

## Reproduce the tested configuration

Module SHA-256:
`a5d65b3215bf62591caea5f2442dcb0f3ad0258d4984f6478434b67474027377`

ELF Build ID:
`00a26bd05187a5b1157e5b6decf6440c776d88d5`

Use matching battery-module symbols and the board DT with both pumps and temperature ADC channels enabled. Build this external module against the matching prepared kernel tree using `KBUILD_EXTRA_SYMBOLS` for the updated `qcom_battmgr` symbols. The tested profile explicitly enables:

```text
powered_trial=1 dual_trial=1 oem_policy=1 oem_first_3a=1
oem_high_band_first_6a1=1 oem_normal_high=1 svooc_skip_soc_step=1
```

The normal-low override skips the stock 55% plug-in SOC current step within the remaining guarded operating envelope. It is opt-in; it does not disable temperature or battery protection.

Run the host checks from the repository root:

```bash
for name in logic stage3b oem_policy ufcs_query; do
  cc -O2 -Idriver-fixes-20261119/supervooc-opd2513 \
    "driver-fixes-20261119/supervooc-opd2513/tests/test_${name}.c" \
    -o "/tmp/opd2513-test-${name}" && "/tmp/opd2513-test-${name}"
done
```

These checks and the ARM64 module build passed before the charging run. Optional OEM UFCS integration remains experimental and is not enabled in this tested SuperVOOC module.

## Prepared persistence tooling

The repository includes persistence tooling. **It has not been installed on the tablet in this publication task, and restart behavior has not been tested.** The user limited this task to GitHub publication and documentation updates.

For a subsequent installation, copy this directory and the exact verified module to the tablet, attach ordinary computer USB, and run on the tablet:

```bash
sudo python3 install-persistent.py --module /path/to/opd2513_vooc.ko
systemctl is-enabled opd2513-supervooc.service
systemctl status opd2513-supervooc.service
journalctl -u opd2513-supervooc.service -b
```

The installer checks board identity, kernel and module hash, backs up existing destination files, installs into `/usr/local/lib/opd2513-supervooc/`, and enables `opd2513-supervooc.service`. The service verifies the loaded Build ID, all seven parameters and pump binding. Replacing an older module is deferred while a charger is supplying high voltage. A compatible battery module and board DT are prerequisites; this script does not replace them or install a new kernel.

To disable this service, run `sudo systemctl disable --now opd2513-supervooc.service`, then boot the retained previous RAM image. Stopping the service leaves the running charging driver loaded. Previous persistent files, if any, are recorded under `/var/lib/opd2513-supervooc/backups/`; restore them according to `installation.json` and reload systemd.

## Prepared RAM image

`pack-camera-charge.py` supports `--oem-normal-high --svooc-skip-soc-step`, in addition to explicit battery, VOOC and ADC module paths. See the [Chinese packing command](README.md#已验证版本的持久化2026-11-23). It checks the camera baseline hashes and camera/flash/audio-bus DT configuration, then embeds the exact module and profile in initrd.

The locally prepared boot image SHA-256 is:
`01b7ebbabdd02ff54ace9c1ae17270bb7827352f462387a8c4b1cfabc29c14c4`

The new image has not been boot-tested. It targets the existing development tablet's UFS rootfs and includes local control configuration; it is not published as generic firmware. Preserve stock Recovery and its boot dependencies, and continue using temporary `fastboot boot`. Standalone cold boot and a new Recovery-entry regression remain unverified. This publication performs no partition writes, flashing, tablet installation or reboot.
