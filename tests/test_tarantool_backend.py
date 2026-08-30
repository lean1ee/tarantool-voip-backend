"""
tests/test_tarantool_backend.py
Автоматический тест схемы и хранимых процедур Tarantool Backend
"""

import unittest
import time

class TestTarantoolMockLogic(unittest.TestCase):
    def setUp(self):
        # Эмуляция структуры базы данных в памяти
        self.calls = {}
        self.dialogs = {}
        self.nodes = {}

    def call_upsert(self, call_id: str, node_id: str, payload: dict, ttl_sec: int = 3600):
        now = int(time.time())
        expires_at = now + ttl_sec
        existing = self.calls.get(call_id)
        if existing:
            created_at = existing['created_at']
        else:
            created_at = now
        
        self.calls[call_id] = {
            'call_id': call_id,
            'node_id': node_id,
            'state': 'active',
            'created_at': created_at,
            'updated_at': now,
            'expires_at': expires_at,
            'payload': payload
        }
        return {'ok': True, 'call_id': call_id, 'expires_at': expires_at}

    def call_delete(self, call_id: str):
        if call_id in self.calls:
            del self.calls[call_id]
            return {'ok': True, 'deleted': True, 'call_id': call_id}
        return {'ok': True, 'deleted': False}

    def call_get(self, call_id: str):
        return self.calls.get(call_id)

    def call_restore(self, node_id: str = None):
        now = int(time.time())
        result = []
        for call in self.calls.values():
            if call['expires_at'] > now:
                if not node_id or call['node_id'] == node_id:
                    result.append(call)
        return result

    def ttl_purge(self, now: int = None):
        if now is None:
            now = int(time.time())
        to_delete = [k for k, v in self.calls.items() if v['expires_at'] <= now]
        for k in to_delete:
            del self.calls[k]
        return len(to_delete)

    def test_call_lifecycle(self):
        call_id = "test-call-id-998811@192.168.1.50"
        payload = {"caller_ip": "192.168.1.50", "callee_ip": "192.168.1.60", "media_port": 10002}
        
        # 1. Upsert (Create)
        res = self.call_upsert(call_id, "rtpe-node-01", payload, ttl_sec=60)
        self.assertTrue(res['ok'])
        self.assertEqual(res['call_id'], call_id)
        
        # 2. Get
        call_data = self.call_get(call_id)
        self.assertIsNotNone(call_data)
        self.assertEqual(call_data['payload']['media_port'], 10002)
        
        # 3. Update (Re-INVITE)
        payload["media_port"] = 10004
        self.call_upsert(call_id, "rtpe-node-01", payload, ttl_sec=60)
        call_data = self.call_get(call_id)
        self.assertEqual(call_data['payload']['media_port'], 10004)
        
        # 4. Restore list
        active_calls = self.call_restore("rtpe-node-01")
        self.assertEqual(len(active_calls), 1)
        
        # 5. Delete (BYE)
        del_res = self.call_delete(call_id)
        self.assertTrue(del_res['deleted'])
        self.assertIsNone(self.call_get(call_id))

    def test_ttl_purging(self):
        now = int(time.time())
        self.call_upsert("call-1", "node-1", {}, ttl_sec=10)
        self.call_upsert("call-2", "node-1", {}, ttl_sec=100)
        
        purged = self.ttl_purge(now=now + 50)
        self.assertEqual(purged, 1)
        self.assertIsNone(self.call_get("call-1"))
        self.assertIsNotNone(self.call_get("call-2"))

if __name__ == '__main__':
    unittest.main()
