"""Keep the packaged worker's local smoke test independent of build proxies."""
import asyncio
import os
import unittest
from unittest.mock import patch

from pvt_remote.host import self_test


class SelfTestTests(unittest.IsolatedAsyncioTestCase):
    async def test_self_test_bypasses_build_proxy(self):
        requests = []

        async def reject_proxy(reader, writer):
            try:
                requests.append(await reader.readuntil(b'\r\n\r\n'))
                writer.write(b'HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n')
                await writer.drain()
            finally:
                writer.close()
                await writer.wait_closed()

        async with await asyncio.start_server(reject_proxy, '127.0.0.1', 0) as proxy:
            address = f'http://127.0.0.1:{proxy.sockets[0].getsockname()[1]}'
            environment = {
                key: address
                for name in ('http_proxy', 'https_proxy', 'all_proxy', 'ws_proxy', 'wss_proxy')
                for key in (name, name.upper())
            }
            # Launchpad supplies proxies without a localhost bypass. Override
            # any developer-machine exclusions so the regression is exercised.
            environment.update(no_proxy='', NO_PROXY='')
            with patch.dict(os.environ, environment):
                await asyncio.wait_for(self_test(), timeout=30)
            self.assertEqual(requests, [], 'The local self-test contacted a proxy')


if __name__ == '__main__':
    unittest.main()
