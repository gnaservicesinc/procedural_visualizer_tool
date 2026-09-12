import asyncio
import json
import unittest
import uuid
from unittest.mock import patch

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

from pvt_remote.protocol import b64, compact
from pvt_remote.relay import Relay


class Socket:
    """In-memory WebSocket with genuine challenge signatures and controllable IO."""
    def __init__(self, identity=None, key=None, messages=(), gate=None):
        self.identity = identity or str(uuid.uuid4())
        self.key = key or Ed25519PrivateKey.generate()
        self.messages = messages
        self.gate = gate
        self.sent = []
        self.closed = None

    async def send(self, raw):
        self.sent.append(json.loads(raw))

    async def recv(self):
        challenge = self.sent[0]['challenge']
        return compact(dict(id=self.identity, key=b64(self.key.public_key().public_bytes_raw()),
                            signature=b64(self.key.sign(compact(['pvt-relay-v1', self.identity, challenge])))))

    async def close(self, code, reason):
        self.closed = code
        if self.gate:
            self.gate.set()

    async def __aiter__(self):
        for recipient in self.messages:
            yield compact(dict(version=1, **{'from': self.identity, 'to': recipient})).decode()
        if self.gate:
            await self.gate.wait()


class RelayTests(unittest.IsolatedAsyncioTestCase):
    async def test_more_than_4096_historical_registrations(self):
        relay = Relay()
        # Actual verified registrations, not just pre-populated cache entries.
        for _ in range(4097):
            socket = Socket()
            await relay.connection(socket)
            self.assertIsNone(socket.closed)
        self.assertEqual(len(relay.keys), 4096)
        self.assertEqual(relay.clients, {})
        self.assertEqual(relay.connections, 0)

    async def test_eviction_preserves_active_and_recent_pins(self):
        relay = Relay()
        active = Socket()
        relay.keys[active.identity] = active.key.public_key().public_bytes_raw()
        relay.clients[active.identity] = active
        oldest = str(uuid.uuid4())
        for identity in [oldest] + [str(uuid.uuid4()) for _ in range(4094)]:
            relay.keys[identity] = b'x' * 32
        newcomer = Socket()
        await relay.connection(newcomer)
        self.assertIsNone(newcomer.closed)
        self.assertNotIn(oldest, relay.keys)
        for pinned in [active, newcomer]:
            impostor = Socket(identity=pinned.identity)
            await relay.connection(impostor)
            self.assertEqual(impostor.closed, 1008)
            self.assertEqual(relay.keys[pinned.identity], pinned.key.public_key().public_bytes_raw())
        self.assertIs(relay.clients[active.identity], active)

    async def test_reconnect_cleanup_keeps_replacement(self):
        relay = Relay()
        first = Socket(gate=asyncio.Event())
        task = asyncio.create_task(relay.connection(first))
        while first.identity not in relay.clients:
            await asyncio.sleep(0)
        second = Socket(identity=first.identity, key=first.key, gate=asyncio.Event())
        replacement = asyncio.create_task(relay.connection(second))
        await task
        self.assertIs(relay.clients[first.identity], second)
        second.gate.set()
        await replacement
        self.assertEqual(relay.clients, {})
        self.assertEqual(relay.connections, 0)

    async def test_host_can_answer_all_64_remotes(self):
        relay = Relay()
        receivers = [Socket() for _ in range(64)]
        relay.clients.update({s.identity: s for s in receivers})
        sender = Socket(messages=[s.identity for s in receivers])
        with patch('pvt_remote.relay.time') as clock:
            clock.monotonic.return_value = 100
            await relay.connection(sender)
        self.assertIsNone(sender.closed)
        self.assertTrue(all(len(s.sent) == 1 for s in receivers))

    async def test_per_recipient_and_aggregate_limits_still_apply(self):
        for recipients, delivered in [([str(uuid.uuid4())] * 61, 60),
                                      ([str(uuid.uuid4()) for _ in range(257)], 256)]:
            relay = Relay()
            sink = Socket()
            relay.clients.update({recipient: sink for recipient in recipients})
            sender = Socket(messages=recipients)
            with patch('pvt_remote.relay.time') as clock:
                clock.monotonic.return_value = 100
                await relay.connection(sender)
            self.assertEqual(sender.closed, 1008)
            self.assertEqual(len(sink.sent), delivered)

    async def test_rate_window_expires(self):
        relay = Relay()
        receiver = Socket()
        relay.clients[receiver.identity] = receiver
        sender = Socket(messages=[receiver.identity] * 61)
        with patch('pvt_remote.relay.time') as clock:
            clock.monotonic.side_effect = [100] * 60 + [161]
            await relay.connection(sender)
        self.assertIsNone(sender.closed)
        self.assertEqual(len(receiver.sent), 61)


if __name__ == '__main__':
    unittest.main()
