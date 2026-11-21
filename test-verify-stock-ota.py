#!/usr/bin/env python3
"""Exercise the OTA verifier with local, synthetic AOSP-style packages."""

import io
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from zipfile import ZipFile


def sign(metadata, cert, key):
    data = io.BytesIO()
    with ZipFile(data, 'w') as archive:
        archive.writestr('META-INF/com/android/metadata', metadata)
    payload = data.getvalue()[:-2]
    der = subprocess.run(['openssl', 'cms', '-sign', '-binary', '-md', 'sha256',
                          '-outform', 'DER', '-signer', cert, '-inkey', key],
                         input=payload, capture_output=True, check=True).stdout
    size = len(der) + 6
    return payload + struct.pack('<H', size) + der + struct.pack('<HHH', size, 0xffff, size)


def main():
    root = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory() as temp:
        directory = Path(temp)
        cert, key = directory / 'cert.pem', directory / 'key.pem'
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
                        '-keyout', key, '-out', cert, '-subj', '/CN=Fixture', '-days', '1'],
                       capture_output=True, check=True)
        trust = directory / 'trust.zip'
        with ZipFile(trust, 'w') as archive:
            archive.write(cert, 'fixture.x509.pem')
        fingerprint = 'OnePlus/OPD2513/OP6547L1:16/example'
        meta = f'post-build={fingerprint}\nota-type=AB\n'
        valid = sign(meta, cert, key)
        ota = directory / 'ota.zip'
        for name, sample, anchors, flags, expected in (
            ('valid', valid, trust, [], None),
            ('exact build', valid, trust, ['--expected-fingerprint', fingerprint], None),
            ('same model, different build', valid, trust,
             ['--expected-fingerprint', fingerprint + '-next'],
             'does not match the expected device build fingerprint'),
            ('tampered', valid.replace(b'ota-type=AB', b'ota-type=AC'), trust, [],
             'OTA signature rejected'),
            ('wrong device', sign(meta.replace('OPD2513', 'OPD2514'), cert, key),
             trust, [], 'does not target OPD2513'),
            ('incremental', sign(meta + 'pre-build-incremental=123\n', cert, key),
             trust, [], 'not a full-package candidate'),
            ('untrusted signer', valid, root / 'stock-recovery' / 'otacerts.zip', [],
             'OTA signature rejected'),
            ('ordinary ZIP', b'PK\x05\x06' + bytes(18), trust, [],
             'not an Android whole-file-signed ZIP'),
        ):
            ota.write_bytes(sample)
            result = subprocess.run([sys.executable, root / 'verify-stock-ota.py',
                                     ota, '--trust', anchors, *flags], capture_output=True, text=True)
            if expected is None:
                assert result.returncode == 0 and 'target metadata validated' in result.stdout, result
            else:
                assert result.returncode == 1 and expected in result.stderr, result
            print(f'{name}: passed')


if __name__ == '__main__':
    main()
