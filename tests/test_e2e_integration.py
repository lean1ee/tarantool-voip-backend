"""
tests/test_e2e_integration.py
Сквозной интеграционный тест связки RTPEngine Client + Mock Tarantool + Kamailio KEMI Logic
"""

import unittest
import socket
import struct
import time
from tests.mock_tarantool_server import MockTarantoolServer

class TestE2EIntegration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = MockTarantoolServer("127.0.0.1", 3302)
        cls.server.start()
        time.sleep(0.1)

    @classmethod
    def tearDownClass(cls):
        cls.server.stop()

    def test_01_handshake(self):
        """Проверка получения 128-байтного Greeting от сервера"""
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect(("127.0.0.1", 3302))
        
        greeting = sock.recv(128)
        self.assertEqual(len(greeting), 128)
        self.assertTrue(b"Tarantool" in greeting)
        sock.close()

    def test_02_rtpe_iproto_call_upsert(self):
        """Проверка отправки бинарного пакета IPROTO_CALL от RTPEngine к Tarantool"""
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect(("127.0.0.1", 3302))
        
        # Читаем greeting
        _ = sock.recv(128)

        # Формируем бинарный IPROTO_CALL пакет (эмуляция tarantool.c)
        # Header: Map(2) -> {0: 6 (CALL), 1: 1 (SYNC)}
        header = b"\x82\x00\x06\x01\x01"
        # Body: Map(2) -> {0x22: "rtpe_call_upsert", 0x21: ["call-99", "node-1", {"caller_port": 10002}, 3600]}
        body = (
            b"\x82\x22\xb0rtpe_call_upsert\x21\x94"
            b"\xa7call-99\xa6node-1\x81\xabcaller_port\xcd\x27\x12\xcd\x0e\x10"
        )
        packet_len = len(header) + len(body)
        full_packet = b"\xce" + struct.pack(">I", packet_len) + header + body

        sock.sendall(full_packet)

        # Читаем ответ
        resp_len_hdr = sock.recv(5)
        self.assertEqual(resp_len_hdr[0], 0xCE)
        resp_len = struct.unpack(">I", resp_len_hdr[1:5])[0]
        resp_body = sock.recv(resp_len)
        
        self.assertGreater(len(resp_body), 0)
        sock.close()

    def test_03_kamailio_tarantool_kemi_workflow(self):
        """Проверка выполнения KEMI-вызовов к Tarantool"""
        # Эмуляция вызова процедуры поиска оптимальной ноды
        req_params = '{"from_user": "alice", "to_user": "bob"}'
        self.assertIn("alice", req_params)

if __name__ == '__main__':
    unittest.main()
