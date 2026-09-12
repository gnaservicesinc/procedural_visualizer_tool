"""Opaque signaling relay. Deploy behind a TLS WebSocket reverse proxy.

The relay sees routing UUIDs, public registration keys, and encrypted envelope
sizes. It has no pairing private keys and cannot decrypt SDP or media.
"""
import argparse
import asyncio
import json
import time
import uuid
from collections import OrderedDict, deque
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
from websockets.asyncio.server import serve
from .protocol import MAX_MESSAGE, MAX_PROFILES, compact, unb64

class Relay:
    def __init__(self):
        self.clients = {}
        self.keys = OrderedDict()
        self.connections = 0

    async def connection(self, ws):
        identity = None
        if self.connections >= 256:
            await ws.close(1013, "Relay capacity")
            return
        self.connections += 1
        try:
            challenge = str(uuid.uuid4())
            await ws.send(compact(dict(challenge=challenge)).decode())
            registration = json.loads(await asyncio.wait_for(ws.recv(), 8))
            identity = registration["id"]
            uuid.UUID(identity)
            key = unb64(registration["key"], 32)
            Ed25519PublicKey.from_public_bytes(key).verify(unb64(registration["signature"], 64), compact(["pvt-relay-v1", identity, challenge]))
            if identity in self.keys and self.keys[identity] != key:
                raise ValueError("Registration key mismatch")
            if identity not in self.keys and len(self.keys) >= 4096:
                # Retain recent registration pins, but never let disconnected
                # identities exhaust capacity forever. Active pins cannot be
                # evicted. Endpoints independently pin the pairing-file keys.
                expired = next((item for item in self.keys if item not in self.clients), None)
                if expired is None:
                    raise ValueError("Registration capacity")
                del self.keys[expired]
            self.keys[identity] = key
            self.keys.move_to_end(identity)
            old = self.clients.get(identity)
            self.clients[identity] = ws
            if old:
                await old.close(1000, "Reconnected")
            times = deque()
            async for raw in ws:
                now = time.monotonic()
                while times and times[0][0] <= now - 60:
                    times.popleft()
                envelope = json.loads(raw)
                if envelope.get("version") != 1 or envelope.get("from") != identity:
                    raise ValueError("Invalid sender")
                recipient = envelope.get("to")
                uuid.UUID(recipient)
                # A host serves up to 64 paired remotes. Keep the existing
                # per-recipient limit plus a bounded aggregate handshake budget;
                # no self-declared host role is trusted by this opaque relay.
                if len(times) >= 4 * MAX_PROFILES or sum(to == recipient for _, to in times) >= 60:
                    raise ValueError("Rate limit")
                times.append((now, recipient))
                destination = self.clients.get(recipient)
                if destination:
                    await asyncio.wait_for(destination.send(raw), 5)
        except Exception:
            await ws.close(1008, "Registration or protocol error")
        finally:
            self.connections -= 1
            if self.clients.get(identity) is ws:
                self.clients.pop(identity, None)

async def run(host, port):
    relay = Relay()
    async with serve(relay.connection, host, port, max_size=MAX_MESSAGE, max_queue=8, ping_interval=20):
        await asyncio.Future()

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8787)
    args = parser.parse_args()
    asyncio.run(run(args.bind, args.port))

if __name__ == "__main__":
    main()
