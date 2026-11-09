"""Optional ASan/UBSan smoke tests, with self-contained TLS and mock fixtures."""
import os
from pathlib import Path
import re
import shlex
import ssl
import subprocess
import tempfile
import threading
import unittest
from http.server import ThreadingHTTPServer

import mock_server

ROOT = Path(__file__).resolve().parent.parent
DIAGNOSTIC = re.compile(r'ERROR: AddressSanitizer|SUMMARY: (?:AddressSanitizer|UndefinedBehaviorSanitizer)|runtime error:')

class SanitizerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='clawc-py-asan-')
        cls.base = Path(cls.tmp.name)
        cls.binary = cls.base / 'clawc-asan'
        cls.workspace = cls.base / 'workspace'
        cc = shlex.split(os.environ.get('CC', 'cc'))
        subprocess.run(cc + ['-std=c99', '-O1', '-g', '-fsanitize=address,undefined',
                             '-DCLAWC_TLS', '-o', str(cls.binary)] +
                       list(map(str, sorted((ROOT / 'src').glob('*.c')))) + ['-lssl', '-lcrypto'],
                       cwd=ROOT, check=True, capture_output=True)
        key, cert = cls.base / 'key.pem', cls.base / 'cert.pem'
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-keyout', str(key),
                        '-out', str(cert), '-days', '2', '-nodes', '-subj', '/CN=localhost',
                        '-addext', 'subjectAltName=DNS:localhost,IP:127.0.0.1'],
                       check=True, capture_output=True)
        cls.old_reqdir, cls.old_port = mock_server.REQDIR, mock_server.SELF_PORT[0]
        mock_server.REQDIR = str(cls.base / 'requests')
        Path(mock_server.REQDIR).mkdir()
        cls.http = ThreadingHTTPServer(('127.0.0.1', 0), mock_server.Handler)
        cls.https = ThreadingHTTPServer(('127.0.0.1', 0), mock_server.Handler)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(cert, key)
        cls.https.socket = ctx.wrap_socket(cls.https.socket, server_side=True)
        mock_server.SELF_PORT[0] = cls.http.server_port
        cls.threads = []
        for server in (cls.http, cls.https):
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            cls.threads.append(thread)
        cls.env = os.environ.copy()
        cls.env.update(CLAWC_TELEGRAM_USERS='42', ASAN_OPTIONS='detect_leaks=0')
        p = subprocess.run([str(cls.binary), '-w', str(cls.workspace), 'init'],
                           env=cls.env, capture_output=True, text=True, timeout=60)
        if p.returncode != 0 or DIAGNOSTIC.search(p.stdout + p.stderr):
            # unittest does not call tearDownClass after setUpClass errors.
            cls.tearDownClass()
            raise RuntimeError('sanitized init failed: ' + p.stdout + p.stderr)

    @classmethod
    def tearDownClass(cls):
        for server in (cls.http, cls.https):
            server.shutdown()
            server.server_close()
        for thread in cls.threads:
            thread.join()
        mock_server.REQDIR, mock_server.SELF_PORT[0] = cls.old_reqdir, cls.old_port
        cls.tmp.cleanup()

    def check_run(self, protocol, args, *, telegram=False, stdin=None):
        port = self.http.server_port if protocol == 'http' else self.https.server_port
        address = '%s://127.0.0.1:%d' % (protocol, port)
        env = self.env.copy()
        if telegram:
            env['CLAWC_TELEGRAM_API'] = address
            for path in (self.workspace / 'history').glob('tg-offset-*'):
                path.unlink()
        command = [str(self.binary), '-q', '--insecure', '-w', str(self.workspace),
                   '-u', address + '/v1', '-k', 't', *args]
        p = subprocess.run(command, cwd=ROOT, env=env, input=stdin, capture_output=True,
                           text=True, timeout=60)
        output = p.stdout + p.stderr
        self.assertEqual(p.returncode, 0, output)
        self.assertIsNone(DIAGNOSTIC.search(output), output)

    def test_telegram_http(self):
        self.check_run('http', ['telegram', '-token', 'TESTTOKEN', '--once', '-poll', '1'], telegram=True)

    def test_https_fetch(self):
        self.check_run('https', ['run', 'use-fetch now'])

    def test_telegram_https(self):
        self.check_run('https', ['telegram', '-token', 'TESTTOKEN', '--once', '-poll', '1'], telegram=True)

    def test_chat_persistence(self):
        self.check_run('http', ['chat'], stdin='persist me please\n/quit\n')

if __name__ == '__main__':
    unittest.main()
