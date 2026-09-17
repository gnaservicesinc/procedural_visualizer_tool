"""Bundled desktop worker. Qt owns rendering, editing, and all device I/O.

The private stdin/stdout pipe carries bounded media and command messages. No
listener or mDNS service exists until Qt sends an explicit enable command.
"""
import argparse
import asyncio
import fractions
import errno
import io
import ipaddress
import json
import logging
import re
import socket
import sys
import time
import tempfile
import uuid
from datetime import datetime, timezone
from pathlib import Path

from aiortc.sdp import SessionDescription
from aiortc import RTCConfiguration, RTCIceServer, RTCPeerConnection, RTCSessionDescription, MediaStreamTrack, RTCRtpSender
from av import AudioFrame, VideoFrame
from PIL import Image
from websockets.asyncio.client import connect
from websockets.asyncio.server import serve
from zeroconf import IPVersion, ServiceInfo
from ifaddr import get_adapters
from zeroconf.asyncio import AsyncZeroconf

from .protocol import Cipher, MAX_MESSAGE, MAX_PROFILES, b64, compact, endpoint, new_identity, profile, save_private, unb64

class Video(MediaStreamTrack):
    kind = "video"
    def __init__(self, host, remote_id):
        super().__init__()
        self.host = host
        self.remote_id = remote_id
        self.started = time.monotonic()
        self.sequence = -1
        self.pause_deadline = self.started
        self.last_image = None
        self.encoder = None
        if sys.platform == 'darwin':
            from .macos_video import Encoder
            self.encoder = Encoder()

    async def recv(self):
        media = self.host.remote_media(self.remote_id)
        if media["paused"]:
            # Keep the WebRTC clock and connection alive while deliberately
            # replacing the live picture with a blackout or frozen frame.
            now = time.monotonic()
            self.pause_deadline = max(self.pause_deadline, now)
            await asyncio.sleep(max(0, self.pause_deadline - now))
            self.pause_deadline += 1 / 30
            image = self.last_image if media["pause_mode"] == "freeze" else None
            if image is None:
                source = self.host.image
                size = source.size if source is not None else (1280, 720)
                image = Image.new("RGB", size)
        else:
            while self.host.image is None or self.sequence == self.host.sequence:
                await asyncio.sleep(0.005)
            self.sequence = self.host.sequence
            image = self.host.image
            self.last_image = image.copy()
            self.pause_deadline = time.monotonic()
        if self.encoder:
            # Packet transport bypasses aiortc's software video encoders.
            return self.encoder.encode(image,
                int((time.monotonic() - self.started) * 90000))
        frame = VideoFrame.from_image(image)
        frame.pts = int((time.monotonic() - self.started) * 90000)
        frame.time_base = fractions.Fraction(1, 90000)
        return frame

    def stop(self):
        if self.encoder:
            self.encoder.close()
        super().stop()

class Audio(MediaStreamTrack):
    kind = "audio"
    def __init__(self, host, remote_id):
        super().__init__()
        self.queue = asyncio.Queue(maxsize=5)
        self.host = host
        self.remote_id = remote_id
        self.started = time.monotonic()
        self.samples = 0
        host.audio_tracks.add(self)

    async def recv(self):
        # Continuous 20 ms clock; lack of routed audio means silence, never
        # stale buffered audio. Each viewer has its own bounded queue.
        await asyncio.sleep(max(0, self.started + self.samples / 48000 - time.monotonic()))
        media = self.host.remote_media(self.remote_id)
        if media["paused"] or media["muted"]:
            while not self.queue.empty():
                self.queue.get_nowait()
            data = bytes(3840)
        else:
            data = self.queue.get_nowait() if not self.queue.empty() else bytes(3840)
        frame = AudioFrame(format="s16", layout="stereo", samples=960)
        frame.planes[0].update(data)
        frame.sample_rate = 48000
        frame.pts = self.samples
        frame.time_base = fractions.Fraction(1, 48000)
        self.samples += 960
        return frame

    def stop(self):
        self.host.audio_tracks.discard(self)
        super().stop()

