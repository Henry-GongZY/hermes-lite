#!/usr/bin/env python3
"""Device-side benchmark driver: drives the ESP32 REPL over serial and
collects per-turn wall-clock times. Blocked only on user WiFi provisioning.

Usage: python3 scripts/bench_device.py /dev/cu.usbserial-0001
"""
import serial, time, re, statistics, sys

PROMPTS = [
    ("pure-llm", "用一句话回答：1+1 等于几？"),
    ("one-tool", "调用 sys_info 工具查看设备状态，然后只回复工具返回的内容。"),
]

def main(port):
    s = serial.Serial(port, 115200, timeout=1)
    s.setDTR(False); s.setRTS(False)
    time.sleep(2.5)
    s.reset_input_buffer()
    for tag, prompt in PROMPTS:
        vals = []
        for _ in range(3):
            s.write(prompt.encode() + b"\n")
            end, buf = time.time() + 120, b""
            while time.time() < end:
                chunk = s.readline()
                buf += chunk
                if b" ms)" in chunk:  # firmware prints "(NNN ms)" at reply end
                    m = re.search(rb"\((\d+) ms\)", chunk)
                    if m:
                        vals.append(int(m.group(1)))
                        break
        if vals:
            print(f"{tag}: median {statistics.median(vals):.0f} ms over {len(vals)} runs")
        else:
            print(f"{tag}: no timing captured (device online?)")

if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbserial-0001")
