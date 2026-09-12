import asyncio
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from pvt_remote.protocol import Cipher, new_identity, profile, save_private
from pvt_remote.host import Host

class ProtocolTests(unittest.TestCase):
    def setUp(self):
        self.host = Cipher(new_identity())
        self.remote = Cipher(new_identity('pvtremote', 'control'))

    def test_round_trip_and_replay(self):
        payload = {'op': 'offer', 'sdp': 'private connection parameters'}
        sealed = self.remote.seal(self.host.public, payload)
        self.assertNotIn('private connection parameters', json.dumps(sealed))
        self.assertEqual(self.host.open(self.remote.public, sealed), payload)
        with self.assertRaises(ValueError): self.host.open(self.remote.public, sealed)

    def test_tampering_wrong_peer_and_expiry(self):
        sealed = self.remote.seal(self.host.public, {'op': 'offer'})
        for field, value in [('to', self.remote.public['id']), ('ct', sealed['ct'][:-4] + 'AAAA'), ('ts', 0)]:
            with self.assertRaises(Exception): self.host.open(self.remote.public, dict(sealed, **{field: value}))
        attacker = Cipher(new_identity('pvtremote', 'control'))
        with self.assertRaises(Exception): self.host.open(attacker.public, sealed)
        self.assertEqual(self.host.open(self.remote.public, sealed)['op'], 'offer')

    def test_public_exports_strip_private_and_validate_transport(self):
        public = profile(dict(self.host.public, ed_private='never export this'))
        self.assertNotIn('ed_private', public)
        for url in ['https://host', 'ws://user:pass@host', 'wss://host/#fragment']:
            with self.assertRaises(ValueError): profile(dict(public, endpoints=[url]))
        with self.assertRaises(ValueError): profile(dict(public, signaling_url='ws://public.example'))

    def test_identity_written_owner_only(self):
        with tempfile.TemporaryDirectory() as temp:
            file = Path(temp) / 'identity.json'
            save_private(file, self.host.identity)
            self.assertEqual(json.loads(file.read_text()), self.host.identity)
            # POSIX mode bits do not describe Windows inherited user-profile ACLs.
            if os.name != 'nt':
                self.assertEqual(file.stat().st_mode & 0o777, 0o600)

class HostTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.host = Host(self.directory.name)
        self.control = Cipher(new_identity('pvtremote', 'control'))
        self.other = Cipher(new_identity('pvtremote', 'control'))
        self.display = Cipher(new_identity('pvtremote', 'display'))
        self.host.config['remotes'] = [self.control.public, self.other.public, self.display.public]
        self.host.config['active_control'] = self.control.public['id']
        self.host.emit = lambda message: None

    async def asyncTearDown(self):
        await self.host.disable()
        self.directory.cleanup()

    async def test_disabled_has_no_listeners(self):
        self.assertIsNone(self.host.server)
        self.assertIsNone(self.host.mdns)
        response = await self.host.command(self.control.public, {'id':'1', 'action':'set'})
        self.assertFalse(response['ok'])

    async def test_control_exclusivity_and_display_background(self):
        self.host.enabled = True
        def emit(message):
            if message.get('event') == 'command':
                self.host.pending[message['token']].set_result({'ok':True})
        self.host.emit = emit
        for remote in [self.other, self.display]:
            reply = await self.host.command(remote.public, {'id':'1', 'action':'set'})
            self.assertFalse(reply['ok'])
        reply = await self.host.command(self.control.public, {'id':'2', 'action':'set'})
        self.assertTrue(reply['ok'])
        reply = await self.host.command(self.display.public, {'id':'3', 'action':'background'})
        self.assertTrue(reply['ok'])
        self.host.config['remotes'].remove(self.control.public)
        reply = await self.host.command(self.control.public, {'id':'4', 'action':'set'})
        self.assertFalse(reply['ok'])

    async def test_midi_cannot_select_unknown_or_display_profile(self):
        for remote in ['not-imported', self.display.public['id']]:
            with self.assertRaises(ValueError): self.host.validate_config(dict(self.host.config, active_control=remote))

    async def test_authenticated_websocket_and_replay(self):
        from websockets.asyncio.client import connect
        from websockets.asyncio.server import serve
        self.host.enabled = True
        self.host.server = await serve(self.host.connection, '127.0.0.1', 0)
        port = self.host.server.sockets[0].getsockname()[1]
        async with connect(f'ws://127.0.0.1:{port}') as ws:
            challenge = json.loads(await ws.recv())['challenge']
            hello = self.control.seal(self.host.cipher.public, {'op':'hello', 'challenge':challenge})
            await ws.send(json.dumps(hello))
            self.assertEqual(self.control.open(self.host.cipher.public, json.loads(await ws.recv()))['op'], 'hello')
            await ws.send(json.dumps({'op':'command', 'id':'state', 'action':'state'}))
            self.assertTrue(json.loads(await ws.recv())['ok'])
        async with connect(f'ws://127.0.0.1:{port}') as ws:
            await ws.recv()
            await ws.send(json.dumps(hello))
            with self.assertRaises(Exception): await ws.recv()


