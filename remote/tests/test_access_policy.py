import asyncio
import json
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch
from websockets.asyncio.client import connect
from aiortc import RTCPeerConnection
from pvt_remote.host import Host
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
        interfaces = [SimpleNamespace(ips=[SimpleNamespace(ip='192.168.3.5',network_prefix=24), SimpleNamespace(ip=('fd12:abcd::1',0,0),network_prefix=64)])]
        with patch('pvt_remote.host.get_adapters', return_value=interfaces):
            self.assertTrue(self.host.address_allowed('192.168.3.254'))
            self.assertFalse(self.host.address_allowed('192.168.4.1'))
            self.assertTrue(self.host.address_allowed('fd12:abcd::a'))
            self.assertFalse(self.host.address_allowed('fd12:abce::a'))
    async def test_rejected_socket_gets_no_challenge(self):
        self.host.config.update(address_scope='custom',custom_networks='10.0.0.0/8',lan=False)
        await self.host.enable()
        async with connect(self.host.public()['endpoints'][0],proxy=None) as ws:
            with self.assertRaises(Exception): await ws.recv()
        self.assertFalse(self.host.socket_peers)
    async def test_pause_is_persistent_and_denies_commands(self):
        remote = Cipher(new_identity('pvtremote','control'))
        self.host.config['remotes'] = [remote.public]
        self.host.config['active_control'] = remote.public['id']
        self.host.config['lan'] = False
        await self.host.enable()
        async with connect(self.host.public()['endpoints'][0],proxy=None) as ws:
            challenge = json.loads(await ws.recv())['challenge']
            await ws.send(compact(remote.seal(self.host.cipher.public,dict(op='hello',challenge=challenge,client={'browser':'test','version':'1'}))).decode())
            await ws.recv()
            await self.host.input(dict(op='pause',remote=remote.public['id'],paused=True))
            result = await self.host.command(remote.public,dict(id='test',action='set'))
            self.assertFalse(result['ok'])
        saved = json.loads(self.host.config_path.read_text())
        self.assertIn(remote.public['id'], saved['paused_remotes'])
        await self.host.input(dict(op='pause',remote=remote.public['id'],paused=False))
        self.assertNotIn(remote.public['id'],self.host.config['paused_remotes'])
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
