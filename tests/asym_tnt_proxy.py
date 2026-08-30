#!/usr/bin/env python3
"""
High-Performance Dual-Node SIP Proxy connected to Tarantool 3.x (IProto)
Spawns 2 UDP listeners:
- Node 1 on 127.0.0.1:5091
- Node 2 on 127.0.0.1:5092
Executes real binary IProto REPLACE and SELECT against Tarantool 3.x server (127.0.0.1:3301).
"""

import socket
import select
import threading
import struct
import time
import sys
import os

TNT_HOST = os.environ.get("TNT_HOST", "127.0.0.1")
TNT_PORT = int(os.environ.get("TNT_PORT", "3301"))

def tnt_connect():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((TNT_HOST, TNT_PORT))
    # Read greeting (128 bytes)
    greeting = s.recv(128)
    return s

def tnt_replace(s, key, value, sync_id):
    # IProto REPLACE into space 512
    key_bytes = key.encode()
    val_bytes = value.encode()
    
    # MessagePack header: map(2): {0x00: 0x03 (IPROTO_REPLACE), 0x01: sync_id}
    hdr = b'\x82\x00\x03\x01' + struct.pack('>I', sync_id)
    # MessagePack body: map(2): {0x10: 512, 0x21: array(7)}
    body_hdr = b'\x82\x10\xcd\x02\x00\x21\x97'
    # Tuple fields: [key, 'kam_proxy', 'active', now, now, now+3600, value]
    now = int(time.time())
    key_mp = bytes([0xa0 | len(key_bytes)]) if len(key_bytes) <= 31 else b'\xd9' + bytes([len(key_bytes)])
    key_mp += key_bytes
    
    meta_mp = b'\xa9kam_proxy\xa6active' + struct.pack('>III', now, now, now+3600)
    val_mp = (bytes([0xa0 | len(val_bytes)]) if len(val_bytes) <= 31 else b'\xd9' + bytes([len(val_bytes)])) + val_bytes
    
    payload = hdr + body_hdr + key_mp + meta_mp + val_mp
    pkt = b'\xce' + struct.pack('>I', len(payload)) + payload
    s.sendall(pkt)
    
    # Read response
    resp_hdr = s.recv(5)
    resp_len = struct.unpack('>I', resp_hdr[1:5])[0]
    resp_body = s.recv(resp_len)
    return True

def tnt_select(s, key, sync_id):
    key_bytes = key.encode()
    # Header
    hdr = b'\x82\x00\x01\x01' + struct.pack('>I', sync_id)
    # Body
    body_hdr = b'\x86\x10\xcd\x02\x00\x11\x00\x12\x01\x13\x00\x14\x00\x20\x91'
    key_mp = (bytes([0xa0 | len(key_bytes)]) if len(key_bytes) <= 31 else b'\xd9' + bytes([len(key_bytes)])) + key_bytes
    
    payload = hdr + body_hdr + key_mp
    pkt = b'\xce' + struct.pack('>I', len(payload)) + payload
    s.sendall(pkt)
    
    # Read response
    resp_hdr = s.recv(5)
    resp_len = struct.unpack('>I', resp_hdr[1:5])[0]
    resp_body = s.recv(resp_len)
    return True

def run_node(node_id, port):
    tnt_sock = tnt_connect()
    sync_seq = 0
    
    sip_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sip_sock.bind(("127.0.0.1", port))
    print(f"[+] Tarantool SIP Node {node_id} listening on 127.0.0.1:{port}")
    
    while True:
        data, addr = sip_sock.recvfrom(4096)
        msg = data.decode(errors='ignore')
        sync_seq += 1
        
        # Extract Call-ID
        cid = ""
        for line in msg.split('\r\n'):
            if line.lower().startswith("call-id:"):
                cid = line.split(':', 1)[1].strip()
                break
                
        if "X-Store-Dialog: 1" in msg:
            tnt_replace(tnt_sock, cid, f"dialog-active-{cid}", sync_seq)
            resp = f"SIP/2.0 200 OK\r\nCall-ID: {cid}\r\nContent-Length: 0\r\n\r\n"
            sip_sock.sendto(resp.encode(), addr)
        elif "X-Fetch-Dialog: 1" in msg:
            tnt_select(tnt_sock, cid, sync_seq)
            resp = f"SIP/2.0 200 OK\r\nCall-ID: {cid}\r\nContent-Length: 0\r\n\r\n"
            sip_sock.sendto(resp.encode(), addr)
        else:
            resp = f"SIP/2.0 200 OK\r\nContent-Length: 0\r\n\r\n"
            sip_sock.sendto(resp.encode(), addr)

def main():
    t1 = threading.Thread(target=run_node, args=(1, 5091), daemon=True)
    t2 = threading.Thread(target=run_node, args=(2, 5092), daemon=True)
    t1.start()
    t2.start()
    print("[+] Tarantool Proxy Nodes 1 and 2 ready.")
    while True:
        time.sleep(1)

if __name__ == "__main__":
    main()
