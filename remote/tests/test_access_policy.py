import asyncio
import json
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch
from websockets.asyncio.client import connect
from aiortc import RTCPeerConnection
from PIL import Image
from pvt_remote.host import Audio, Host, Video
from pvt_remote.protocol import Cipher, new_identity, compact

class AccessPolicyTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.host = Host(self.directory.name)
        self.host.emit = lambda _: None
    async def asyncTearDown(self):
        await self.host.disable()
        self.directory.cleanup()
    def test_private_is_explicit_not_python_is_private(self):
        self.host.config['address_scope'] = 'private'
        for address in ['10.1.2.3', '172.16.0.1', '172.31.255.255', '192.168.4.5', 'fd12::1', 'fe80::abcd', '::1', '::ffff:10.2.3.4']:
            self.assertTrue(self.host.address_allowed(address), address)
        for address in ['172.32.0.1', '8.8.8.8', '100.64.0.1', '192.0.2.1', '2001:db8::1', '::', '224.0.0.1']:
            self.assertFalse(self.host.address_allowed(address), address)
    def test_this_computer_only_is_the_optimized_default(self):
        self.assertEqual(self.host.config['address_scope'], 'local')
        self.assertFalse(self.host.config['lan'])
        self.assertTrue(self.host.address_allowed('127.0.0.1'))
        self.assertTrue(self.host.address_allowed('::1'))
        self.assertFalse(self.host.address_allowed('192.168.1.2'))
        self.assertEqual(len(self.host.public()['endpoints']), 1)
        self.assertTrue(self.host.public()['endpoints'][0].startswith('ws://127.0.0.1:'))
    def test_custom_address_ranges_do_not_ask_for_remote_ports(self):
        self.host.config.update(address_scope='custom', custom_networks='10.1.2.3-10.1.2.8, fd12::/64')
        self.host.validate_config(self.host.config)
        self.assertTrue(self.host.address_allowed('10.1.2.8'))
        self.assertFalse(self.host.address_allowed('10.1.2.9'))
        self.assertFalse(self.host.address_allowed('127.0.0.1'))
        self.assertEqual(self.host.endpoint("fd12::1",5000),"[fd12::1]:5000")
        for text in ['', '10.0.0.9-10.0.0.1', '10.0.0.1-::1', 'host.local']:
            with self.assertRaises((ValueError, TypeError)):
                self.host.validate_config(dict(self.host.config, custom_networks=text))
    def test_old_remote_port_limits_are_removed_on_load(self):
        saved = dict(self.host.config, remote_port_min=5000, remote_port_max=6000)
        self.host.config_path.write_text(json.dumps(saved))
        migrated = Host(self.directory.name)
        self.assertNotIn('remote_port_min', migrated.config)
        self.assertNotIn('remote_port_max', migrated.config)
        migrated.validate_config(saved)
        self.assertNotIn('remote_port_min', saved)
        self.assertNotIn('remote_port_max', saved)
    def test_subnets_use_interface_masks(self):
        self.host.config['address_scope'] = 'subnet'
        interfaces = [SimpleNamespace(ips=[SimpleNamespace(ip='192.168.3.5',network_prefix=24), SimpleNamespace(ip=('fd12:abcd::1',0,0),network_prefix=64)])]
        with patch('pvt_remote.host.get_adapters', return_value=interfaces):
            self.assertTrue(self.host.address_allowed('192.168.3.254'))
            self.assertFalse(self.host.address_allowed('192.168.4.1'))
            self.assertTrue(self.host.address_allowed('fd12:abcd::a'))
            self.assertFalse(self.host.address_allowed('fd12:abce::a'))

    async def test_remote_media_state_is_persistent_and_display_only(self):
        display = new_identity('pvtremote', 'display')['public']
        control = new_identity('pvtremote', 'control')['public']
        self.host.config['remotes'] = [display, control]
        self.host.enabled = True
        reply = await self.host.command(display, dict(
            op='command', id='media', action='remote_media', paused=True,
            pause_mode='freeze', muted=True, audio_enabled=True))
        self.assertTrue(reply['ok'])
        self.assertEqual(reply['remote_media'], dict(
            paused=True, pause_mode='freeze', muted=True, audio_enabled=True))
        self.assertEqual(self.host.offline_rows()[0]['pause_mode'], 'freeze')
        rejected = await self.host.command(control, dict(
            op='command', id='media-control', action='remote_media', muted=True))
        self.assertFalse(rejected['ok'])
        loaded = Host(self.directory.name)
        self.assertEqual(loaded.remote_media(display['id']), reply['remote_media'])

    async def test_paused_tracks_keep_timing_with_freeze_blackout_and_silence(self):
        display = new_identity('pvtremote', 'display')['public']
        self.host.config['remotes'] = [display]
        self.host.image = Image.new('RGB', (16, 8), (20, 80, 140))
        self.host.sequence = 1
        with patch('pvt_remote.host.sys.platform', 'linux'):
            video = Video(self.host, display['id'])
        live = await video.recv()
        self.assertEqual(live.to_image().getpixel((0, 0)), (20, 80, 140))
        display['media'] = dict(paused=True, pause_mode='freeze', muted=False,
                                audio_enabled=True)
        frozen = await asyncio.wait_for(video.recv(), .2)
        self.assertEqual(frozen.to_image().getpixel((0, 0)), (20, 80, 140))
        display['media']['pause_mode'] = 'blackout'
        black = await asyncio.wait_for(video.recv(), .2)
        self.assertEqual(black.to_image().getpixel((0, 0)), (0, 0, 0))
        audio = Audio(self.host, display['id'])
        audio.queue.put_nowait(bytes([127]) * 3840)
        display['media']['muted'] = True
        silent = await audio.recv()
        self.assertFalse(any(bytes(silent.planes[0])))
        self.assertTrue(audio.queue.empty())
        audio.stop(); video.stop()

    async def test_removed_remote_can_be_recovered_with_metadata(self):
        remote = new_identity('pvtremote', 'display')['public']
        remote.update(label='Lobby display', recognized=True,
                      client={'browser': 'Firefox 147', 'platform': 'Linux',
                              'version': '0.2.3', 'features': ['remote-media-v1']},
                      last_endpoint='192.168.1.30:55000')
        self.host.config['remotes'] = [remote]
        self.host.validate_config(self.host.config)
        await self.host.input(dict(op='remove_remote', remote=remote['id']))
        removed = self.host.removed_profiles()
        self.assertEqual(len(removed), 1)
        self.assertEqual(removed[0]['profile']['label'], 'Lobby display')
        self.assertFalse(self.host.config['remotes'])
        await self.host.input(dict(op='recover_remote', key=removed[0]['key']))
        self.assertEqual(self.host.peer(remote['id'])['label'], 'Lobby display')
        self.assertTrue(self.host.peer(remote['id'])['recognized'])
        self.assertFalse(self.host.removed_profiles())

    async def test_first_authenticated_connection_is_announced_once(self):
        remote = Cipher(new_identity('pvtremote', 'control'))
        self.host.config.update(remotes=[remote.public], lan=False)
        events = []
        self.host.emit = events.append
        await self.host.enable()
        for _ in range(2):
            async with connect(self.host.public()['endpoints'][0], proxy=None) as ws:
                challenge = json.loads(await ws.recv())['challenge']
                await ws.send(compact(remote.seal(self.host.cipher.public, dict(
                    op='hello', challenge=challenge,
                    client={'browser': 'Firefox 147', 'platform': 'Linux',
                            'version': '0.2.3'}))).decode())
                await ws.recv()
        notices = [event for event in events if event.get('event') == 'new_remote']
        self.assertEqual(len(notices), 1)
        self.assertEqual(notices[0]['remote']['role'], 'control')
        await self.host.input(dict(op='rename_remote', remote=remote.public['id'],
                                   name='Front-of-house control'))
        self.assertTrue(self.host.peer(remote.public['id'])['recognized'])
        self.assertEqual(self.host.peer(remote.public['id'])['label'],
                         'Front-of-house control')

    async def test_display_disconnect_is_persistent_reachable_standby(self):
        remote = Cipher(new_identity('pvtremote', 'display'))
        remote.public['client'] = {
            'features': ['remote-disconnect-v1', 'remote-reconnect-v1']}
        self.host.config.update(remotes=[remote.public], lan=False)
        events = []
        self.host.emit = events.append
        await self.host.enable()
        async with connect(self.host.public()['endpoints'][0], proxy=None) as ws:
            challenge = json.loads(await ws.recv())['challenge']
            await ws.send(compact(remote.seal(self.host.cipher.public, dict(
                op='hello', challenge=challenge, client={
                    'features': ['remote-disconnect-v1', 'remote-reconnect-v1']}))).decode())
            await ws.recv()
            await ws.send(json.dumps(dict(op='command', id='off',
                                          action='remote_connection', enabled=False)))
            self.assertTrue(json.loads(await ws.recv())['ok'])
            self.assertFalse(self.host.peer(remote.public['id'])['connection_enabled'])
            for _ in range(20):
                row = next((row for event in reversed(events)
                            for row in event.get('connections', [])
                            if row['id'] == remote.public['id']), None)
                if row and row.get('reachable'):
                    break
                await asyncio.sleep(.1)
            self.assertTrue(row['reachable'])
            self.assertFalse(row['connected'])
            self.assertEqual(row['status'], 'Disconnected')
            await self.host.input(dict(op='remote_action', remote=remote.public['id'],
                                       action='reconnect'))
            control = remote.open(self.host.cipher.public, json.loads(await ws.recv()))
            self.assertEqual(control, {'op': 'remote_control', 'action': 'reconnect'})
            self.assertTrue(self.host.peer(remote.public['id']).get('connection_enabled', True))
        self.assertFalse(self.host.offline_rows()[0]['reachable'])
    async def test_rejected_socket_gets_no_challenge(self):
        self.host.config.update(address_scope='custom',custom_networks='10.0.0.0/8',lan=False)
        await self.host.enable()
        async with connect(self.host.public()['endpoints'][0],proxy=None) as ws:
            with self.assertRaises(Exception): await ws.recv()
        self.assertFalse(self.host.socket_peers)
    async def test_legacy_pause_is_removed_and_authenticated_socket_reconnects(self):
        remote = Cipher(new_identity('pvtremote', 'control'))
        self.host.config.update(remotes=[remote.public], paused_remotes=[remote.public['id']], active_control='stale')
        self.host.config_path.write_text(json.dumps(self.host.config))
        self.host = Host(self.directory.name)
        self.host.emit = lambda _: None
        self.host.config['lan'] = False
        await self.host.enable()
        await self.host.input(dict(op='pause', remote=remote.public['id'], paused=True))
        self.assertNotIn('paused_remotes', self.host.config)
        for _ in range(2):
            async with connect(self.host.public()['endpoints'][0], proxy=None) as ws:
                challenge = json.loads(await ws.recv())['challenge']
                await ws.send(compact(remote.seal(self.host.cipher.public, dict(op='hello', challenge=challenge,
                    client={'browser': 'Chrome 145', 'platform': 'macOS', 'version': '1'}))).decode())
                await ws.recv()
                await ws.send(json.dumps(dict(op='command', id='state', action='state')))
                self.assertTrue(json.loads(await ws.recv())['ok'])
        saved = json.loads(self.host.config_path.read_text())
        self.assertNotIn('paused_remotes', saved)
        self.assertNotIn('active_control', saved)
        self.assertEqual(saved['remotes'][0]['client']['browser'], 'Chrome 145')
        self.assertIn('127.0.0.1:', saved['remotes'][0]['last_endpoint'])

    async def test_old_extension_browser_metadata_comes_from_handshake(self):
        remote = Cipher(new_identity('pvtremote', 'control'))
        self.host.config.update(remotes=[remote.public], lan=False)
        await self.host.enable()
        agent = 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) Chrome/145.0.0.0 Safari/537.36'
        async with connect(self.host.public()['endpoints'][0], proxy=None, user_agent_header=agent) as ws:
            challenge = json.loads(await ws.recv())['challenge']
            await ws.send(compact(remote.seal(self.host.cipher.public, dict(op='hello', challenge=challenge))).decode())
            await ws.recv()
            await asyncio.sleep(0)
            self.assertEqual(self.host.peer(remote.public['id'])['client']['browser'], 'Chrome 145.0.0.0')
            self.assertEqual(self.host.peer(remote.public['id'])['client']['platform'], 'macOS')

    async def test_real_ice_guard_blocks_peer_reflexive_request(self):
        pc = RTCPeerConnection(); pc.createDataChannel('test')
        self.host.config.update(address_scope='custom',custom_networks='127.0.0.1')
        connection = pc.sctp.transport.transport._connection
        calls = []
        connection.request_received = lambda *args: calls.append(args)
        self.host.guard_ice(pc)
        connection.request_received(None,('10.9.8.7',1234),None,b'')
        self.assertFalse(calls)
        connection.request_received(None,('127.0.0.1',1234),None,b'')
        self.assertEqual(len(calls),1)
        await pc.close()
    async def test_offer_filter_and_actual_media_connection(self):
        client = RTCPeerConnection(); client.createDataChannel('pvt-control')
        remote = new_identity('pvtremote','control')['public']
        self.host.config.update(remotes=[remote], active_control=remote['id'], address_scope='any', lan=False)
        await self.host.enable()
        await client.setLocalDescription(await client.createOffer())
        sdp = client.localDescription.sdp
        self.host.config.update(address_scope='custom',custom_networks='192.0.2.0/24')
        with self.assertRaises(ValueError): self.host.allowed_offer(sdp,None)
        self.host.config.update(address_scope='subnet')
        async def answer(value):
            from aiortc import RTCSessionDescription
            await client.setRemoteDescription(RTCSessionDescription(sdp=value['sdp'],type='answer'))
        import uuid
        await self.host.offer(remote,dict(session=str(uuid.uuid4()),sdp=sdp),answer,'127.0.0.1')
        for _ in range(100):
            if client.connectionState == 'connected': break
            await asyncio.sleep(.05)
        self.assertEqual(client.connectionState,'connected')
        events=[]; self.host.emit=events.append
        for _ in range(40):
            if any(e.get('connections') and e['connections'][0].get('status') == 'connected' for e in events): break
            await asyncio.sleep(.05)
        row=next(e['connections'][0] for e in events if e.get('connections') and e['connections'][0].get('status') == 'connected')
        self.assertGreater(row['bytes_sent'],0)
        self.assertGreater(row['bytes_received'],0)
        self.assertTrue(row['media_endpoints'])
        await client.close()
