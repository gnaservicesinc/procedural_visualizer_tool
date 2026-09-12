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
import platform
import socket
import sys
import time
import tempfile
import uuid
from pathlib import Path

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
    def __init__(self, host):
        super().__init__()
        self.host = host
        self.started = time.monotonic()
        self.sequence = -1
        self.encoder = None
        if sys.platform == 'darwin':
            from .macos_video import Encoder
            self.encoder = Encoder()

    async def recv(self):
        while self.host.image is None or self.sequence == self.host.sequence:
            await asyncio.sleep(0.005)
        self.sequence = self.host.sequence
        if self.encoder:
            # Packet transport bypasses aiortc's software video encoders.
            return self.encoder.encode(self.host.image,
                int((time.monotonic() - self.started) * 90000))
        frame = VideoFrame.from_image(self.host.image)
        frame.pts = int((time.monotonic() - self.started) * 90000)
        frame.time_base = fractions.Fraction(1, 90000)
        return frame

    def stop(self):
        if self.encoder:
            self.encoder.close()
        super().stop()

class Audio(MediaStreamTrack):
    kind = "audio"
    def __init__(self, host):
        super().__init__()
        self.queue = asyncio.Queue(maxsize=5)
        self.host = host
        self.started = time.monotonic()
        self.samples = 0
        host.audio_tracks.add(self)

    async def recv(self):
        # Continuous 20 ms clock; lack of routed audio means silence, never
        # stale buffered audio. Each viewer has its own bounded queue.
        await asyncio.sleep(max(0, self.started + self.samples / 48000 - time.monotonic()))
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
        self.config = dict(remotes=[], active_control="", signaling_url="", port=self.automatic_ports()[0], lan=True, ice_servers=[])
        if self.config_path.exists():
            self.config.update(json.loads(self.config_path.read_text()))
        # Migrate previous opt-in LAN settings to automatic reachability.
        # Qt's persisted enable switch still controls whether any listener runs.
        self.config["lan"] = True
        self.validate_config(self.config)
        self.enabled = False
        self.server = None
        self.mdns = None
        self.service = None
        self.relay_task = None
        self.discovery_task = None
        self.sessions = {}
        self.sockets = set()
        self.socket_peers = {}
        self.pending = {}
        self.image = None
        self.sequence = 0
        self.audio_tracks = set()
        self.state = {}
        self.request_times = {}
        self.work = set()

    def task(self, coroutine):
        task = asyncio.create_task(coroutine)
        self.work.add(task)
        task.add_done_callback(self.work.discard)
        return task

    def emit(self, value):
        sys.stdout.write(compact(value).decode() + "\n")
        sys.stdout.flush()

    def validate_config(self, config):
        label = config.get("label", "PVT host")
        if not isinstance(label, str) or not 1 <= len(label.strip()) <= 120:
            raise ValueError("Host name must contain 1–120 characters")
        remotes = config.get("remotes", [])
        if not isinstance(remotes, list) or len(remotes) > MAX_PROFILES:
            raise ValueError("At most 64 paired remotes are supported")
        cleaned = [profile(item, "pvtremote") for item in remotes]
        if len({item["id"] for item in cleaned}) != len(cleaned):
            raise ValueError("Duplicate remote identity")
        active = config.get("active_control", "")
        if active and not any(item["id"] == active and item["role"] == "control" for item in cleaned):
            raise ValueError("Active controller must be an imported control remote")
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
        config["remotes"] = cleaned

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
        if self.config.get("lan"):
            result["endpoints"].append(f"ws://{self.discovery_name()}:{port}")
            for address in self.lan_addresses():
                result["endpoints"].append(f"ws://{address}:{port}")
        result["signaling_url"] = self.config.get("signaling_url", "")
        return profile(result, "pvthost")

    @staticmethod
    def lan_addresses():
        # Hostname lookup often returns only loopback, notably on Linux.
        # Enumerate interfaces without requiring an internet route.
        return sorted({ip.ip for adapter in get_adapters() for ip in adapter.ips
                       if isinstance(ip.ip, str)
                       and not ipaddress.ip_address(ip.ip).is_loopback
                       and not ipaddress.ip_address(ip.ip).is_unspecified})[:12]

    async def refresh_discovery(self):
        addresses = self.lan_addresses()
        if self.service and self.service.parsed_addresses() == addresses:
            return
        # Recreate multicast sockets as well as records after interface changes.
        if self.mdns:
            await self.mdns.async_close()
            self.mdns = self.service = None
        if addresses:
            self.mdns = AsyncZeroconf(ip_version=IPVersion.V4Only)
            self.service = ServiceInfo("_pvt._tcp.local.", f"{self.cipher.public['id']}._pvt._tcp.local.",
                addresses=[socket.inet_aton(a) for a in addresses], port=self.config["port"],
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
                self.server = await serve(self.connection, "0.0.0.0" if self.config.get("lan") else "127.0.0.1", port,
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
        if self.config.get("lan"):
            self.discovery_task = self.task(self.discover())
        if self.config.get("signaling_url"):
            self.relay_task = self.task(self.relay())

    async def disable(self):
        self.enabled = False
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
        if self.mdns:
            await self.mdns.async_close()
            self.mdns = self.service = None
        for future in self.pending.values():
            if not future.done():
                future.set_exception(ValueError("Networking disabled"))
        self.pending.clear()
        self.image = None

    async def connection(self, ws):
        if len(self.sockets) >= 32:
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
                        await self.offer(peer, payload, send)
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

    async def offer(self, peer, payload, send):
        if not self.enabled or not self.peer(peer["id"]):
            raise ValueError("Networking disabled or remote revoked")
        remote_id = peer["id"]
        session_id = payload.get("session")
        uuid.UUID(session_id)
        previous = self.sessions.pop(remote_id, None)
        if previous:
            await previous["pc"].close()
        if len(self.sessions) >= 16:
            raise ValueError("Viewer limit reached")
        ice = [RTCIceServer(**server) for server in self.config.get("ice_servers", [])]
        pc = RTCPeerConnection(RTCConfiguration(iceServers=ice))
        session = dict(pc=pc, peer=peer, channel=None, id=session_id)
        self.sessions[remote_id] = session
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
                        if not isinstance(raw, str) or len(raw) > 65536 or self.sessions.get(remote_id) is not session:
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
                if self.sessions.get(remote_id) is session:
                    self.sessions.pop(remote_id, None)
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
            await pc.setRemoteDescription(RTCSessionDescription(sdp=payload["sdp"], type="offer"))
            if peer["role"] == "display":
                pc.addTrack(Video(self))
                pc.addTrack(Audio(self))
            await pc.setLocalDescription(await pc.createAnswer())
            await send(dict(op="answer", session=session_id, sdp=pc.localDescription.sdp))
        except Exception:
            if self.sessions.get(remote_id) is session:
                self.sessions.pop(remote_id, None)
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

    async def command(self, peer, message):
        request_id = message.get("id")
        if not isinstance(request_id, str) or len(request_id) > 80:
            raise ValueError("Invalid command identifier")
        result = dict(op="result", id=request_id)
        try:
            if not self.enabled or not self.peer(peer["id"]):
                raise ValueError("Remote revoked or networking disabled")
            action = message.get("action")
            if action not in ("state", "background", "set", "live", "playback", "undo", "redo"):
                raise ValueError("Unknown action")
            if action not in ("state", "background") and (peer["role"] != "control" or self.config["active_control"] != peer["id"]):
                raise ValueError("Select this remote as Active Control Remote in PVT")
            now = time.monotonic()
            history = [t for t in self.request_times.get(peer["id"], []) if t > now - 1]
            if len(history) >= 60:
                raise ValueError("Control rate limit exceeded")
            history.append(now)
            self.request_times[peer["id"]] = history
            if action == "state":
                result.update(ok=True, state=self.state, active_control=self.config["active_control"])
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
                                async def send(value, destination=peer):
                                    await ws.send(compact(self.cipher.seal(destination, value)).decode())
                                await self.offer(peer, payload, send)
                        except Exception as error:
                            logging.info("Rejected signaling: %s", error)
            except Exception as error:
                self.emit(dict(event="status", error=f"Reconnecting: {error}"))
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
                self.emit(dict(event="rejected", profile=self.public(), config=self.config, enabled=self.enabled))
                return True
            wanted = message.get("enabled", self.enabled)
            restart = any(candidate.get(key) != self.config.get(key)
                          for key in ("port", "lan", "signaling_url", "ice_servers"))
            previous = {p["id"]: p for p in self.config["remotes"]}
            self.config = candidate  # Permission checks see the handoff immediately.
            revoked = {key for key, old in previous.items() if self.peer(key) != old}
            for ws, remote_id in list(self.socket_peers.items()):
                if remote_id in revoked:
                    await ws.close(1008, "Pairing revoked")
            for remote_id in revoked:
                session = self.sessions.pop(remote_id, None)
                if session:
                    await session["pc"].close()
            if restart or not wanted:
                await self.disable()
            if wanted:
                await self.enable()
            self.emit(dict(event="configured", profile=self.public(), config=self.config, enabled=self.enabled))
        elif op == "enable":
            if message.get("enabled"):
                await self.enable()
            else:
                await self.disable()
            self.emit(dict(event="configured", profile=self.public(), config=self.config, enabled=self.enabled))
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
        self.emit(dict(event="ready", profile=self.public(), config=self.config, enabled=False))
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
                        self.emit(dict(event="configured", config=self.config, profile=self.public(), enabled=self.enabled))
                    self.emit(dict(event="status", error=str(error), enabled=self.enabled))
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
            async with connect(host.public()["endpoints"][0]) as ws:
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
            audio = Audio(host)
            try:
                assert get_encoder(RTCRtpCodecParameters(mimeType="audio/opus", clockRate=48000, channels=2)).encode(await audio.recv())[0]
            finally:
                audio.stop()
        finally:
            await host.disable()
    print("PVT remote self-test passed")


def main():
    if sys.platform == 'darwin' and int(platform.mac_ver()[0].split('.')[0]) < 27:
        raise SystemExit('PVT requires macOS 27.0 or later')
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
