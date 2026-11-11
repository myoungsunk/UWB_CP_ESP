from datetime import datetime, timezone
from hashlib import sha256
from pathlib import Path
import json
import os
import re
import signal
import subprocess
import sys
import time
import serial

stage = Path(__file__).resolve().parent
sys.path.insert(0, str(stage.parent))
from rfb8 import Decoder

port = '/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0'
mac = '28:56:2f:73:06:54'
expected_r5 = '3b7918e7d9174d15369d50bb01660462a552dd42a3ffc8912ba3f1259ddead1a'
r5 = stage.parent / 'TAG_DUAL_DS_CIR.ino.bin'
diag = stage / 'tag_b_diag.bin'
report = {'started_utc': datetime.now(timezone.utc).isoformat(), 'tag_port': port, 'tag_mac': mac,
          'scope': 'R5 B-only guard and RX SELFTEST', 'rf_tx_requested': False,
          'temporary_app_write_attempted': False, 'r5_restored_and_verified': False}
signal.signal(signal.SIGHUP, signal.SIG_IGN)


def confirm_port():
    tty = os.path.realpath(port)
    if not re.fullmatch(r'/dev/ttyUSB\d+', tty) or not Path(tty).is_char_device():
        raise RuntimeError('Previously identified tag USB port is absent')
    in_use = subprocess.run(['fuser', tty], capture_output=True, timeout=5)
    if in_use.returncode != 1 or in_use.stdout.strip():
        raise RuntimeError('Tag port is busy or cannot be checked')
    report['resolved_tty'] = tty


def esp(log, *args):
    cmd = [sys.executable, '-m', 'esptool', '--chip', 'esp32', '--port', port,
           '--baud', '460800', *map(str, args)]
    path = stage / log
    with path.open('x', encoding='utf-8') as f:
        f.write('COMMAND: ' + json.dumps(cmd) + '\n')
        f.flush()
        p = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, timeout=40)
        f.write(f'\nEXIT: {p.returncode}\n')
    text = path.read_text(encoding='utf-8', errors='replace')
    if p.returncode != 0 or not re.search(r'(?im)^MAC:\s*'+re.escape(mac)+r'\s*$', text):
        raise RuntimeError(f'ESP32 command or identity check failed, see {log}')
    return text


def verify(log, image):
    text = esp(log, 'verify-flash', '0x10000', image)
    if 'Verification successful (digest matched).' not in text:
        raise RuntimeError(f'Flash digest did not match, see {log}')


def write(log, image):
    esp(log, 'write-flash', '-z', '--flash-mode', 'keep', '--flash-freq', 'keep',
        '--flash-size', 'keep', '0x10000', image)


def usb():
    confirm_port()
    decoder = Decoder()
    with serial.Serial(port=None, baudrate=921600, timeout=.15, write_timeout=2, exclusive=True) as ser:
        ser.dtr = False
        ser.rts = False
        ser.port = port
        ser.open()
        time.sleep(.8)

        def command(name):
            ser.write((name + '\n').encode('ascii'))
            ser.flush()
            until = time.monotonic() + 5
            while time.monotonic() < until:
                for frame in decoder.feed(ser.read(4096)):
                    m = frame.meta
                    if m.get('event') == name and m.get('role') == 'tag':
                        return m
                    if m.get('event') == 'ERROR':
                        raise RuntimeError(str(m))
            raise TimeoutError(name)

        hello = command('HELLO')
        report['hello'] = hello
        if hello.get('firmware') != 'D8_R5_B_ONLY_DIAG_20261111' or hello.get('active') is not False:
            raise RuntimeError('B-only diagnostic HELLO identity does not match')
        selftest = command('SELFTEST')
        report['selftest'] = selftest
        report['decoder'] = decoder.statistics()
        report['b_pass'] = (hello.get('hw_ok') is True and hello.get('fault_latched') is False
                            and selftest.get('pass') is True and selftest.get('dev_b') == 0xDECA0130
                            and selftest.get('ant_b_tx') == selftest.get('ant_b_rx') == 16436)


try:
    assert sha256(r5.read_bytes()).hexdigest() == expected_r5
    expected_diag = sys.argv[1]
    assert re.fullmatch(r'[a-f0-9]{64}', expected_diag)
    assert sha256(diag.read_bytes()).hexdigest() == expected_diag
    report['diagnostic_app_sha256'] = expected_diag
    report['r5_app_sha256'] = expected_r5
    confirm_port()
    verify('before_r5.log', r5)
    report['temporary_app_write_attempted'] = True
    write('diagnostic_upload.log', diag)
    verify('diagnostic_verify.log', diag)
    usb()
except Exception as exc:
    report['diagnostic_error'] = repr(exc)
finally:
    if report['temporary_app_write_attempted']:
        try:
            confirm_port()
            esp('restore_identity.log', 'flash-id')
            write('restore_upload.log', r5)
            verify('restore_verify.log', r5)
            report['r5_restored_and_verified'] = True
        except Exception as exc:
            report['restore_error'] = repr(exc)
    report['finished_utc'] = datetime.now(timezone.utc).isoformat()
    (stage / 'tag_b_result.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, indent=2))
sys.exit(0 if report.get('b_pass') and report['r5_restored_and_verified'] else
         2 if 'b_pass' in report and report['r5_restored_and_verified'] else 3)

