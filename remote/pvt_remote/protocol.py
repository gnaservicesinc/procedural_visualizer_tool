"""Pinned identities, signed encrypted envelopes, and bounded replay protection.

Pairing files contain PUBLIC keys only. Ed25519 authenticates an envelope;
X25519/HKDF-SHA256 derives a direction-specific AES-256-GCM signaling key.
DTLS-SRTP and SCTP protect the subsequent WebRTC media and data channels.
"""
import base64
import json
import os
import time
import uuid
from collections import OrderedDict
from pathlib import Path
from urllib.parse import urlsplit

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ed25519, x25519
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.hashes import SHA256
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

MAX_MESSAGE = 1024 * 1024
MAX_PROFILES = 64

def b64(data):
    return base64.b64encode(data).decode("ascii")

def unb64(data, length=None):
    if not isinstance(data, str) or len(data) > MAX_MESSAGE:
        raise ValueError("Invalid encoded field")
    result = base64.b64decode(data, validate=True)
    if length is not None and len(result) != length:
        raise ValueError("Invalid key or nonce length")
    return result

def compact(value):
    return json.dumps(value, separators=(",", ":"), ensure_ascii=False).encode()

def endpoint(value, relay=False):
    if not isinstance(value, str) or len(value) > 2048:
        raise ValueError("Invalid endpoint")
    url = urlsplit(value)
    if url.scheme not in ("ws", "wss") or not url.hostname or url.username or url.password or url.fragment:
        raise ValueError("Expected a WebSocket URL without credentials")
    if relay and url.scheme != "wss" and url.hostname not in ("localhost", "127.0.0.1", "::1"):
        raise ValueError("Public signaling requires wss://")
    return value

def profile(value, expected=None):
    if not isinstance(value, dict) or value.get("version") != 1 or value.get("type") not in ("pvthost", "pvtremote"):
        raise ValueError("Unsupported pairing file")
    if expected and value["type"] != expected:
        raise ValueError("Wrong pairing file type")
    uuid.UUID(value["id"])
    if not isinstance(value.get("label"), str) or not 1 <= len(value["label"]) <= 120:
        raise ValueError("Name must contain 1–120 characters")
    unb64(value["ed25519"], 32)
    unb64(value["x25519"], 32)
    clean = {key: value[key] for key in ("version", "type", "id", "label", "ed25519", "x25519")}
    if value["type"] == "pvtremote":
        if value.get("role") not in ("display", "control"):
            raise ValueError("Invalid remote role")
        clean["role"] = value["role"]
    else:
        urls = value.get("endpoints", [])
        if not isinstance(urls, list) or len(urls) > 16:
            raise ValueError("Too many endpoints")
        clean["endpoints"] = [endpoint(url) for url in urls]
        clean["signaling_url"] = endpoint(value["signaling_url"], True) if value.get("signaling_url") else ""
    return clean

def new_identity(kind="pvthost", role=None):
    ed = ed25519.Ed25519PrivateKey.generate()
    x = x25519.X25519PrivateKey.generate()
    public = dict(version=1, type=kind, id=str(uuid.uuid4()), label="PVT host" if kind == "pvthost" else "PVT remote",
                  ed25519=b64(ed.public_key().public_bytes_raw()), x25519=b64(x.public_key().public_bytes_raw()))
    if role:
        public["role"] = role
    if kind == "pvthost":
        public.update(endpoints=[], signaling_url="")
    return dict(public=public, ed_private=b64(ed.private_bytes_raw()), x_private=b64(x.private_bytes_raw()))

def save_private(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    temporary = path.with_name(path.name + ".tmp-" + str(uuid.uuid4()))
    try:
        fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w") as output:
            json.dump(value, output, indent=2)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)

class Cipher:
    def __init__(self, identity):
        self.identity = identity
        self.public = profile(identity["public"])
        self.ed = ed25519.Ed25519PrivateKey.from_private_bytes(unb64(identity["ed_private"], 32))
        self.x = x25519.X25519PrivateKey.from_private_bytes(unb64(identity["x_private"], 32))
        if b64(self.ed.public_key().public_bytes_raw()) != self.public["ed25519"] or b64(self.x.public_key().public_bytes_raw()) != self.public["x25519"]:
            raise ValueError("Identity key mismatch")
        self.seen = OrderedDict()

    def key(self, peer, sender, receiver):
        shared = self.x.exchange(x25519.X25519PublicKey.from_public_bytes(unb64(peer["x25519"], 32)))
        return HKDF(algorithm=SHA256(), length=32, salt=bytes(32), info=f"pvt-remotes/v1/{sender}/{receiver}".encode()).derive(shared)

    @staticmethod
    def header(e):
        return [1, e["from"], e["to"], e["id"], e["ts"], e["nonce"]]

    def seal(self, peer, payload):
        e = dict(version=1, **{"from": self.public["id"]}, to=peer["id"], id=str(uuid.uuid4()), ts=int(time.time()), nonce=b64(os.urandom(12)))
        aad = compact(self.header(e))
        e["ct"] = b64(AESGCM(self.key(peer, e["from"], e["to"])).encrypt(unb64(e["nonce"], 12), compact(payload), aad))
        e["sig"] = b64(self.ed.sign(compact(self.header(e) + [e["ct"]])))
        return e

    def open(self, peer, e):
        if len(compact(e)) > MAX_MESSAGE or e.get("version") != 1 or e.get("from") != peer["id"] or e.get("to") != self.public["id"]:
            raise ValueError("Wrong envelope identity")
        now = time.time()
        if type(e.get("ts")) is not int or abs(now - e["ts"]) > 120:
            raise ValueError("Expired envelope; check both device clocks")
        uuid.UUID(e["id"])
        token = (e["from"], e["id"])
        while self.seen and next(iter(self.seen.values())) < now - 240:
            self.seen.popitem(last=False)
        if token in self.seen:
            raise ValueError("Replayed envelope")
        # Never evict fresh replay entries to accept new traffic.
        if len(self.seen) >= 8192:
            raise ValueError("Signaling rate limit exceeded")
        ed25519.Ed25519PublicKey.from_public_bytes(unb64(peer["ed25519"], 32)).verify(unb64(e["sig"], 64), compact(self.header(e) + [e["ct"]]))
        clear = AESGCM(self.key(peer, e["from"], e["to"])).decrypt(unb64(e["nonce"], 12), unb64(e["ct"]), compact(self.header(e)))
        payload = json.loads(clear)
        if not isinstance(payload, dict):
            raise ValueError("Expected message object")
        self.seen[token] = now
        return payload
