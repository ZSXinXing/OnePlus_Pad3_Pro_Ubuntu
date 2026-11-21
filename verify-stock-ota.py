#!/usr/bin/env python3
"""Verify an AOSP-style whole-file OTA against a known device trust bundle."""

import argparse
from pathlib import Path
import struct
import subprocess
import tempfile
from zipfile import BadZipFile, ZipFile


def read_signature(package):
    # AOSP install/verifier.cpp: exclude the EOCD comment length and comment.
    package.seek(-6, 2)
    length = package.tell() + 6
    offset, marker, comment = struct.unpack('<HHH', package.read(6))
    end = length - comment - 22
    if marker != 0xffff or not 6 < offset <= comment or end < 0:
        raise ValueError('not an Android whole-file-signed ZIP')
    package.seek(end)
    eocd = package.read(22 + comment)
    if (eocd[:4] != b'PK\x05\x06' or b'PK\x05\x06' in eocd[4:]
            or struct.unpack_from('<H', eocd, 20)[0] != comment):
        raise ValueError('invalid or ambiguous ZIP end of central directory')
    return end + 20, eocd[-offset:-6]


def verify_signature(package, trust):
    signed_size, der = read_signature(package)
    with ZipFile(trust) as archive:
        certs = [archive.read(info) for info in archive.infolist()
                 if info.filename.endswith('.x509.pem') and info.file_size <= 65536]
    if not certs:
        raise ValueError('the device trust bundle contains no OTA certificates')
    with tempfile.TemporaryDirectory() as temp:
        anchors = Path(temp) / 'anchors.pem'
        signature = Path(temp) / 'signature.der'
        anchors.write_bytes(b'\n'.join(certs))
        signature.write_bytes(der)
        # Require the signer to come from our trust bundle, not from the CMS.
        # The device pins these certs directly rather than building a PKI chain.
        command = ['openssl', 'cms', '-verify', '-binary', '-inform', 'DER',
                   '-in', str(signature), '-nointern', '-noverify',
                   '-certfile', str(anchors), '-content', '/dev/stdin',
                   '-out', '/dev/null']
        with subprocess.Popen(command, stdin=subprocess.PIPE) as verifier:
            try:
                with verifier.stdin as pipe:
                    package.seek(0)
                    while signed_size:
                        block = package.read(min(signed_size, 1024 * 1024))
                        if not block:
                            raise ValueError('truncated signed data')
                        pipe.write(block)
                        signed_size -= len(block)
            except BrokenPipeError as exc:
                raise ValueError('OTA signature rejected') from exc
            if verifier.wait():
                raise ValueError('OTA signature rejected')


def read_metadata(package, expected_fingerprint=None):
    with ZipFile(package) as archive:
        path = 'META-INF/com/android/metadata'
        matches = [info for info in archive.infolist() if info.filename == path]
        if len(matches) != 1 or matches[0].file_size > 65536:
            raise ValueError('missing or ambiguous Android OTA text metadata')
        data = archive.read(matches[0]).decode('utf-8')
    entries = {}
    for line in data.splitlines():
        if not line:
            continue
        key, sep, value = line.partition('=')
        if not key or not sep or key in entries:
            raise ValueError('invalid or duplicate OTA metadata key')
        entries[key] = value
    fingerprints = entries.get('post-build', '').split('|')
    if not fingerprints or not all(item.startswith('OnePlus/OPD2513/OP6547L1:')
                                   for item in fingerprints):
        raise ValueError('OTA post-build fingerprint does not target OPD2513/OP6547L1')
    if entries.get('pre-build') or entries.get('pre-build-incremental'):
        raise ValueError('OTA is tied to a specific source build (not a full-package candidate)')
    if expected_fingerprint is not None and expected_fingerprint not in fingerprints:
        raise ValueError('OTA post-build does not match the expected device build fingerprint')
    return entries


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('package', type=Path)
    parser.add_argument('--trust', type=Path, default=Path(__file__).resolve().parent
                        / 'stock-recovery' / 'otacerts.zip')
    parser.add_argument('--expected-fingerprint',
                        help='require an exact signed post-build fingerprint match')
    args = parser.parse_args()
    try:
        with args.package.open('rb') as package:
            verify_signature(package, args.trust)
            metadata = read_metadata(package, args.expected_fingerprint)
    except (OSError, ValueError, BadZipFile) as exc:
        parser.exit(1, f'Not validated: {exc}\n')
    print('Signature and target metadata validated; recovery and rollback remain untested.')
    for name in ('pre-device', 'post-build', 'post-build-incremental',
                 'post-security-patch-level', 'post-timestamp', 'ota-type'):
        print(f'{name}={metadata.get(name, "(absent)")}')


if __name__ == '__main__':
    main()
