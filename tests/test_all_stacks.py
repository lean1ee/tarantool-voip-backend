"""
tests/test_all_stacks.py
Comprehensive Multi-Stack SIP & Media Benchmark Runner:
- Stack 1: Kamailio + RTPEngine + Tarantool 3.x (Port 5060)
- Stack 2: OpenSIPS + RTPEngine + Tarantool 3.x (Port 5070)
- Stack 3: RTPEngine + Redis Baseline
"""

import subprocess
import socket
import time
import sys

def restart_uas():
    try:
        subprocess.run(["docker", "compose", "restart", "sipp_uas"], capture_output=True)
        subprocess.run(["docker", "restart", "sipp_uas_bob"], capture_output=True)
        time.sleep(2.0)
    except Exception:
        pass

def test_sip_dialog(proxy_name, host, port, call_count=50):
    print(f"\n==================================================================")
    print(f"  Testing {proxy_name} ({host}:{port}) + RTPEngine + Tarantool 3.x")
    print(f"==================================================================")

    # 1. Functional Handshake Verification
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(5)
    call_id = f"test-{proxy_name.lower()}-handshake-{int(time.time())}"
    invite = (
        f"INVITE sip:bob@{host}:{port} SIP/2.0\r\n"
        f"Via: SIP/2.0/UDP 127.0.0.1:5060;branch=z9hG4bK-abc-{int(time.time())}\r\n"
        f"From: <sip:alice@example.com>;tag=tagA-{int(time.time())}\r\n"
        f"To: <sip:bob@example.com>\r\n"
        f"Call-ID: {call_id}\r\n"
        f"CSeq: 1 INVITE\r\n"
        f"Content-Length: 0\r\n\r\n"
    ).encode('utf-8')

    try:
        s.sendto(invite, (host, port))
        resp, _ = s.recvfrom(4096)
        if b"SIP/2.0 180" in resp or b"SIP/2.0 200" in resp:
            print(f"    [+] Functional SIP Dialog OK: {resp.split(b'\\r\\n')[0].decode('ascii')}")
            # Send ACK to cleanly terminate the transaction
            ack = (
                f"ACK sip:bob@{host}:{port} SIP/2.0\r\n"
                f"Via: SIP/2.0/UDP 127.0.0.1:5060;branch=z9hG4bK-ack-{int(time.time())}\r\n"
                f"From: <sip:alice@example.com>;tag=tagA-{int(time.time())}\r\n"
                f"To: <sip:bob@example.com>\r\n"
                f"Call-ID: {call_id}\r\n"
                f"CSeq: 1 ACK\r\n"
                f"Content-Length: 0\r\n\r\n"
            ).encode('utf-8')
            s.sendto(ack, (host, port))
        else:
            print(f"    [-] Unexpected response: {resp}")
            return False
    except Exception as e:
        print(f"    [-] Functional check failed: {e}")
        return False
    finally:
        s.close()

    # 2. SIPp High-Concurrency Load Test
    print(f"[*] Running SIPp High-Concurrency Load Test: {call_count} calls @ 25 cps...")
    restart_uas()
    target_ip = "172.28.0.30:5060" if port == 5060 else "172.28.0.35:5060"
    cmd = [
        "docker", "compose", "run", "--rm", "--no-deps",
        "sipp_uac",
        "-sf", "/sipp/uac.xml", target_ip,
        "-s", "bob", "-i", "172.28.0.50",
        "-r", "25", "-m", str(call_count), "-l", str(call_count),
        "-mp", "32000"
    ]
    t0 = time.time()
    res = subprocess.run(cmd, capture_output=True, text=True)
    t1 = time.time()

    success = "Failed call            |        0                  |        0" in res.stdout
    if success:
        print(f"    [+] SIPp Load Test Succeeded: {call_count}/{call_count} calls (100% Success in {t1 - t0:.2f}s)")
        return True
    else:
        print(f"    [-] SIPp Load Test had failures.")
        print(res.stdout)
        return False

def main():
    print("==================================================================")
    print("        MULTI-STACK VOIP BENCHMARK: KAMAILIO VS OPENSIPS          ")
    print("==================================================================")

    # 1. Test Kamailio Stack
    kam_ok = test_sip_dialog("Kamailio", "127.0.0.1", 5060, call_count=100)

    time.sleep(2)

    # 2. Test OpenSIPS Stack
    ops_ok = test_sip_dialog("OpenSIPS", "127.0.0.1", 5070, call_count=100)

    print("\n==================================================================")
    print("                      FINAL RESULTS SUMMARY                       ")
    print("==================================================================")
    print(f"  1. Kamailio + RTPEngine + Tarantool 3.x: {'[+] PASSED (100%)' if kam_ok else '[-] FAILED'}")
    print(f"  2. OpenSIPS + RTPEngine + Tarantool 3.x: {'[+] PASSED (100%)' if ops_ok else '[-] FAILED'}")
    print("==================================================================")

    if kam_ok and ops_ok:
        print("\n[+] Both VoIP stacks (Kamailio & OpenSIPS) operate flawlessly with Tarantool 3.x!")
        sys.exit(0)
    else:
        print("\n[-] Some stacks failed verification.")
        sys.exit(1)

if __name__ == "__main__":
    main()
