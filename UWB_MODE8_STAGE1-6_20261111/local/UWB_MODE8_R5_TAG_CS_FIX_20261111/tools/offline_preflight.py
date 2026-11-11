#!/usr/bin/env python3
"""Offline upload-readiness check; never contacts devices or writes flash."""
from __future__ import annotations
import argparse
from pathlib import Path
import hashlib
import json
import struct
import sys
from source_contract import matches

ROOT=Path(__file__).resolve().parents[1]


def scan(root: Path = ROOT) -> dict:
    path=root/'dist/target_build_receipt.json'
    receipt=json.loads(path.read_text()) if path.is_file() else {}
    result={'checks': {}, 'roles': {}, 'offline_only': True,
            'actual_flash_verified': False, 'rf_tested': False}
    result['checks']['target_receipt']=bool(receipt.get('target_build') is True)
    result['checks']['fqbn']=receipt.get('fqbn') == 'esp32:esp32:esp32doit-devkit-v1'
    result['checks']['core']=receipt.get('required_core') == '3.3.7'
    for role in ('anchor','tag'):
        info=receipt.get('roles', {}).get(role, {})
        folder=root/'dist'/role
        checks={'source_binding': matches(root,role,info.get('source_manifest'))}
        for extension in ('bin','bootloader.bin','partitions.bin','elf'):
            filename=f'{role.upper()}_DUAL_DS_CIR.ino.{extension}'
            file=folder/filename
            expected=info.get('artifacts',{}).get(filename,{})
            data=file.read_bytes() if file.is_file() else b''
            checks[f'{extension}_sha256']=bool(data and expected.get('bytes')==len(data)
                                               and expected.get('sha256')==hashlib.sha256(data).hexdigest())
            if extension == 'bin':
                checks['esp32_app_image_header']=len(data)>256 and data[0]==0xe9 and data[1]>0
            if extension == 'elf':
                checks['xtensa_elf_header']=len(data)>52 and data[:4]==b'\x7fELF' and data[4]==1 and struct.unpack_from('<H',data,18)[0]==94
        merged_file=folder/f'{role.upper()}_DUAL_DS_CIR.ino.merged.bin'
        merged_meta=info.get('artifacts',{}).get(merged_file.name,{})
        merged=merged_file.read_bytes() if merged_file.is_file() else b''
        checks['merged_sha256']=bool(merged and len(merged)==merged_meta.get('bytes') and
                                     hashlib.sha256(merged).hexdigest()==merged_meta.get('sha256'))
        for offset,suffix in ((0x1000,'bootloader.bin'),(0x8000,'partitions.bin'),(0x10000,'bin')):
            component=folder/f'{role.upper()}_DUAL_DS_CIR.ino.{suffix}'
            data=component.read_bytes() if component.is_file() else b''
            checks[f'merged_{suffix}_at_{offset:x}']=bool(merged and data and
                    offset+len(data)<=len(merged) and merged[offset:offset+len(data)]==data)
        result['roles'][role]={'checks':checks,'flash_preflight_pass':all(checks.values())}
    result['flash_preflight_pass']=all(result['checks'].values()) and all(
        result['roles'][role]['flash_preflight_pass'] for role in ('anchor','tag'))
    return result

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path)
    a=p.parse_args()
    result=scan()
    if a.out:
        a.out.parent.mkdir(parents=True,exist_ok=True)
        a.out.write_text(json.dumps(result,indent=2))
    print(json.dumps(result,indent=2))
    sys.exit(0 if result['flash_preflight_pass'] else 2)
