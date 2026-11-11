"""Use the package collector with a local anchor and the tag's serial port over SSH."""

import argparse
from pathlib import Path
from queue import Empty, Queue
import shlex
import subprocess
import sys
import threading

import serial


ROOT = Path(__file__).resolve().parent
WORKSPACE = ROOT.parents[1]
TAG_PORT = "/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0"
sys.path.insert(0, str(ROOT.parent / "UWB_MODE8_R5_TAG_CS_FIX_20261111/tools"))
import run_pair


BRIDGE = '''
import os
import select
import serial
import sys

link = serial.Serial(port=None, baudrate=921600, timeout=0, write_timeout=2, exclusive=True)
link.dtr = False
link.rts = False
link.port = sys.argv[1]
link.open()
with link:
    sys.stdout.buffer.write(b"PI_SERIAL_READY\\n")
    sys.stdout.buffer.flush()
    while True:
        ready, _, _ = select.select([sys.stdin.fileno(), link.fileno()], [], [])
        if sys.stdin.fileno() in ready:
            data = os.read(sys.stdin.fileno(), 4096)
            if not data:
                break
            link.write(data)
            link.flush()
        if link.fileno() in ready:
            data = link.read(8192)
            if data:
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
'''


class PiSerial:
    def __init__(self, log_path, port, baudrate, timeout, write_timeout):
        if baudrate != 921600:
            raise ValueError("The Pi serial bridge uses 921600 baud")
        self.port = port
        self.baudrate = baudrate
        self.timeout = timeout
        self.log_path = log_path
        self.queue = Queue()
        self.process = None

    def open(self):
        remote = shlex.join(["python3", "-u", "-B", "-c", BRIDGE, self.port])
        self.log = self.log_path.open("xb")
        try:
            self.process = subprocess.Popen(
                ["ssh", "-T", "-F", str(WORKSPACE / ".codex-robot-ssh-config"),
                 "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", "slam-robot-tailscale",
                 remote],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.log,
            )
            self.reader = threading.Thread(target=self.pump, daemon=True)
            self.reader.start()
            if self.queue.get(timeout=12) != b"PI_SERIAL_READY\n":
                raise OSError(f"Pi tag port did not open. See {self.log_path}")
        except Exception:
            self.close()
            raise

    def pump(self):
        output = self.process.stdout
        self.queue.put(output.readline())
        while chunk := output.read1(65536):
            self.queue.put(chunk)
        self.queue.put(None)

    @property
    def in_waiting(self):
        return 65536

    def read(self, size):
        try:
            chunk = self.queue.get(timeout=self.timeout)
        except Empty:
            return b""
        if chunk is None:
            raise OSError(f"Pi serial connection closed. See {self.log_path}")
        return chunk

    def write(self, data):
        return self.process.stdin.write(data)

    def flush(self):
        self.process.stdin.flush()

    def close(self):
        try:
            if self.process is not None:
                try:
                    self.process.stdin.close()
                except BrokenPipeError:
                    pass
                try:
                    self.process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    self.process.terminate()
                    self.process.wait(timeout=3)
                self.reader.join(timeout=1)
                self.process.stdout.close()
        finally:
            self.log.close()


class SplitLink(run_pair.Link):
    def __init__(self, name, device, directory):
        native = serial.Serial
        try:
            if name == "tag":
                serial.Serial = lambda **kwargs: PiSerial(directory / "pi_ssh_serial.log", **kwargs)
            super().__init__(name, device, directory)
        finally:
            serial.Serial = native


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--stage", type=int, choices=(2, 3, 4, 5), default=2)
    parser.add_argument("--count", type=int, default=20)
    parser.add_argument("--anchor", default="COM4")
    args = parser.parse_args()
    run_pair.Link = SplitLink
    sys.argv = [sys.argv[0], "--anchor", args.anchor, "--tag", TAG_PORT,
                "--out", str(args.out), "--stage", str(args.stage), "--count", str(args.count),
                "--period-us", "300000"]
    return run_pair.main()


if __name__ == "__main__":
    sys.exit(main())
