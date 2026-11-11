#!/usr/bin/env python3
"""Post-flash USB-only Mode-8 HELLO/SELFTEST. NO ranging or RF TX performed."""
from __future__ import annotations
import argparse
import json
import secrets
import sys
import time
from pathlib import Path
from rfb8 import Decoder

def main()->int:
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--port', required=True)
    ap.add_argument('--role', required=True, choices=['anchor','tag'])
    ap.add_argument('--out',type=Path)
    a=ap.parse_args()
    import serial
    result={'port':a.port,'role':a.role,'pass':False,'no_rf_tx_requested':True,
            'actual_usb_hardware_test':True,'checks':{}}
    try:
        with serial.Serial(port=None,baudrate=921600,timeout=0.2,write_timeout=2) as ser:
            ser.dtr=False;ser.rts=False;ser.port=a.port;ser.open()
            time.sleep(1.0)
            decoder=Decoder()
            def command(name, match, timeout=8):
                data=(name+'\n').encode('ascii')
                if ser.write(data)!=len(data):
                    raise OSError('Short USB serial write')
                ser.flush();until=time.monotonic()+timeout
                while time.monotonic()<until:
                    for msg in decoder.feed(ser.read(4096)):
                        m=msg.meta
                        if m.get('event') in ('ERROR','HARDWARE_FAULT_LATCHED'):
                            raise RuntimeError(str(m))
                        if match(m):
                            return m
                raise TimeoutError(f'No valid response to {name}')
            request=secrets.randbelow(0xffffffff-1)+1
            command(f'STOP {request}',lambda m:m.get('event')=='STOPPED' and
                    m.get('role')==a.role and m.get('stop_request')==request)
            hello=command('HELLO',lambda m:m.get('event')=='HELLO' and m.get('role')==a.role)
            result['hello']=hello
            expected='D8_R6_ANCHOR_STAGE6_P2IOFIX_20261111' if a.role=='anchor' else 'D8_R4_SOURCE_20261111'
            result['checks']['firmware']=hello.get('firmware')==expected
            result['checks']['mode']=hello.get('mode')==8 and hello.get('wire')=='RFB8'
            result['checks']['config']=hello.get('config_id')==459867368
            result['checks']['faults']=hello.get('hw_ok') is True and hello.get('fault_latched') is False
            result['checks']['idle']=hello.get('active') is False
            st=command('SELFTEST',lambda m:m.get('event')=='SELFTEST' and m.get('role')==a.role)
            result['selftest']=st
            result['checks']['selftest']=st.get('pass') is True
            result['checks']['dev_a']=st.get('dev_a')==0xDECA0130
            if a.role=='tag':
                result['checks']['dev_b']=st.get('dev_b')==0xDECA0130
            result['checks']['decoder']=not any(decoder.statistics()[k] for k in
                ('crc_errors','schema_errors','sequence_gap_events','trailing_bytes'))
            result['pass']=all(result['checks'].values())
    except Exception as e:
        result['error']=repr(e)
    if a.out:
        a.out.parent.mkdir(parents=True,exist_ok=True)
        a.out.write_text(json.dumps(result,indent=2,ensure_ascii=False),encoding='utf-8')
    print(json.dumps(result,indent=2,ensure_ascii=False))
    return 0 if result['pass'] else 2

if __name__=='__main__':
    sys.exit(main())
