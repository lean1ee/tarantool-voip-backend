"""
tests/run_sipp_test.py
Скрипт запуска стресс-тестирования через SIPp в Docker
"""

import subprocess
import sys

def run_sipp_load(calls=30, rate=10):
    print(f"[*] Starting SIPp Load Test: {calls} calls at {rate} calls/second...")
    cmd = [
        "docker", "compose", "run", "--rm", "sipp_uac",
        "-sf", "/sipp/uac.xml", "172.28.0.30:5060",
        "-s", "bob", "-i", "172.28.0.50",
        "-r", str(rate), "-m", str(calls), "-l", str(calls),
        "-mp", "32000"
    ]
    res = subprocess.run(cmd, capture_output=True, text=True)
    print(res.stdout)
    if "Failed call            |        0                  |        0" in res.stdout:
        print("[+] SIPp Load Test Succeeded with 0 Failed Calls (100% Success)!")
        return True
    else:
        print("[-] Some calls failed.")
        return False

if __name__ == "__main__":
    calls = int(sys.argv[1]) if len(sys.argv) > 1 else 30
    rate = int(sys.argv[2]) if len(sys.argv) > 2 else 10
    success = run_sipp_load(calls, rate)
    sys.exit(0 if success else 1)
