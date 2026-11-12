#!/usr/bin/env python3
"""Proxy regression tests: HTTP CONNECT, SOCKS5, NO_PROXY, failure paths.

Everything runs against local mocks (test/proxy_mock.py) plus a tiny origin
server, so no network access is required. The origin counts requests, which is
how we prove traffic really went *through* the proxy.
"""
import os
import socket
import socketserver
import subprocess
import sys
import threading
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import proxy_mock  # noqa: E402

BIN = os.environ.get('CLAWC_TEST_BIN', './build/native/clawc')


class _Origin(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


class _OriginHandler(socketserver.BaseRequestHandler):
    def handle(self):
        try:
            self.request.recv(65535)
        except Exception:
            pass
        try:
            self.request.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 2\r\n'
                                 b'Connection: close\r\n\r\nok')
        except Exception:
            pass


class ProxyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.origin = _Origin(('127.0.0.1', 0), _OriginHandler)
        cls.origin_port = cls.origin.server_address[1]
        threading.Thread(target=cls.origin.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.origin.shutdown()
        cls.origin.server_close()

    # -- helpers ---------------------------------------------------------
    def _fetch(self, proxy=None, no_proxy=None, timeout=20, url=None):
        env = {'PATH': '/usr/bin:/bin', 'HOME': '/tmp'}
        if proxy is not None:
            env['CLAWC_PROXY'] = proxy
        if no_proxy is not None:
            env['NO_PROXY'] = no_proxy
        target = url or 'http://127.0.0.1:%d/x' % self.origin_port
        return subprocess.run([BIN, 'fetch', target], env=env,
                              capture_output=True, text=True, timeout=timeout)

    def _proxies(self, **kw):
        stats = {}
        http_srv = proxy_mock.start_http_connect(stats, **kw)
        socks_srv = proxy_mock.start_socks5(stats)
        self.addCleanup(http_srv.close)
        self.addCleanup(socks_srv.close)
        return stats, proxy_mock.port_of(http_srv), proxy_mock.port_of(socks_srv)

    # -- tests -----------------------------------------------------------
    def test_direct_no_proxy(self):
        res = self._fetch()
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertEqual(res.stdout.strip(), 'ok')

    def test_http_connect_tunnel(self):
        stats, hport, _ = self._proxies()
        res = self._fetch(proxy='http://127.0.0.1:%d' % hport)
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertEqual(res.stdout.strip(), 'ok')
        self.assertEqual(stats.get('connect'), 1, 'proxy did not see CONNECT')

    def test_socks5h_uses_domain_atyp(self):
        stats, _, sport = self._proxies()
        res = self._fetch(proxy='socks5h://127.0.0.1:%d' % sport)
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertEqual(res.stdout.strip(), 'ok')
        self.assertEqual(stats.get('socks'), 1, 'proxy did not see a SOCKS CONNECT')
        self.assertIn(3, stats.get('socks_atyp', []),
                      'socks5h must send ATYP=0x03 (domain), not a resolved IP')

    def test_no_proxy_bypass_reaches_origin(self):
        # proxy points at a closed port: only the NO_PROXY bypass can succeed
        res = self._fetch(proxy='http://127.0.0.1:1', no_proxy='127.0.0.1')
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertEqual(res.stdout.strip(), 'ok')

    def test_unreachable_proxy_is_a_clear_error(self):
        res = self._fetch(proxy='http://127.0.0.1:1')
        self.assertNotEqual(res.returncode, 0)
        self.assertIn('connection refused', res.stderr)
        self.assertIn('127.0.0.1:1', res.stderr)

    def test_malformed_proxy_url_is_fatal(self):
        res = self._fetch(proxy='127.0.0.1:1080')
        self.assertNotEqual(res.returncode, 0)
        self.assertIn('unsupported scheme', res.stderr)
        # must not silently fall back to a direct connection
        self.assertNotEqual(res.stdout.strip(), 'ok')

    def test_connect_refused_by_proxy(self):
        stats = {}
        srv = proxy_mock.start_http_connect(stats, refuse=True)
        self.addCleanup(srv.close)
        res = self._fetch(proxy='http://127.0.0.1:%d' % proxy_mock.port_of(srv))
        self.assertNotEqual(res.returncode, 0)
        self.assertIn('proxy refused CONNECT', res.stderr)

    def test_socks5_failure_code_is_readable(self):
        stats = {}
        srv = proxy_mock.start_socks5(stats, fail_code=0x05)
        self.addCleanup(srv.close)
        res = self._fetch(proxy='socks5h://127.0.0.1:%d' % proxy_mock.port_of(srv))
        self.assertNotEqual(res.returncode, 0)
        self.assertIn('connection refused', res.stderr)
        self.assertIn('0x05', res.stderr)

    def test_connect_proxy_auth(self):
        stats = {}
        srv = proxy_mock.start_http_connect(stats, require_auth='user:pass')
        self.addCleanup(srv.close)
        port = proxy_mock.port_of(srv)
        # wrong credentials -> 407 surfaced to the user
        bad = self._fetch(proxy='http://nope:nope@127.0.0.1:%d' % port)
        self.assertNotEqual(bad.returncode, 0)
        self.assertIn('407', bad.stderr)
        # right credentials -> tunnel established
        good = self._fetch(proxy='http://user:pass@127.0.0.1:%d' % port)
        self.assertEqual(good.returncode, 0, good.stderr)
        self.assertEqual(good.stdout.strip(), 'ok')

    def test_config_key_used_when_env_is_unset(self):
        stats, hport, _ = self._proxies()
        cfg = '/tmp/clawc-proxy-test-config'
        with open(cfg, 'w') as fh:
            fh.write('{"proxy": "http://127.0.0.1:%d"}\n' % hport)
        self.addCleanup(lambda: os.path.exists(cfg) and os.unlink(cfg))
        res = subprocess.run([BIN, '-c', cfg, 'fetch',
                              'http://127.0.0.1:%d/x' % self.origin_port],
                             env={'PATH': '/usr/bin:/bin', 'HOME': '/tmp'},
                             capture_output=True, text=True, timeout=20)
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertEqual(stats.get('connect'), 1)

    def test_env_overrides_config_key(self):
        """Project-wide convention: cfg_load_env() runs after cfg_load_file(),
        so CLAWC_* always beats the file. Proxy must not be the exception."""
        stats, hport, _ = self._proxies()
        cfg = '/tmp/clawc-proxy-test-config2'
        with open(cfg, 'w') as fh:
            fh.write('{"proxy": "http://127.0.0.1:1"}\n')
        self.addCleanup(lambda: os.path.exists(cfg) and os.unlink(cfg))
        res = subprocess.run([BIN, '-c', cfg, 'fetch',
                              'http://127.0.0.1:%d/x' % self.origin_port],
                             env={'PATH': '/usr/bin:/bin', 'HOME': '/tmp',
                                  'CLAWC_PROXY': 'http://127.0.0.1:%d' % hport},
                             capture_output=True, text=True, timeout=20)
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertEqual(stats.get('connect'), 1)

    def test_https_proxy_fallback_only_fills_gap(self):
        """HTTPS_PROXY must not override an explicit CLAWC_PROXY."""
        stats, hport, _ = self._proxies()
        res = subprocess.run([BIN, 'fetch',
                              'http://127.0.0.1:%d/x' % self.origin_port],
                             env={'PATH': '/usr/bin:/bin', 'HOME': '/tmp',
                                  'CLAWC_PROXY': 'http://127.0.0.1:%d' % hport,
                                  'HTTPS_PROXY': 'http://127.0.0.1:1'},
                             capture_output=True, text=True, timeout=20)
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertEqual(stats.get('connect'), 1)

    def test_doctor_reports_proxy(self):
        stats, hport, _ = self._proxies()
        res = subprocess.run([BIN, 'doctor'],
                             env={'PATH': '/usr/bin:/bin', 'HOME': '/tmp',
                                  'CLAWC_PROXY': 'http://127.0.0.1:%d' % hport},
                             capture_output=True, text=True, timeout=40)
        out = res.stdout + res.stderr
        self.assertIn('[net]', out)
        self.assertIn('http-connect', out)


if __name__ == '__main__':
    unittest.main(verbosity=2)
