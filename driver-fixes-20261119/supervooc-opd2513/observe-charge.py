#!/usr/bin/env python3
"""Collect a single wall-adapter trial locally, including after USB disconnects."""
import argparse
import json
from pathlib import Path
import subprocess
import time

SUPPLIES = Path('/sys/class/power_supply')
BAT = SUPPLIES / 'qcom-battmgr-bat'
USB = SUPPLIES / 'qcom-battmgr-usb'


def value(path):
    try:
        raw = path.read_text().strip()
    except OSError:
        return None
    try:
        return int(raw)
    except ValueError:
        return raw


def snapshot():
    return {
        'monotonic_s': time.monotonic(),
        'charging_sensors_mc': {
            label.read_text().strip(): value(label.with_name(
                label.name.removesuffix('_label') + '_input'))
            for label in Path('/sys/bus/iio/devices').glob('iio:device*/in_temp*_label')
            if value(label) in ('batt_con_therm_adc', 'usb_con_therm_adc',
                                'svooc_mos_btb_therm')
        },
        'battery': {key: value(BAT / key) for key in (
            'status', 'capacity', 'temp', 'voltage_now', 'current_now', 'charge_counter')},
        'vooc': next((value(p) for p in Path('/sys/bus/i2c/drivers/opd2513_vooc').glob('*-*/session_state')), None),
        'usb': {key: value(USB / key) for key in (
            'online', 'usb_type', 'voltage_now', 'current_now', 'input_current_limit')},
    }


def wall_connected(sample):
    usb = sample['usb']
    kind = usb['usb_type'] or ''
    voltage = usb['voltage_now']
    # PMIC firmware can report the previous charger type briefly after unplug.
    if isinstance(voltage, int) and voltage < 1_000_000:
        return False
    return usb['online'] == 1 and any(
        '[' + name + ']' in kind for name in ('DCP', 'CDP', 'PD', 'PD_DRP', 'PD_PPS', 'BrickID'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--wait-seconds', type=int, default=900)
    parser.add_argument('--capture-seconds', type=int, default=240)
    args = parser.parse_args()
    if not (1 <= args.wait_seconds <= 3600 and 1 <= args.capture_seconds <= 900):
        parser.error('wait must be 1..3600 and capture 1..900 seconds')
    args.output.mkdir(parents=True, exist_ok=False)
    metadata = {'boot_id': value(Path('/proc/sys/kernel/random/boot_id')),
                'kernel': value(Path('/proc/sys/kernel/osrelease')),
                'wait_seconds': args.wait_seconds, 'capture_seconds': args.capture_seconds,
                'scope': 'read-only telemetry; no charging control or battery thresholds changed'}
    (args.output / 'metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
    started = time.monotonic()
    trial = None
    first = last = None
    count = 0
    reason = 'adapter_not_seen'
    with (args.output / 'samples.jsonl').open('x', buffering=1) as stream:
        while True:
            sample = snapshot()
            now = sample['monotonic_s']
            if trial is None and wall_connected(sample):
                trial = now
                first = sample
            sample['trial_elapsed_s'] = None if trial is None else now - trial
            stream.write(json.dumps(sample) + '\n')
            count += 1
            if trial is None and now - started >= args.wait_seconds:
                break
            if trial is not None:
                if not wall_connected(sample):
                    reason = 'adapter_disconnected'
                    break
                last = sample
                if now - trial >= args.capture_seconds:
                    reason = 'capture_complete'
                    break
            time.sleep(1)
    net_mah = None
    if first and last:
        a, b = first['battery']['charge_counter'], last['battery']['charge_counter']
        if isinstance(a, int) and isinstance(b, int):
            net_mah = (b - a) / 1000
    summary = {'reason': reason, 'samples': count, 'net_charge_mAh': net_mah,
               'first_wall_sample': first, 'last_sample': last,
               'note': 'Positive counter delta measures net gain; an icon or online flag alone does not.'}
    (args.output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    log = subprocess.run(['dmesg'], capture_output=True, text=True, check=False)
    (args.output / 'kernel.log').write_text(log.stdout)
    print(json.dumps(summary))


if __name__ == '__main__':
    main()
