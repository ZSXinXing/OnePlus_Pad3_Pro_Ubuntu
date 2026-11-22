#!/usr/bin/env python3
"""Persist the exact tested module on a running OPD2513 Ubuntu installation."""
import argparse
import datetime
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--module', type=Path, required=True)
    args = parser.parse_args()
    if os.geteuid() != 0:
        parser.error('Run as root on the tablet')
    source = Path(__file__).resolve().parent
    spec = importlib.util.spec_from_file_location('profile', source / 'opd2513-supervooc-load.py')
    profile = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(profile)
    profile.check_board()
    if hashlib.sha256(args.module.read_bytes()).hexdigest() != profile.MODULE_SHA256:
        parser.error('Module does not match the hardware-tested connector fix')
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    backup = Path('/var/lib/opd2513-supervooc/backups') / stamp
    backup.mkdir(parents=True)
    targets = [(args.module, profile.ROOT / 'opd2513_vooc.ko'),
               (source / 'opd2513-supervooc-load.py', profile.ROOT / 'opd2513-supervooc-load.py'),
               (source / 'opd2513-supervooc.service', Path('/etc/systemd/system/opd2513-supervooc.service'))]
    records = []
    for src, dst in targets:
        dst.parent.mkdir(parents=True, exist_ok=True)
        existed = dst.exists()
        if existed:
            shutil.copy2(dst, backup / dst.name)
        with tempfile.NamedTemporaryFile(dir=dst.parent, delete=False) as stream:
            temp = Path(stream.name)
            stream.write(src.read_bytes())
            stream.flush()
            os.fsync(stream.fileno())
        temp.chmod(0o644)
        os.replace(temp, dst)
        records.append({'path': str(dst), 'previously_existed': existed,
                        'sha256': hashlib.sha256(dst.read_bytes()).hexdigest()})
    (backup / 'installation.json').write_text(json.dumps(records, indent=2) + '\n')
    subprocess.run(['systemctl', 'daemon-reload'], check=True)
    # Restart also verifies repeat installations when the oneshot is already active.
    subprocess.run(['systemctl', 'enable', 'opd2513-supervooc.service'], check=True)
    subprocess.run(['systemctl', 'restart', 'opd2513-supervooc.service'], check=True)
    subprocess.run(['sync'], check=True)
    print('Persistent profile installed; prior files recorded at ' + str(backup))


if __name__ == '__main__':
    main()