class HandoffTests(unittest.IsolatedAsyncioTestCase):
    async def test_handoff_preserves_viewers_and_revocation_closes_them(self):
        with tempfile.TemporaryDirectory() as temporary:
            host = Host(temporary)
            a = new_identity('pvtremote', 'control')['public']
            b = new_identity('pvtremote', 'control')['public']
            display = new_identity('pvtremote', 'display')['public']
            host.config.update(remotes=[a,b,display], active_control=a['id'])
            host.enabled = True
            host.emit = lambda value: None
            class Peer:
                closed = False
                async def close(self): self.closed = True
            peer = Peer()
            host.sessions[display['id']] = {'pc':peer}
            await host.input({'op':'configure','enabled':True,'config':{'active_control':b['id']}})
            self.assertFalse(peer.closed)
            self.assertEqual(host.config['active_control'],b['id'])
            response = await host.command(a, {'id':'late','action':'set'})
            self.assertFalse(response['ok'])
            await host.input({'op':'configure','enabled':True,'config':{'remotes':[a,b]}})
            self.assertTrue(peer.closed)
            self.assertNotIn(display['id'],host.sessions)
            await host.disable()


class AutomaticReachabilityTests(unittest.IsolatedAsyncioTestCase):
    async def test_offline_localhost_and_busy_port(self):
        from websockets.asyncio.server import serve
        with tempfile.TemporaryDirectory() as temp:
            host = Host(temp)
            self.assertTrue(host.config['lan'])
            async with serve(lambda ws: None, '0.0.0.0', host.config['port']):
                with patch.object(host, 'lan_addresses', return_value=[]):
                    await host.enable()
                    try:
                        self.assertEqual(host.config['port'], host.automatic_ports()[1])
                        # An unavailable LAN doesn't remove loopback or change identity.
                        await asyncio.sleep(0)
                        self.assertIsNone(host.mdns)
                        self.assertIn('127.0.0.1', host.public()['endpoints'][0])
                        saved = Host(temp)
                        self.assertEqual(saved.cipher.public, host.cipher.public)
                        self.assertEqual(saved.automatic_ports(), host.automatic_ports())
                    finally:
                        await host.disable()

    async def test_invalid_import_does_not_enable_or_replace_saved_pairing(self):
        with tempfile.TemporaryDirectory() as temp:
            host = Host(temp)
            events = []
            host.emit = events.append
            before = dict(host.config)
            bad = new_identity('pvtremote', 'display')['public']
            bad['ed25519'] = 'bad'
            await host.input(dict(op='configure', enabled=True, config=dict(remotes=[bad])))
            self.assertEqual(events[-1]['event'], 'rejected')
            self.assertEqual(host.config, before)
            self.assertFalse(host.enabled)
            self.assertIsNone(host.server)

    async def test_legacy_pairing_migration_preserves_keys_and_controller(self):
        with tempfile.TemporaryDirectory() as temp:
            host = Host(temp)
            remote = new_identity('pvtremote', 'control')['public']
            host.config.update(lan=False, remotes=[remote], active_control=remote['id'])
            save_private(host.config_path, host.config)
            loaded = Host(temp)
            self.assertTrue(loaded.config['lan'])
            self.assertFalse(loaded.enabled)
            self.assertEqual(loaded.config['remotes'], [remote])
            self.assertEqual(loaded.config['active_control'], remote['id'])
            self.assertEqual(loaded.cipher.public, host.cipher.public)

    async def test_address_change_republishes_stable_identity(self):
        from unittest.mock import AsyncMock
        with tempfile.TemporaryDirectory() as temp:
            host = Host(temp)
            first, second = AsyncMock(), AsyncMock()
            with patch('pvt_remote.host.AsyncZeroconf', side_effect=[first, second]), patch.object(host, 'lan_addresses', return_value=['192.168.1.8']):
                await host.refresh_discovery()
                name = host.service.server
                self.assertEqual(name, host.discovery_name() + '.')
                exported = host.public()['endpoints'][1]
                with patch.object(host, 'lan_addresses', return_value=['192.168.2.20']):
                    await host.refresh_discovery()
                    self.assertEqual(host.service.server, name)
                    self.assertEqual(host.public()['endpoints'][1], exported)
                    self.assertEqual(host.service.parsed_addresses(), ['192.168.2.20'])
                first.async_close.assert_awaited_once()
                second.async_register_service.assert_awaited_once()
                await host.disable()

if __name__ == "__main__":
    unittest.main()