class Host:
    def __init__(self, directory):
        self.directory = Path(directory)
        identity_path = self.directory / "identity.json"
        self.identity = json.loads(identity_path.read_text()) if identity_path.exists() else new_identity()
        if not identity_path.exists():
            save_private(identity_path, self.identity)
        self.cipher = Cipher(self.identity)
        self.config_path = self.directory / "remotes.json"
        self.config = dict(remotes=[], signaling_url="", port=self.automatic_ports()[0],
                           lan=False, ice_servers=[], address_scope="local",
                           custom_networks="")
        if self.config_path.exists():
            self.config.update(json.loads(self.config_path.read_text()))
        self.validate_config(self.config)
        save_private(self.config_path, self.config)
        self.enabled = False
        self.server = None
        self.mdns = None
        self.service = None
        self.relay_task = None
        self.relay_socket = None
        self.relay_presence = {}
        self.discovery_task = None
        self.sessions = {}
        self.sockets = set()
        self.socket_peers = {}
        self.socket_details = {}
        self.monitor_task = None
        self.network_cache = None
        self.pending = {}
        self.image = None
        self.sequence = 0
        self.audio_tracks = set()
        self.state = {}
        self.request_times = {}
        self.work = set()
        self.announced_remotes = set()
        self.latest_connections = {}
        self.active_remote_ids = set()
        self.removed_directory = self.directory / "removed-remotes"

    def task(self, coroutine):
        task = asyncio.create_task(coroutine)
        self.work.add(task)
        task.add_done_callback(self.work.discard)
        return task

    def emit(self, value):
        sys.stdout.write(compact(value).decode() + "\n")
        sys.stdout.flush()

    @staticmethod
    def now_text():
        return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")

    def removed_profiles(self):
        result = []
        if not self.removed_directory.exists():
            return result
        for path in sorted(self.removed_directory.glob("*.pvtremote"), reverse=True):
            try:
                if path.stat().st_size > 65536:
                    raise ValueError("Removed Remote file is too large")
                archive = json.loads(path.read_text())
                remote = archive.get("profile", archive)
                clean = self.clean_remote(remote)
                result.append(dict(key=path.name,
                                   removed_at=str(archive.get("removed_at", ""))[:40],
                                   profile=clean))
            except Exception:
                logging.warning("Ignoring invalid removed Remote file: %s", path)
        return result

    def event_state(self, event="configured", **extra):
        return dict(event=event, profile=self.public(), config=self.config,
                    removed=self.removed_profiles(), enabled=self.enabled, **extra)

    def clean_remote(self, value):
        clean = profile(value, "pvtremote")
        client = self.client_info(value.get("client"))
        if client:
            clean["client"] = client
        if isinstance(value.get("last_endpoint"), str):
            clean["last_endpoint"] = value["last_endpoint"][:200]
        if value.get("recognized") is True:
            clean["recognized"] = True
        if value.get("connection_enabled") is False:
            clean["connection_enabled"] = False
        if isinstance(value.get("last_seen"), str):
            clean["last_seen"] = value["last_seen"][:40]
        if isinstance(value.get("manual_address"), str):
            clean["manual_address"] = value["manual_address"][:253]
        addresses = value.get("manual_addresses")
        if isinstance(addresses, list) and addresses:
            clean["manual_addresses"] = [str(item)[:64] for item in addresses[:16]]
        media = value.get("media")
        if isinstance(media, dict):
            clean["media"] = {
                "paused": media.get("paused") is True,
                "pause_mode": "freeze" if media.get("pause_mode") == "freeze" else "blackout",
                "muted": media.get("muted") is True,
                "audio_enabled": media.get("audio_enabled") is True,
            }
        last = value.get("last_connection")
        if isinstance(last, dict):
            clean["last_connection"] = self.clean_connection(last)
        return clean

    @staticmethod
    def clean_connection(value):
        clean = {}
        for key in ("endpoint", "media_endpoints", "status", "last_seen"):
            if isinstance(value.get(key), str):
                clean[key] = value[key][:1000]
        for key in ("bytes_sent", "bytes_received", "packets_lost"):
            if type(value.get(key)) is int and value[key] >= 0:
                clean[key] = value[key]
        for key in ("kbps", "rtt_ms"):
            if isinstance(value.get(key), (int, float)) and value[key] >= 0:
                clean[key] = value[key]
        if isinstance(value.get("client"), dict):
            clean["client"] = Host.client_info(value["client"])
        return clean

    def validate_config(self, config):
        # Older releases exposed a browser-source port filter. Browser and media
        # ports are selected automatically, so retaining an invisible old limit
        # would make otherwise allowed remotes fail unpredictably.
        config.pop("remote_port_min", None)
        config.pop("remote_port_max", None)
        label = config.get("label", "PVT host")
        if not isinstance(label, str) or not 1 <= len(label.strip()) <= 120:
            raise ValueError("Host name must contain 1–120 characters")
        remotes = config.get("remotes", [])
        if not isinstance(remotes, list) or len(remotes) > MAX_PROFILES:
            raise ValueError("At most 64 paired remotes are supported")
        cleaned = [self.clean_remote(item) for item in remotes]
        if len({item["id"] for item in cleaned}) != len(cleaned):
            raise ValueError("Duplicate remote identity")
        # Retire the old per-device blocks even when loading legacy settings.
        config.pop("active_control", None)
        config.pop("paused_remotes", None)
        if type(config.get("port")) is not int or not 1024 <= config["port"] <= 65535:
            raise ValueError("Port must be 1024–65535")
        if config.get("signaling_url"):
            endpoint(config["signaling_url"], True)
        ice = config.get("ice_servers", [])
        if not isinstance(ice, list) or len(ice) > 8:
            raise ValueError("At most eight ICE servers are supported")
        for server in ice:
            urls = server.get("urls", [])
            if isinstance(urls, str):
                urls = [urls]
            if not urls or len(urls) > 8 or any(not isinstance(u, str) or not u.startswith(("stun:", "stuns:", "turn:", "turns:")) for u in urls):
                raise ValueError("Invalid ICE server URL")
        if config.get("address_scope") not in ("local", "subnet", "private", "any", "custom"):
            raise ValueError("Choose an allowed address range")
        networks = config.get("custom_networks", "")
        if not isinstance(networks, str) or len(networks) > 4096:
            raise ValueError("The address list is too long")
        try:
            parsed_networks = self.parse_networks(networks)
        except (ValueError, TypeError) as error:
            raise ValueError("One of the allowed addresses or ranges is not valid") from error
        if config["address_scope"] == "custom" and not parsed_networks:
            raise ValueError("Enter at least one address or start-to-end range")
        # Loopback-only mode avoids interface enumeration, mDNS, firewall
        # exposure and non-loopback listeners entirely.
        config["lan"] = config["address_scope"] != "local"
        config["remotes"] = cleaned

    @staticmethod
    def parse_networks(text):
        result = []
        for value in text.replace(",", " ").split():
            if "-" in value:
                first, last = value.split("-", 1)
                result.extend(ipaddress.summarize_address_range(ipaddress.ip_address(first), ipaddress.ip_address(last)))
            else:
                result.append(ipaddress.ip_network(value, strict=False))
        return result

    def address_allowed(self, address):
        try:
            ip = ipaddress.ip_address(address.split("%", 1)[0])
            if isinstance(ip, ipaddress.IPv6Address) and ip.ipv4_mapped:
                ip = ip.ipv4_mapped
            if ip.is_unspecified or ip.is_multicast:
                return False
            scope = self.config["address_scope"]
            if scope == "any":
                return True
            cache_key = (scope, self.config["custom_networks"])
            cached = self.network_cache
            if cached and cached[0] == cache_key and time.monotonic() - cached[1] < 3:
                return any(ip.version == network.version and ip in network for network in cached[2])
            if scope == "local":
                networks = self.parse_networks("127.0.0.0/8 ::1/128")
            elif scope == "private":
                networks = self.parse_networks("10.0.0.0/8 172.16.0.0/12 192.168.0.0/16 fc00::/7 fe80::/10 127.0.0.0/8 ::1/128")
            elif scope == "custom":
                networks = self.parse_networks(self.config["custom_networks"])
            else:
                networks = self.parse_networks("127.0.0.0/8 ::1/128")
                for adapter in get_adapters():
                    for local in adapter.ips:
                        address = local.ip if isinstance(local.ip, str) else local.ip[0]
                        networks.append(ipaddress.ip_network(f"{address.split('%')[0]}/{local.network_prefix}", strict=False))
            self.network_cache = (cache_key, time.monotonic(), networks)
            return any(ip.version == network.version and ip in network for network in networks)
        except (ValueError, TypeError):
            return False

    @staticmethod
    def endpoint(address, port):
        return f"[{address}]:{port}" if ":" in address else f"{address}:{port}"

    @staticmethod
    def endpoint_is_loopback(value):
        try:
            address = value[1:value.index("]")] if value.startswith("[") else value.rsplit(":", 1)[0]
            return ipaddress.ip_address(address).is_loopback
        except (ValueError, TypeError):
            return False

    @staticmethod
    def client_info(value):
        # Authenticated, self-reported hints, never authorization identities.
        if not isinstance(value, dict):
            return {}
        result = {key: " ".join(str(value.get(key, "")).split())[:200]
                  for key in ("browser", "version", "platform")}
        features = value.get("features", [])
        if isinstance(features, list):
            result["features"] = [feature for feature in features
                                  if isinstance(feature, str)
                                  and re.fullmatch(r"[a-z0-9-]{1,40}", feature)][:16]
        return result

    @staticmethod
    def browser_headers(ws):
        # Older extensions did not send client metadata. The WebSocket request
        # still supplies a browser User-Agent; it is descriptive, never trusted.
        agent = ws.request.headers.get("User-Agent", "")[:1000]
        match = re.search(r"(Edg|Firefox|Chrome|Version)/([\d.]+)", agent)
        names = {"Edg": "Edge", "Firefox": "Firefox", "Chrome": "Chrome", "Version": "Safari"}
        # Edge also advertises Chrome; prefer its explicit product token.
        edge = re.search(r"Edg/([\d.]+)", agent)
        browser = f"Edge {edge[1]}" if edge else f"{names[match[1]]} {match[2]}" if match else agent
        platform = next((name for token, name in [("Android", "Android"), ("iPhone", "iOS"),
            ("iPad", "iPadOS"), ("Windows", "Windows"), ("Macintosh", "macOS"),
            ("CrOS", "ChromeOS"), ("Linux", "Linux")] if token in agent), "")
        return {"browser": browser[:200], "platform": platform}

    def remember_client(self, peer, value, endpoint=None):
        current = self.peer(peer["id"])
        if current is None:
            return
        before = dict(current)
        client = {k: v for k, v in self.client_info(value).items() if v}
        if client:
            current["client"] = dict(current.get("client", {}), **client)
        if endpoint:
            current["last_endpoint"] = endpoint
        if current != before:
            try:
                save_private(self.config_path, self.config)
            except OSError:
                logging.exception("Could not save remote connection details")
            self.emit(self.event_state())

    def remote_media(self, remote_id):
        peer = self.peer(remote_id) or {}
        media = peer.get("media", {})
        return dict(paused=media.get("paused") is True,
                    pause_mode="freeze" if media.get("pause_mode") == "freeze" else "blackout",
                    muted=media.get("muted") is True,
                    audio_enabled=media.get("audio_enabled") is True)

    def remember_presence(self, peer, payload, relay=False):
        current = self.peer(peer["id"])
        if current is None:
            return
        before = dict(current)
        current["connection_enabled"] = payload.get("connected") is not False
        self.remember_client(current, payload.get("client"))
        if relay:
            self.relay_presence[current["id"]] = time.monotonic()
        if current != before:
            save_private(self.config_path, self.config)

    def offline_rows(self):
        rows = []
        for peer in self.config["remotes"]:
            last = dict(peer.get("last_connection", {}))
            client = dict(peer.get("client", {}))
            client.update(last.get("client", {}))
            media = self.remote_media(peer["id"])
            status = "Unavailable"
            if peer.get("manual_address"):
                status = f"Looking for {peer['manual_address']} · waiting for the remote to connect"
            row = dict(last, id=peer["id"], label=peer["label"], role=peer["role"],
                       client=client, connected=False, reachable=False,
                       connection_enabled=peer.get("connection_enabled") is not False,
                       status=status, seconds=0,
                       **media)
            row.setdefault("endpoint", peer.get("last_endpoint", ""))
            row.setdefault("last_seen", peer.get("last_seen", ""))
            rows.append(row)
        return rows

    def archive_remote(self, remote):
        self.removed_directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        name = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ-") + str(uuid.uuid4()) + ".pvtremote"
        save_private(self.removed_directory / name,
                     dict(removed_at=self.now_text(), profile=remote))

    def prune_restored_archives(self):
        active = {peer["id"] for peer in self.config["remotes"]}
        for entry in self.removed_profiles():
            if entry["profile"]["id"] in active:
                (self.removed_directory / entry["key"]).unlink(missing_ok=True)

    def allowed_offer(self, sdp, source):
        description = SessionDescription.parse(sdp)
        for media in description.media:
            candidates = []
            for candidate in media.ice_candidates:
                address = candidate.ip
                # Browser privacy candidates refer to the authenticated direct
                # peer. Never resolve an arbitrary name from untrusted SDP.
                if address.endswith(".local") and source:
                    address = source
                if self.address_allowed(address):
                    candidate.ip = address
                    candidates.append(candidate)
            media.ice_candidates = candidates
            media.ice_candidates_complete = True
        if not any(m.ice_candidates for m in description.media):
            raise ValueError("The remote is outside the allowed address ranges")
        return str(description)

    def guard_ice(self, pc):
        # aiortc exposes no candidate admission callback. Keep this adapter
        # isolated and fail closed if its transport contract changes. Filtering
        # SDP alone is insufficient: ICE can learn peer-reflexive candidates.
        transports = {t.receiver.transport.transport for t in pc.getTransceivers()}
        if pc.sctp:
            transports.add(pc.sctp.transport.transport)
        for transport in transports:
            connection = transport._connection
            original_request = connection.request_received
            def request(message, addr, protocol, raw, original=original_request):
                if self.address_allowed(addr[0]):
                    original(message, addr, protocol, raw)
            connection.request_received = request
            original_data = connection.data_received
            connection.pvt_ready = False
            def data_received(data, component, original=original_data, conn=connection):
                if data is None or conn.pvt_ready:
                    original(data, component)
            connection.data_received = data_received
            original_gather = connection.gather_candidates
            async def gather(original=original_gather, conn=connection):
                await original()
                for protocol in conn._protocols:
                    if getattr(protocol, "pvt_filtered", False):
                        continue
                    receive = protocol.datagram_received
                    def received(data, addr, original=receive):
                        if self.address_allowed(addr[0]):
                            original(data, addr)
                    protocol.datagram_received = received
                    protocol.pvt_filtered = True
                conn.pvt_ready = True
            connection.gather_candidates = gather


    async def monitor(self):
        while self.enabled:
            rows = []
            now = time.monotonic()
            # A browser profile can have several tabs. Track each session rather
            # than replacing another tab that has the same authenticated identity.
            entries = [(key, session["peer"]["id"], session, session.get("socket"))
                       for key, session in list(self.sessions.items())]
            attached = {entry[3] for entry in entries if entry[3] is not None}
            entries += [(str(id(ws)), remote_id, None, ws)
                        for ws, remote_id in list(self.socket_peers.items()) if ws not in attached]
            entries += [(f"relay-{remote_id}", remote_id, None, None)
                        for remote_id, seen in list(self.relay_presence.items())
                        if now - seen < 45
                        and not any(item[1] == remote_id for item in entries)]
            active = set()
            for key, remote_id, session, ws in entries:
                peer = self.peer(remote_id)
                if not peer:
                    continue
                active.add(remote_id)
                details = self.socket_details.get(ws, {})
                client = dict(peer.get("client", {}))
                client.update({k: v for k, v in details.get("client", {}).items() if v})
                if session:
                    client.update({k: v for k, v in session.get("client", {}).items() if v})
                connected = session is not None
                connection_enabled = peer.get("connection_enabled") is not False
                row = dict(id=remote_id, session=key, label=peer["label"], role=peer["role"],
                           endpoint=details.get("endpoint", "Encrypted relay"),
                           seconds=int(now - (session["started"] if session else details.get("started", now))),
                           client=client, connected=connected, reachable=True,
                           connection_enabled=connection_enabled,
                           status=(session["pc"].connectionState if session else
                                   "Connecting" if connection_enabled else "Disconnected"),
                           **self.remote_media(remote_id))
                latency = getattr(ws, "latency", 0) if ws is not None else 0
                if latency > 0:
                    row["rtt_ms"] = round(latency * 1000, 1)
                if session:
                    stats = await session["pc"].getStats()
                    if session["pc"].sctp:
                        stats.update(session["pc"].sctp.transport._get_stats())
                    transports = [s for s in stats.values() if s.type == "transport"]
                    sent = sum(s.bytesSent for s in transports)
                    received = sum(s.bytesReceived for s in transports)
                    old_time, old_bytes = session.get("sample", (now, sent + received))
                    row.update(bytes_sent=sent, bytes_received=received,
                               kbps=round(max(0, sent + received - old_bytes) * 8 / max(.001, now - old_time) / 1000, 1))
                    session["sample"] = (now, sent + received)
                    feedback = [s for s in stats.values() if s.type == "remote-inbound-rtp"]
                    if feedback:
                        row["packets_lost"] = sum(s.packetsLost for s in feedback)
                        rtt = [s.roundTripTime for s in feedback if s.roundTripTime is not None]
                        if rtt:
                            row["rtt_ms"] = round(max(rtt) * 1000, 1)
                    transports = {t.receiver.transport.transport for t in session["pc"].getTransceivers()}
                    if session["pc"].sctp:
                        transports.add(session["pc"].sctp.transport.transport)
                    selected = {self.endpoint(*pair.remote_addr) for transport in transports
                                for pair in transport._connection._nominated.values()}
                    row["media_endpoints"] = ", ".join(sorted(selected)) or session.get("media_endpoints", "")
                self.latest_connections[remote_id] = self.clean_connection(
                    dict(row, last_seen=self.now_text()))
                # A permanent remote slot represents its authenticated identity,
                # even when several tabs happen to use that identity.
                existing = next((item for item in rows if item["id"] == remote_id), None)
                if existing:
                    was_connected = existing["connected"]
                    existing["connected"] = was_connected or connected
                    existing["status"] = ("Connected in multiple tabs"
                                          if was_connected and connected
                                          else existing["status"] if was_connected
                                          else row["status"])
                    existing["bytes_sent"] = existing.get("bytes_sent", 0) + row.get("bytes_sent", 0)
                    existing["bytes_received"] = existing.get("bytes_received", 0) + row.get("bytes_received", 0)
                    existing["kbps"] = existing.get("kbps", 0) + row.get("kbps", 0)
                else:
                    rows.append(row)
            dropped = self.active_remote_ids - active
            if dropped:
                for remote_id in dropped:
                    peer = self.peer(remote_id)
                    last = self.latest_connections.get(remote_id)
                    if peer is not None and last:
                        peer["last_connection"] = last
                        peer["last_seen"] = last.get("last_seen", self.now_text())
                save_private(self.config_path, self.config)
            self.active_remote_ids = active
            online = {row["id"] for row in rows}
            rows.extend(row for row in self.offline_rows() if row["id"] not in online)
            self.emit(dict(event="connections", connections=rows))
            await asyncio.sleep(1)


    def automatic_ports(self):
        # Stable bounded alternatives allow a paired browser to recover from a
        # port conflict without service enumeration or another pairing export.
        first = 49152 + int(self.identity["public"]["id"].replace("-", "")[:8], 16) % 16000
        return [49152 + (first - 49152 + offset) % 16000 for offset in (0, 4093, 8191, 12289)]

    def discovery_name(self):
        return f"pvt-{self.cipher.public['id']}.local"

    def public(self):
        result = dict(self.cipher.public)
        result["label"] = self.config.get("label", "PVT desktop")
        port = self.config["port"]
        result["endpoints"] = [f"ws://127.0.0.1:{port}"]
        if self.config["address_scope"] != "local":
            result["endpoints"].append(f"ws://{self.discovery_name()}:{port}")
            for address in self.lan_addresses():
                result["endpoints"].append(f"ws://[{address}]:{port}" if ":" in address else f"ws://{address}:{port}")
        result["signaling_url"] = self.config.get("signaling_url", "")
        return profile(result, "pvthost")

    @staticmethod
    def lan_addresses():
        # Hostname lookup often returns only loopback, notably on Linux.
        # Enumerate interfaces without requiring an internet route.
        addresses = set()
        for adapter in get_adapters():
            for interface in adapter.ips:
                address = interface.ip if isinstance(interface.ip, str) else interface.ip[0]
                ip = ipaddress.ip_address(address)
                if not ip.is_loopback and not ip.is_unspecified and not ip.is_link_local:
                    addresses.add(address)
        return sorted(addresses)[:12]

    async def refresh_discovery(self):
        addresses = self.lan_addresses()
        if self.service and self.service.parsed_addresses() == addresses:
            return
        # Recreate multicast sockets as well as records after interface changes.
        if self.mdns:
            await self.mdns.async_close()
            self.mdns = self.service = None
        if addresses:
            self.mdns = AsyncZeroconf(ip_version=IPVersion.All)
            self.service = ServiceInfo("_pvt._tcp.local.", f"{self.cipher.public['id']}._pvt._tcp.local.",
                addresses=[socket.inet_pton(socket.AF_INET6 if ":" in a else socket.AF_INET, a) for a in addresses], port=self.config["port"],
                properties={"id": self.cipher.public["id"], "version": "1"},
                server=self.discovery_name() + ".")
            await self.mdns.async_register_service(self.service)

    async def discover(self):
        while self.enabled:
            try:
                await self.refresh_discovery()
            except Exception:
                # Multicast failures must never tear down same-machine output.
                logging.exception("Discovery will retry")
                if self.mdns:
                    await self.mdns.async_close()
                self.mdns = self.service = None
            await asyncio.sleep(3)

    def peer(self, remote_id):
        return next((p for p in self.config["remotes"] if p["id"] == remote_id), None)

    async def enable(self):
        if self.enabled:
            return
        for port in dict.fromkeys([self.config["port"], *self.automatic_ports()]):
            try:
                self.server = await serve(self.connection, ["0.0.0.0", "::"] if self.config["address_scope"] != "local" else "127.0.0.1", port,
                    max_size=MAX_MESSAGE, max_queue=8, write_limit=65536, ping_interval=20, open_timeout=5)
                self.config["port"] = port
                break
            except OSError as error:
                if error.errno not in (errno.EADDRINUSE, errno.EACCES):
                    raise
        if not self.server:
            raise OSError("Automatic connection endpoints are busy")
        try:
            save_private(self.config_path, self.config)
        except Exception:
            await self.disable()
            raise
        self.enabled = True
        if self.config["address_scope"] != "local":
            self.discovery_task = self.task(self.discover())
        self.monitor_task = self.task(self.monitor())
        if self.config.get("signaling_url") and self.config["address_scope"] == "any":
            self.relay_task = self.task(self.relay())

    async def disable(self):
        self.enabled = False
        if self.monitor_task:
            self.monitor_task.cancel()
            await asyncio.gather(self.monitor_task, return_exceptions=True)
            self.monitor_task = None
        if self.discovery_task:
            self.discovery_task.cancel()
            await asyncio.gather(self.discovery_task, return_exceptions=True)
            self.discovery_task = None
        if self.relay_task:
            self.relay_task.cancel()
            self.relay_task = None
        if self.server:
            self.server.close()
            await self.server.wait_closed()
            self.server = None
        for session in list(self.sessions.values()):
            await session["pc"].close()
        self.sessions.clear()
        for remote_id in self.active_remote_ids:
            peer = self.peer(remote_id)
            last = self.latest_connections.get(remote_id)
            if peer is not None and last:
                peer["last_connection"] = last
                peer["last_seen"] = last.get("last_seen", self.now_text())
        if self.active_remote_ids:
            save_private(self.config_path, self.config)
        self.active_remote_ids.clear()
        if self.mdns:
            await self.mdns.async_close()
            self.mdns = self.service = None
        for future in self.pending.values():
            if not future.done():
                future.set_exception(ValueError("Networking disabled"))
        self.pending.clear()
        self.image = None
        self.emit(dict(event="connections", connections=self.offline_rows()))

    async def connection(self, ws):
        if not self.address_allowed(ws.remote_address[0]):
            await ws.close(1008, "Endpoint outside allowed ranges")
            return
        if len(self.sockets) >= MAX_PROFILES:
            await ws.close(1013, "Connection limit")
            return
        self.sockets.add(ws)
        try:
            challenge = str(uuid.uuid4())
            await ws.send(compact(dict(challenge=challenge)).decode())
            envelope = json.loads(await asyncio.wait_for(ws.recv(), 8))
            peer = self.peer(envelope.get("from"))
            if not peer:
                raise ValueError("Remote is not paired")
            hello = self.cipher.open(peer, envelope)
            if hello.get("op") != "hello" or hello.get("challenge") != challenge:
                raise ValueError("Invalid authentication challenge")
            await ws.send(compact(self.cipher.seal(peer, dict(op="hello", challenge=challenge))).decode())
            self.socket_peers[ws] = peer["id"]
            client = self.browser_headers(ws)
            client.update({k: v for k, v in self.client_info(hello.get("client")).items() if v})
            self.socket_details[ws] = dict(endpoint=self.endpoint(ws.remote_address[0], ws.remote_address[1]), started=time.monotonic(), client=client)
            self.remember_client(peer, client, self.socket_details[ws]["endpoint"])
            if not peer.get("recognized") and peer["id"] not in self.announced_remotes:
                self.announced_remotes.add(peer["id"])
                self.emit(dict(event="new_remote", remote=dict(
                    id=peer["id"], label=peer["label"], role=peer["role"],
                    client=client, endpoint=self.socket_details[ws]["endpoint"])))
            expected = set(peer.get("manual_addresses", []))
            actual = str(ipaddress.ip_address(ws.remote_address[0].split("%", 1)[0]))
            if actual in expected:
                peer.pop("manual_address", None)
                peer.pop("manual_addresses", None)
                save_private(self.config_path, self.config)
            loopback = ipaddress.ip_address(ws.remote_address[0]).is_loopback
            async def send(payload):
                await ws.send(compact(self.cipher.seal(peer, payload)).decode())
            async for raw in ws:
                if not self.enabled or not self.peer(peer["id"]):
                    break
                message = json.loads(raw)
                if message.get("version") == 1:
                    payload = self.cipher.open(peer, message)
                    if payload.get("op") == "offer":
                        if peer.get("connection_enabled") is False:
                            peer.pop("connection_enabled", None)
                            save_private(self.config_path, self.config)
                        await self.offer(peer, payload, send, ws.remote_address[0], ws)
                    elif payload.get("op") == "presence":
                        self.remember_presence(peer, payload)
                    else:
                        raise ValueError("Unexpected signaling message")
                elif loopback and message.get("op") == "command":
                    response = await self.command(peer, message)
                    await ws.send(compact(response).decode())
                else:
                    raise ValueError("Plain control is restricted to loopback")
        except Exception as error:
            logging.info("Connection ended: %s", error)
            await ws.close(1008, "Authentication or protocol error")
        finally:
            self.sockets.discard(ws)
            self.socket_peers.pop(ws, None)
            self.socket_details.pop(ws, None)

    async def offer(self, peer, payload, send, source=None, ws=None):
        if not self.enabled or not self.peer(peer["id"]):
            raise ValueError("Networking disabled or remote revoked")
        if source is None and self.config["address_scope"] != "any":
            raise ValueError("Relay connections require Any IP")
        sdp = self.allowed_offer(payload["sdp"], source)
        remote_id = peer["id"]
        session_id = payload.get("session")
        uuid.UUID(session_id)
        # A new offer replaces only the session on this signaling socket.
        # Other tabs using the same imported profile remain connected.
        for key, old in list(self.sessions.items()):
            if (ws is not None and old.get("socket") is ws) or key == session_id:
                if old["peer"]["id"] != remote_id:
                    raise ValueError("Session identity mismatch")
                self.sessions.pop(key, None)
                await old["pc"].close()
        if len(self.sessions) >= MAX_PROFILES:
            raise ValueError("Connection limit reached")
        ice = [RTCIceServer(**server) for server in self.config.get("ice_servers", [])]
        pc = RTCPeerConnection(RTCConfiguration(iceServers=ice))
        session = dict(pc=pc, peer=peer, channel=None, id=session_id, socket=ws, started=time.monotonic(),
                       client=self.client_info(payload.get("client")),
                       media_endpoints=", ".join(self.endpoint(c.ip,c.port) for m in SessionDescription.parse(sdp).media for c in m.ice_candidates))
        self.sessions[session_id] = session
        self.remember_client(peer, payload.get("client"))
        @pc.on("datachannel")
        def datachannel(channel):
            if channel.label != "pvt-control" or session["channel"] is not None:
                channel.close()
                return
            session["channel"] = channel
            @channel.on("message")
            def message(raw):
                async def handle():
                    try:
                        if not isinstance(raw, str) or len(raw) > 65536 or self.sessions.get(session_id) is not session:
                            raise ValueError("Invalid data channel message")
                        result = await self.command(peer, json.loads(raw))
                        if channel.readyState == "open" and channel.bufferedAmount < MAX_MESSAGE:
                            await self.send_result(channel, result)
                    except Exception:
                        channel.close()
                if len(self.work) < 64:
                    self.task(handle())
                else:
                    channel.close()
        @pc.on("connectionstatechange")
        async def state_change():
            if pc.connectionState in ("failed", "closed"):
                if self.sessions.get(session_id) is session:
                    self.sessions.pop(session_id, None)
                if pc.connectionState != "closed":
                    await pc.close()
        try:
            if peer['role'] == 'display' and sys.platform == 'darwin':
                codecs = [codec for codec in RTCRtpSender.getCapabilities('video').codecs
                          if codec.mimeType.lower() == 'video/h264'
                          and str(codec.parameters.get('packetization-mode')) == '1']
                if not codecs:
                    raise RuntimeError('Remote video requires H264 support')
                # Preferences must precede offer processing: aiortc resolves
                # the sender codecs while applying the remote description.
                transceiver = pc.addTransceiver('video', direction='sendonly')
                transceiver.setCodecPreferences(codecs)
            await pc.setRemoteDescription(RTCSessionDescription(sdp=sdp, type="offer"))
            self.guard_ice(pc)
            if peer["role"] == "display":
                pc.addTrack(Video(self, remote_id))
                pc.addTrack(Audio(self, remote_id))
            await pc.setLocalDescription(await pc.createAnswer())
            await send(dict(op="answer", session=session_id, sdp=pc.localDescription.sdp))
        except Exception:
            if self.sessions.get(session_id) is session:
                self.sessions.pop(session_id, None)
            await pc.close()
            raise

    async def send_result(self, channel, result):
        # SCTP peers commonly negotiate a 64 KiB maximum user message. A real
        # project registry can exceed it, so split large replies explicitly.
        raw = compact(result)
        if len(raw) > 4 * MAX_MESSAGE:
            raw = compact(dict(op="result", id=result["id"], ok=False,
                               error="Project state exceeds the 4 MiB remote limit"))
        if len(raw) <= 24000:
            channel.send(raw.decode())
            return
        chunks = [raw[offset:offset + 16384] for offset in range(0, len(raw), 16384)]
        deadline = time.monotonic() + 5
        for index, chunk in enumerate(chunks):
            while channel.bufferedAmount > 512 * 1024:
                if channel.readyState != "open" or time.monotonic() > deadline:
                    raise ValueError("Data channel backpressure timeout")
                await asyncio.sleep(0.01)
            if channel.readyState != "open":
                return
            channel.send(compact(dict(op="chunk", id=result["id"], index=index,
                                     count=len(chunks), data=b64(chunk))).decode())

    async def send_remote_control(self, remote_id, action, **fields):
        peer = self.peer(remote_id)
        if peer is None:
            return False
        sent = False
        payload = dict(op="remote_control", action=action, **fields)
        message = compact(payload).decode()
        # A deliberately disconnected remote keeps only this authenticated
        # signaling path alive. It is reachable without running WebRTC media or
        # retrying the media connection, so PVT can explicitly turn it back on.
        for ws, socket_remote in list(self.socket_peers.items()):
            if socket_remote != remote_id:
                continue
            try:
                await ws.send(compact(self.cipher.seal(peer, payload)).decode())
                sent = True
            except Exception:
                pass
        for session in self.sessions.values():
            if session["peer"]["id"] != remote_id:
                continue
            channel = session.get("channel")
            if (session.get("socket") is None and channel
                    and channel.readyState == "open" and channel.bufferedAmount < 65536):
                channel.send(message)
                sent = True
        if (not sent and self.relay_socket
                and time.monotonic() - self.relay_presence.get(remote_id, 0) < 45):
            try:
                await self.relay_socket.send(
                    compact(self.cipher.seal(peer, payload)).decode())
                sent = True
            except Exception:
                pass
        return sent

    def update_remote_media(self, peer, values):
        media = self.remote_media(peer["id"])
        for key in ("paused", "muted", "audio_enabled"):
            if key in values:
                if type(values[key]) is not bool:
                    raise ValueError("Remote media switches must be true or false")
                media[key] = values[key]
        if "pause_mode" in values:
            if values["pause_mode"] not in ("blackout", "freeze"):
                raise ValueError("Pause mode must be blackout or freeze")
            media["pause_mode"] = values["pause_mode"]
        peer["media"] = media
        save_private(self.config_path, self.config)
        return media

    async def command(self, peer, message):
        request_id = message.get("id")
        if not isinstance(request_id, str) or len(request_id) > 80:
            raise ValueError("Invalid command identifier")
        result = dict(op="result", id=request_id)
        try:
            if not self.enabled or not self.peer(peer["id"]):
                raise ValueError("Remote revoked or networking disabled")
            action = message.get("action")
            if action not in ("state", "background", "set", "live", "playback", "undo", "redo", "remote_media", "remote_connection"):
                raise ValueError("Unknown action")
            if action == "remote_media" and peer["role"] != "display":
                raise ValueError("Only Remote Display can control its media")
            if action == "remote_connection" and peer["role"] != "display":
                raise ValueError("Only Remote Display can disconnect itself")
            if action not in ("state", "background", "remote_media", "remote_connection") and peer["role"] != "control":
                raise ValueError("Display remotes cannot edit PVT")
            now = time.monotonic()
            history = [t for t in self.request_times.get(peer["id"], []) if t > now - 1]
            if len(history) >= 60:
                raise ValueError("Control rate limit exceeded")
            history.append(now)
            self.request_times[peer["id"]] = history
            if action == "state":
                # Older extensions use this response field to enable editing.
                result.update(ok=True, state=self.state,
                              active_control=peer["id"] if peer["role"] == "control" else "",
                              remote_media=self.remote_media(peer["id"]))
            elif action == "remote_media":
                fields = {key: message[key] for key in
                          ("paused", "pause_mode", "muted", "audio_enabled")
                          if key in message}
                media = self.update_remote_media(peer, fields) if fields else self.remote_media(peer["id"])
                result.update(ok=True, remote_media=media)
            elif action == "remote_connection":
                if message.get("enabled") is not False:
                    raise ValueError("A connected remote can only request Disconnect")
                peer["connection_enabled"] = False
                save_private(self.config_path, self.config)
                result.update(ok=True)
            else:
                if len(self.pending) >= 32:
                    raise ValueError("Desktop is busy")
                token = str(uuid.uuid4())
                future = asyncio.get_running_loop().create_future()
                self.pending[token] = future
                self.emit(dict(event="command", token=token, remote=peer["id"], command=message))
                try:
                    reply = await asyncio.wait_for(future, 5)
                    result.update(reply)
                finally:
                    self.pending.pop(token, None)
        except Exception as error:
            result.update(ok=False, error=str(error))
        return result

    async def relay(self):
        delay = 1
        while self.enabled:
            try:
                async with connect(self.config["signaling_url"], max_size=MAX_MESSAGE, max_queue=8) as ws:
                    self.relay_socket = ws
                    challenge = json.loads(await asyncio.wait_for(ws.recv(), 10))["challenge"]
                    public = self.cipher.public
                    proof = b64(self.cipher.ed.sign(compact(["pvt-relay-v1", public["id"], challenge])))
                    await ws.send(compact(dict(op="register", id=public["id"], key=public["ed25519"], signature=proof)).decode())
                    delay = 1
                    async for raw in ws:
                        envelope = json.loads(raw)
                        peer = self.peer(envelope.get("from"))
                        if not peer:
                            continue
                        try:
                            payload = self.cipher.open(peer, envelope)
                            if payload.get("op") == "offer":
                                if peer.get("connection_enabled") is False:
                                    peer.pop("connection_enabled", None)
                                    save_private(self.config_path, self.config)
                                async def send(value, destination=peer):
                                    await ws.send(compact(self.cipher.seal(destination, value)).decode())
                                await self.offer(peer, payload, send)
                            elif payload.get("op") == "presence":
                                self.remember_presence(peer, payload, relay=True)
                        except Exception as error:
                            logging.info("Rejected signaling: %s", error)
            except Exception as error:
                self.emit(dict(event="status", error=f"Reconnecting: {error}"))
            finally:
                self.relay_socket = None
                self.relay_presence.clear()
            await asyncio.sleep(delay)
            delay = min(delay * 2, 30)

    async def input(self, message):
        op = message.get("op")
        if op == "configure":
            candidate = dict(self.config, **message["config"])
            try:
                self.validate_config(candidate)
                save_private(self.config_path, candidate)
            except Exception as error:
                logging.warning("Pairing configuration rejected: %s", error)
                self.emit(self.event_state("rejected", error=str(error)))
                return True
            wanted = message.get("enabled", self.enabled)
            restart = any(candidate.get(key) != self.config.get(key)
                          for key in ("port", "lan", "signaling_url", "ice_servers", "address_scope", "custom_networks"))
            previous = {p["id"]: p for p in self.config["remotes"]}
            self.config = candidate  # Permission checks see the handoff immediately.
            self.prune_restored_archives()
            revoked = {key for key, old in previous.items() if not self.peer(key) or any(
                self.peer(key).get(field) != old.get(field) for field in ("role", "ed25519", "x25519"))}
            for ws, remote_id in list(self.socket_peers.items()):
                if remote_id in revoked:
                    await ws.close(1008, "Pairing revoked")
            for key, session in list(self.sessions.items()):
                if session["peer"]["id"] in revoked:
                    self.sessions.pop(key, None)
                    await session["pc"].close()
            if restart or not wanted:
                await self.disable()
            if wanted:
                await self.enable()
            self.emit(self.event_state())
            if not self.enabled:
                self.emit(dict(event="connections", connections=self.offline_rows()))
        elif op == "enable":
            if message.get("enabled"):
                await self.enable()
            else:
                await self.disable()
            self.emit(self.event_state())
            if not self.enabled:
                self.emit(dict(event="connections", connections=self.offline_rows()))
        elif op == "rename_remote":
            peer = self.peer(message.get("remote"))
            name = " ".join(str(message.get("name", "")).split())
            if not peer:
                raise ValueError("Remote is no longer paired")
            if not 1 <= len(name) <= 120:
                raise ValueError("Remote name must contain 1–120 characters")
            if any(other["id"] != peer["id"] and other["label"].casefold() == name.casefold()
                   for other in self.config["remotes"]):
                raise ValueError("Each remote must have a unique name")
            peer["label"] = name
            peer["recognized"] = True
            save_private(self.config_path, self.config)
            self.emit(self.event_state())
            if not self.enabled:
                self.emit(dict(event="connections", connections=self.offline_rows()))
        elif op == "remove_remote":
            peer = self.peer(message.get("remote"))
            if not peer:
                raise ValueError("Remote is no longer paired")
            self.archive_remote(peer)
            self.config["remotes"] = [item for item in self.config["remotes"]
                                      if item["id"] != peer["id"]]
            save_private(self.config_path, self.config)
            for ws, remote_id in list(self.socket_peers.items()):
                if remote_id == peer["id"]:
                    await ws.close(1008, "Pairing removed")
            for key, session in list(self.sessions.items()):
                if session["peer"]["id"] == peer["id"]:
                    self.sessions.pop(key, None)
                    await session["pc"].close()
            self.emit(self.event_state())
            self.emit(dict(event="connections", connections=self.offline_rows()))
        elif op == "recover_remote":
            key = str(message.get("key", ""))
            path = self.removed_directory / key
            if path.parent != self.removed_directory or not path.is_file() or path.suffix != ".pvtremote":
                raise ValueError("Removed Remote file was not found")
            archive = json.loads(path.read_text())
            peer = self.clean_remote(archive.get("profile", archive))
            if self.peer(peer["id"]):
                raise ValueError("That remote is already present")
            if len(self.config["remotes"]) >= MAX_PROFILES:
                raise ValueError("At most 64 paired remotes are supported")
            if any(item["label"].casefold() == peer["label"].casefold()
                   for item in self.config["remotes"]):
                peer["label"] += " (recovered)"
            self.config["remotes"].append(peer)
            self.validate_config(self.config)
            save_private(self.config_path, self.config)
            path.unlink()
            self.emit(self.event_state())
            self.emit(dict(event="connections", connections=self.offline_rows()))
        elif op == "locate_remote":
            peer = self.peer(message.get("remote"))
            target = str(message.get("address", "")).strip()
            if not peer:
                raise ValueError("Remote is no longer paired")
            if not target or len(target) > 253 or not re.fullmatch(r"[A-Za-z0-9._:-]+", target):
                raise ValueError("Enter an IP address or hostname")
            host = target[1:-1] if target.startswith("[") and target.endswith("]") else target
            infos = await asyncio.get_running_loop().getaddrinfo(host, None,
                type=socket.SOCK_STREAM)
            addresses = sorted({str(ipaddress.ip_address(item[4][0].split("%", 1)[0]))
                                for item in infos})
            if not addresses:
                raise ValueError("That address could not be found")
            if not any(self.address_allowed(address) for address in addresses):
                raise ValueError("That address is outside the current accepted connection range")
            peer["manual_address"] = target
            peer["manual_addresses"] = addresses
            save_private(self.config_path, self.config)
            self.emit(self.event_state())
            self.emit(dict(event="connections", connections=self.offline_rows()))
        elif op == "remote_action":
            peer = self.peer(message.get("remote"))
            action = message.get("action")
            if not peer:
                raise ValueError("Remote is no longer paired")
            if action in ("pause", "unpause", "mute", "unmute"):
                if peer["role"] != "display":
                    raise ValueError("That media action is only available for Remote Display")
                values = ({"paused": True, "pause_mode": message.get("pause_mode", "blackout")}
                          if action == "pause" else {"paused": False}
                          if action == "unpause" else {"muted": action == "mute"})
                media = self.update_remote_media(peer, values)
                await self.send_remote_control(peer["id"], "media", media=media)
            elif action in ("disconnect", "reconnect"):
                feature = "remote-disconnect-v1" if action == "disconnect" else "remote-reconnect-v1"
                features = peer.get("client", {}).get("features", [])
                if feature not in features:
                    raise ValueError("Update this remote extension to use that connection action")
                if action == "disconnect" and peer["role"] != "display":
                    raise ValueError("PVT only disconnects Remote Display")
                if action == "reconnect" and peer["role"] != "display" \
                        and not self.endpoint_is_loopback(peer.get("last_endpoint", "")):
                    raise ValueError("Remote Control can only be reconnected from this computer")
                if action == "reconnect" and peer.get("connection_enabled") is not False:
                    raise ValueError("The remote is not intentionally disconnected")
                if not await self.send_remote_control(peer["id"], action):
                    raise ValueError("The remote is unavailable")
                if action == "disconnect":
                    peer["connection_enabled"] = False
                else:
                    peer.pop("connection_enabled", None)
                save_private(self.config_path, self.config)
            else:
                raise ValueError("Unknown remote action")
            self.emit(self.event_state())
        elif op == "reply":
            future = self.pending.get(message.get("token"))
            if future and not future.done():
                future.set_result(message["result"])
        elif op == "state":
            self.state = message["state"]
        elif op == "video" and self.enabled:
            raw = unb64(message["jpeg"])
            image = Image.open(io.BytesIO(raw))
            if image.width > 1920 or image.height > 1080:
                raise ValueError("Video frame exceeds stream resolution")
            self.image = image.convert("RGB")
            self.sequence += 1
        elif op == "audio" and self.enabled:
            data = unb64(message["pcm"], 3840)
            for track in self.audio_tracks:
                if track.queue.full():
                    track.queue.get_nowait()
                track.queue.put_nowait(data)
        elif op == "shutdown":
            return False
        return True

    async def run(self):
        self.emit(self.event_state("ready"))
        self.emit(dict(event="connections", connections=self.offline_rows()))
        try:
            while True:
                raw = await asyncio.to_thread(sys.stdin.buffer.readline, 4 * MAX_MESSAGE)
                if not raw:
                    break
                if len(raw) >= 4 * MAX_MESSAGE:
                    raise ValueError("Desktop message too large")
                message = {}
                try:
                    message = json.loads(raw)
                    if not await self.input(message):
                        break
                except Exception as error:
                    if isinstance(message, dict) and message.get("op") == "configure":
                        self.emit(self.event_state())
                    self.emit(dict(event="status", error=str(error), enabled=self.enabled,
                                   operation=message.get("op", "") if isinstance(message, dict) else ""))
        finally:
            await self.disable()
            for task in list(self.work):
                task.cancel()
            await asyncio.gather(*self.work, return_exceptions=True)


