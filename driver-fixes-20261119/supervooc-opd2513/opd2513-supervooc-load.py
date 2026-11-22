#!/usr/bin/env python3
"""Load the hardware-tested OPD2513 profile from the persistent Ubuntu rootfs."""
import hashlib
from pathlib import Path
import subprocess
import time

KERNEL = '7.3.0-opd2513-fwzstd+'
MODULE_SHA256 = 'a5d65b3215bf62591caea5f2442dcb0f3ad0258d4984f6478434b67474027377'
BUILD_ID = '00a26bd05187a5b1157e5b6decf6440c776d88d5'
ROOT = Path('/usr/local/lib/opd2513-supervooc')
PARAMETERS = ('powered_trial', 'dual_trial', 'oem_policy', 'oem_first_3a',
              'oem_high_band_first_6a1', 'oem_normal_high', 'svooc_skip_soc_step')


def check_board():
    compatible = Path('/sys/firmware/devicetree/base/compatible').read_bytes().split(b'\0')
    if b'oneplus,opd2513' not in compatible:
        raise RuntimeError('This profile requires oneplus,opd2513')
    if subprocess.check_output(['uname', '-r'], text=True).strip() != KERNEL:
        raise RuntimeError('Kernel mismatch: rebuild and validate before installation')


def current_matches():
    note = Path('/sys/module/opd2513_vooc/notes/.note.gnu.build-id')
    if not note.exists() or not note.read_bytes().endswith(bytes.fromhex(BUILD_ID)):
        return False
    return all((Path('/sys/module/opd2513_vooc/parameters') / name).read_text().strip()
               in ('Y', '1') for name in PARAMETERS)


def verify_binding():
    driver = Path('/sys/bus/i2c/drivers/opd2513_vooc')
    for _ in range(50):
        if any(driver.glob('*-*/session_state')):
            return
        time.sleep(0.1)
    raise RuntimeError('SuperVOOC module has no bound charge pump; inspect journal/kernel log')


def main():
    check_board()
    module = ROOT / 'opd2513_vooc.ko'
    if hashlib.sha256(module.read_bytes()).hexdigest() != MODULE_SHA256:
        raise RuntimeError('Persistent module SHA-256 mismatch')
    if not current_matches():
        if Path('/sys/module/opd2513_vooc').exists():
            # Never remove a driver while its adapter is supplying high voltage.
            usb = Path('/sys/class/power_supply/qcom-battmgr-usb')
            online = int((usb / 'online').read_text())
            voltage = int((usb / 'voltage_now').read_text())
            usb_type = (usb / 'usb_type').read_text()
            if online and not (0 < voltage <= 5500000 and
                               any(t in usb_type for t in ('[SDP]', '[CDP]'))):
                raise RuntimeError('Driver replacement deferred until ordinary USB or unplugged')
            subprocess.run(['rmmod', 'opd2513_vooc'], check=True)
        for name in ('qcom_vadc_common', 'qcom_spmi_adc5_gen3'):
            if not Path('/sys/module', name).exists():
                subprocess.run(['modprobe', name], check=True)
        subprocess.run(['insmod', str(module)] + [p + '=1' for p in PARAMETERS], check=True)
    if not current_matches():
        raise RuntimeError('Loaded module or charging parameters differ from the verified profile')
    verify_binding()
    print('OPD2513 persistent SuperVOOC profile verified: ' + BUILD_ID, flush=True)


if __name__ == '__main__':
    main()
