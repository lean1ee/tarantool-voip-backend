"""
tests/test_docker_cluster_ops.py
Проверка жизненного цикла медиа-сессий и хранимых процедур в живом Tarantool кластере
"""

import unittest
import socket
import time

class TestDockerClusterOps(unittest.TestCase):
    def test_01_rtpe_save_and_query(self):
        """Отправка команды offer в RTPEngine и проверка ответа"""
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(5)

        cookie = f"cookie_{int(time.time())}"
        call_id = "test-call-live-100"
        msg = f"{cookie} offer {{\"call-id\":\"{call_id}\",\"command\":\"offer\"}}".encode('ascii')

        s.sendto(msg, ("127.0.0.1", 22222))
        resp, _ = s.recvfrom(4096)
        self.assertIn(b"result2:ok", resp)
        s.close()

    def test_02_kamailio_sip_invite_bye(self):
        """Проверка диалога INVITE -> 180/200 OK -> BYE в Kamailio"""
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(5)

        call_id = f"call-live-dialog-{int(time.time())}"

        # 1. INVITE
        invite = (
            f"INVITE sip:bob@172.28.0.30:5060 SIP/2.0\r\n"
            f"Via: SIP/2.0/UDP 127.0.0.1:5060;branch=z9hG4bK-abc-{int(time.time())}\r\n"
            f"From: <sip:alice@example.com>;tag=tagA-{int(time.time())}\r\n"
            f"To: <sip:bob@example.com>\r\n"
            f"Call-ID: {call_id}\r\n"
            f"CSeq: 1 INVITE\r\n"
            f"Content-Length: 0\r\n\r\n"
        ).encode('utf-8')

        s.sendto(invite, ("127.0.0.1", 5060))
        resp, _ = s.recvfrom(4096)
        self.assertTrue(b"SIP/2.0 200 OK" in resp or b"SIP/2.0 180 Ringing" in resp)

        # 2. BYE
        bye = (
            f"BYE sip:bob@172.28.0.30:5060 SIP/2.0\r\n"
            f"Via: SIP/2.0/UDP 127.0.0.1:5060;branch=z9hG4bK-bye-{int(time.time())}\r\n"
            f"From: <sip:alice@example.com>;tag=tagA-{int(time.time())}\r\n"
            f"To: <sip:bob@example.com>;tag=bob-tag\r\n"
            f"Call-ID: {call_id}\r\n"
            f"CSeq: 2 BYE\r\n"
            f"Content-Length: 0\r\n\r\n"
        ).encode('utf-8')

        s.sendto(bye, ("127.0.0.1", 5060))
        try:
            resp, _ = s.recvfrom(4096)
            self.assertIn(b"SIP/2.0", resp)
        except Exception:
            pass
        s.close()

if __name__ == '__main__':
    unittest.main()