async def self_test():
    from aiortc.codecs import get_encoder
    from aiortc.rtcrtpparameters import RTCRtpCodecParameters
    with tempfile.TemporaryDirectory() as directory:
        host = Host(directory)
        host.config["lan"] = False
        await host.enable()
        try:
            remote = Cipher(new_identity("pvtremote", "display"))
            host.config["remotes"] = [remote.public]
            # This smoke test connects only to our loopback listener. Build
            # proxies (including Launchpad's) cannot route to that listener.
            async with connect(host.public()["endpoints"][0], proxy=None) as ws:
                challenge = json.loads(await ws.recv())["challenge"]
                await ws.send(compact(remote.seal(host.cipher.public, dict(op="hello", challenge=challenge))).decode())
                assert remote.open(host.cipher.public, json.loads(await ws.recv()))["challenge"] == challenge
            frame = VideoFrame.from_image(Image.new("RGB", (64, 64)))
            frame.pts = 0
            frame.time_base = fractions.Fraction(1, 90000)
            if sys.platform == 'darwin':
                from .macos_video import Encoder
                encoder = Encoder()
                try:
                    packet = encoder.encode(Image.new('RGB', (64, 64)), 0)
                    assert get_encoder(RTCRtpCodecParameters(mimeType='video/H264', clockRate=90000)).pack(packet)[0]
                finally:
                    encoder.close()
            else:
                assert get_encoder(RTCRtpCodecParameters(mimeType="video/VP8", clockRate=90000)).encode(frame)[0]
            audio = Audio(host, remote.public["id"])
            try:
                assert get_encoder(RTCRtpCodecParameters(mimeType="audio/opus", clockRate=48000, channels=2)).encode(await audio.recv())[0]
            finally:
                audio.stop()
        finally:
            await host.disable()
    print("PVT remote self-test passed")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--directory")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    logging.basicConfig(stream=sys.stderr, level=logging.WARNING)
    if args.self_test:
        asyncio.run(self_test())
    elif args.directory:
        asyncio.run(Host(args.directory).run())
    else:
        parser.error("--directory is required")

if __name__ == "__main__":
    main()
