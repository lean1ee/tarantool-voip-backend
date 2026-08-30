"""
tests/mock_tarantool_server.py
Легковесный mock-сервер IProto Tarantool 3.x для интеграционных тестов
"""

import socket
import threading
import struct
import base64
import os
import time

IPROTO_OK = 0
IPROTO_SELECT = 1
IPROTO_INSERT = 2
IPROTO_REPLACE = 3
IPROTO_UPDATE = 4
IPROTO_DELETE = 5
IPROTO_CALL = 6
IPROTO_AUTH = 7
IPROTO_EVAL = 8

IPROTO_REQUEST_TYPE = 0x00
IPROTO_SYNC = 0x01
IPROTO_DATA = 0x30
IPROTO_ERROR_24 = 0x31

class MockTarantoolServer:
    def __init__(self, host="127.0.0.1", port=3301):
        self.host = host
        self.port = port
        self.running = False
        self.server_sock = None
        self.thread = None
        self.stored_calls = {}
        self.total_requests = 0

    def start(self):
        self.server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server_sock.bind((self.host, self.port))
        self.server_sock.listen(5)
        self.running = True
        self.thread = threading.Thread(target=self._accept_loop, daemon=True)
        self.thread.start()

    def stop(self):
        self.running = False
        if self.server_sock:
            try:
                self.server_sock.close()
            except Exception:
                pass

    def _accept_loop(self):
        while self.running:
            try:
                client_sock, _ = self.server_sock.accept()
                t = threading.Thread(target=self._handle_client, args=(client_sock,), daemon=True)
                t.start()
            except Exception:
                break

    def _handle_client(self, sock):
        try:
            # 1. Отправляем Greeting (128 байт)
            version_str = b"Tarantool 3.0.0 (Binary) MockServer\n"
            greeting = version_str.ljust(64, b" ")
            salt = base64.b64encode(os.urandom(32))[:44] + b"\n"
            greeting += salt.ljust(64, b" ")
            sock.sendall(greeting)

            # 2. Читаем запросы IProto
            while self.running:
                # Длина пакета (MessagePack uint32 или uint8/16/32)
                raw_len = sock.recv(5)
                if not raw_len:
                    break
                
                # Парсим MP_UINT32 (0xCE + 4 bytes) или MP_UINT8/16
                if raw_len[0] == 0xCE:
                    length = struct.unpack(">I", raw_len[1:5])[0]
                else:
                    length = raw_len[0]
                
                body_data = b""
                while len(body_data) < length:
                    chunk = sock.recv(length - len(body_data))
                    if not chunk:
                        break
                    body_data += chunk

                self.total_requests += 1
                
                # Ответ: IPROTO_OK (успех)
                # Header: { IPROTO_REQUEST_TYPE: 0, IPROTO_SYNC: 1 }
                # Body: { IPROTO_DATA: [ { status: "ok" } ] }
                resp_payload = b"\x82\x00\x00\x01\x01\x81\x30\x91\x81\xa6status\xa2ok"
                resp_len = len(resp_payload)
                resp_header = b"\xce" + struct.pack(">I", resp_len)
                sock.sendall(resp_header + resp_payload)

        except Exception:
            pass
        finally:
            sock.close()

if __name__ == "__main__":
    server = MockTarantoolServer("127.0.0.1", 3301)
    server.start()
    print(f"Mock Tarantool Server running on {server.host}:{server.port}")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        server.stop()
