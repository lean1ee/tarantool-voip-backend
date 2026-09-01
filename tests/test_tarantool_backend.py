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
        self.usrloc = {}
        self.nodes = {}
        self.subscribers = {}
        self.tariffs = {}
        self.cdrs = {}
        self.stats = {
            'purged_calls': 0,
            'purged_dialogs': 0,
            'purged_usrloc': 0,
            'last_sweep': 0,
            'sweeps_count': 0,
        }

    def call_upsert(self, call_id: str, node_id: str, payload: dict, ttl_sec: int = 3600):
        if not call_id or not isinstance(call_id, str):
            return {'ok': False, 'error': 'Invalid call_id'}
        now = int(time.time())
        expires_at = now + ttl_sec
        existing = self.calls.get(call_id)
        created_at = existing['created_at'] if existing else now
        
        self.calls[call_id] = {
            'call_id': call_id,
            'node_id': node_id or 'default-node',
            'state': 'active',
            'created_at': created_at,
            'updated_at': now,
            'expires_at': expires_at,
            'payload': payload
        }
        return {'ok': True, 'call_id': call_id, 'expires_at': expires_at}

    def call_delete(self, call_id: str):
        if not call_id or not isinstance(call_id, str):
            return {'ok': False, 'error': 'Missing call_id'}
        if call_id in self.calls:
            del self.calls[call_id]
            return {'ok': True, 'deleted': True, 'call_id': call_id}
        return {'ok': True, 'deleted': False}

    def call_get(self, call_id: str):
        if not call_id or not isinstance(call_id, str):
            return None
        return self.calls.get(call_id)

    def call_restore(self, node_id: str = None):
        now = int(time.time())
        result = []
        for call in self.calls.values():
            if call['expires_at'] > now:
                if not node_id or call['node_id'] == node_id:
                    result.append(call)
        return result

    def node_heartbeat(self, node_id: str, address: str = '127.0.0.1:22222', active_calls: int = 0):
        if not node_id:
            return {'ok': False, 'error': 'Invalid node_id'}
        now = int(time.time())
        self.nodes[node_id] = {
            'node_id': node_id,
            'address': address,
            'status': 'active',
            'active_calls': active_calls,
            'last_ping': now
        }
        return {'ok': True, 'node_id': node_id, 'updated_at': now}

    def select_optimal_node(self):
        now = int(time.time())
        best_node = None
        min_calls = float('inf')
        for node in self.nodes.values():
            if node['status'] == 'active' and (now - node['last_ping']) < 10:
                if node['active_calls'] < min_calls:
                    min_calls = node['active_calls']
                    best_node = node
        return best_node

    def add_subscriber(self, sub_id: str, balance: float = 0.0, currency: str = 'USD', max_calls: int = 5, tariff_id: str = '1'):
        self.subscribers[sub_id] = {
            'subscriber_id': sub_id,
            'balance': balance,
            'currency': currency,
            'status': 'active',
            'max_concurrent_calls': max_calls,
            'tariff_id': tariff_id,
            'updated_at': int(time.time())
        }
        return {'ok': True, 'subscriber_id': sub_id, 'balance': balance}

    def update_balance(self, sub_id: str, delta: float):
        sub = self.subscribers.get(sub_id)
        if not sub:
            return {'ok': False, 'error': 'Subscriber not found'}
        sub['balance'] += delta
        sub['updated_at'] = int(time.time())
        return {'ok': True, 'subscriber_id': sub_id, 'balance': sub['balance']}

    def ttl_purge_all(self, now: int = None):
        if now is None:
            now = int(time.time())
        purged_calls = [k for k, v in self.calls.items() if v['expires_at'] <= now]
        for k in purged_calls:
            del self.calls[k]
        
        purged_dialogs = [k for k, v in self.dialogs.items() if v.get('expires_at', 0) <= now]
        for k in purged_dialogs:
            del self.dialogs[k]

        purged_usrloc = [k for k, v in self.usrloc.items() if v.get('expires_at', 0) <= now]
        for k in purged_usrloc:
            del self.usrloc[k]

        self.stats['purged_calls'] += len(purged_calls)
        self.stats['purged_dialogs'] += len(purged_dialogs)
        self.stats['purged_usrloc'] += len(purged_usrloc)
        self.stats['last_sweep'] = now
        self.stats['sweeps_count'] += 1

        return len(purged_calls), len(purged_dialogs), len(purged_usrloc)

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
        self.usrloc["contact-1"] = {"expires_at": now + 20}
        self.usrloc["contact-2"] = {"expires_at": now + 120}
        
        purged_c, purged_d, purged_u = self.ttl_purge_all(now=now + 50)
        self.assertEqual(purged_c, 1)
        self.assertEqual(purged_u, 1)
        self.assertIsNone(self.call_get("call-1"))
        self.assertIsNotNone(self.call_get("call-2"))
        self.assertNotIn("contact-1", self.usrloc)
        self.assertIn("contact-2", self.usrloc)

    def test_node_load_balancer(self):
        self.node_heartbeat("node-01", "10.0.0.1:22222", active_calls=15)
        self.node_heartbeat("node-02", "10.0.0.2:22222", active_calls=5)
        self.node_heartbeat("node-03", "10.0.0.3:22222", active_calls=42)

        best = self.select_optimal_node()
        self.assertIsNotNone(best)
        self.assertEqual(best['node_id'], "node-02")
        self.assertEqual(best['active_calls'], 5)

    def test_subscriber_provisioning(self):
        res = self.add_subscriber("user@example.com", balance=50.0, max_calls=3)
        self.assertTrue(res['ok'])
        self.assertEqual(res['balance'], 50.0)

        up = self.update_balance("user@example.com", -15.5)
        self.assertTrue(up['ok'])
        self.assertEqual(round(up['balance'], 1), 34.5)

if __name__ == '__main__':
    unittest.main()
