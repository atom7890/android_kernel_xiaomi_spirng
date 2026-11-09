#!/usr/bin/env python3
"""Extract the firmware payloads referenced by the original Spring FFU table."""
import argparse
import hashlib
import json
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('reference_module', type=Path)
parser.add_argument('output_directory', type=Path)
args = parser.parse_args()

manifest = json.loads(Path(__file__).with_name('spring-ffu-manifest.json').read_text())
reference = args.reference_module.read_bytes()
if hashlib.sha256(reference).hexdigest() != manifest['reference_module_sha256']:
    parser.exit(1, 'Reference module does not match the verified Spring FFU module.\n')

payloads = {}
for name, entry in manifest['payloads'].items():
    start, size = entry['file_offset'], entry['size']
    payload = reference[start:start + size]
    if len(payload) != size or hashlib.sha256(payload).hexdigest() != entry['sha256']:
        parser.exit(1, f'Payload validation failed for {name}.\n')
    payloads[name] = payload

args.output_directory.mkdir(parents=True, exist_ok=False)
for name, payload in payloads.items():
    (args.output_directory / f'{name}.bin').write_bytes(payload)
    print(f'{name}: {len(payload)} bytes')
