"""
tests/run_e2e_docker.py
Сквозной автоматизированный тест кластера Kamailio + RTPEngine + Tarantool в Docker
"""

import socket
import time
import sys
import struct

def test_tarantool_handshake(host="127.0.0.1", port=3301):
    print(f"[*] 1. Testing Tarantool Handshake ({host}:{port})...")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5)
    try:
        s.connect((host, port))
        greeting = s.recv(128)
        assert len(greeting) == 128, f"Expected 128 bytes greeting, got {len(greeting)}"
        assert b"Tarantool" in greeting, "Greeting does not contain 'Tarantool'"
        version = greeting[:64].strip().decode('ascii', errors='ignore')
        print(f"    [+] Tarantool Connected: {version}")
        return True
    except Exception as e:
        print(f"    [-] Tarantool connection failed: {e}")
        return False
    finally:
        s.close()

def test_rtpengine_ng_protocol(host="127.0.0.1", port=22222):
    print(f"[*] 2. Testing RTPEngine NG Protocol ({host}:{port}/udp)...")
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(5)
    try:
        cookie = "test_cookie_101"
        # Отправляем ping
        ping_msg = f"{cookie} ping".encode('ascii')
        s.sendto(ping_msg, (host, port))
        resp, _ = s.recvfrom(4096)
        assert b"pong" in resp, f"Expected pong in response, got {resp}"
        print(f"    [+] RTPEngine PING/PONG OK: {resp.decode('ascii', errors='ignore')}")

        # Отправляем offer
        call_id = "test-call-e2e-8888"
        offer_msg = f"{cookie} offer {{\"call-id\":\"{call_id}\",\"from-tag\":\"tagA\"}}".encode('ascii')
        s.sendto(offer_msg, (host, port))
        offer_resp, _ = s.recvfrom(4096)
        assert b"result2:ok" in offer_resp, f"Expected ok in offer response, got {offer_resp}"
        print(f"    [+] RTPEngine OFFER Processed OK: {offer_resp.decode('ascii', errors='ignore')}")
        return True
    except Exception as e:
        print(f"    [-] RTPEngine NG test failed: {e}")
        return False
    finally:
        s.close()

def test_kamailio_sip(host="127.0.0.1", port=5060):
    print(f"[*] 3. Testing Kamailio SIP Signaling with Tarantool KEMI ({host}:{port}/udp)...")
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(5)
    try:
        sip_invite = (
            "INVITE sip:bob@172.28.0.30:5060 SIP/2.0\r\n"
            "Via: SIP/2.0/UDP 127.0.0.1:5060;branch=z9hG4bK-771122\r\n"
            "From: <sip:alice@example.com>;tag=alice-tag-1\r\n"
            "To: <sip:bob@example.com>\r\n"
            "Call-ID: sip-call-e2e-9999@127.0.0.1\r\n"
            "CSeq: 1 INVITE\r\n"
            "Content-Length: 0\r\n\r\n"
        ).encode('utf-8')

        s.sendto(sip_invite, (host, port))
        resp = b""
        for _ in range(3):
            chunk, _ = s.recvfrom(4096)
            resp += chunk
            if b"SIP/2.0 200 OK" in chunk or b"SIP/2.0 180 Ringing" in chunk:
                break
        assert b"SIP/2.0 200 OK" in resp or b"SIP/2.0 180 Ringing" in resp, f"Expected SIP 180/200 OK, got: {resp}"
        print(f"    [+] Kamailio SIP Response: {resp.split(b'\\r\\n')[0].decode('ascii')}")
        return True
    except Exception as e:
        print(f"    [-] Kamailio SIP test failed: {e}")
        return False
    finally:
        s.close()

def main():
    print("==================================================================")
    print("  E2E Test Suite: Kamailio + RTPEngine + Tarantool 3.x Cluster   ")
    print("==================================================================")
    
    tnt_ok = test_tarantool_handshake()
    rtpe_ok = test_rtpengine_ng_protocol()
    kam_ok = test_kamailio_sip()

    print("------------------------------------------------------------------")
    if tnt_ok and rtpe_ok and kam_ok:
        print("  ALL E2E INTEGRATION TESTS PASSED (3/3 SUCCESS)                 ")
        print("==================================================================")
        sys.exit(0)
    else:
        print("  SOME TESTS FAILED! Check container logs.                       ")
        print("==================================================================")
        sys.exit(1)

if __name__ == "__main__":
    main()
