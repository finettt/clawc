"""End-to-end cases with isolated workspaces and ephemeral HTTP/TLS ports."""
import os
import json
import shutil
import socket
import signal
import contextlib
from pathlib import Path
import re
import ssl
import subprocess
import tempfile
import threading
import time
import unittest
from http.server import ThreadingHTTPServer

import mock_server

ROOT = Path(__file__).resolve().parent.parent

class IntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='clawc-py-integration-')
        cls.base = Path(cls.temp.name)
        cls.binary = Path(os.environ.get('CLAWC_TEST_BIN', ROOT / 'clawc')).resolve()
        cls.cert, cls.key = cls.base / 'cert.pem', cls.base / 'key.pem'
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048',
                        '-keyout', str(cls.key), '-out', str(cls.cert), '-days', '2',
                        '-nodes', '-subj', '/CN=localhost', '-addext',
                        'subjectAltName=DNS:localhost,IP:127.0.0.1'],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        cls.old_reqdir, cls.old_port = mock_server.REQDIR, mock_server.SELF_PORT[0]
        cls.reqdir = cls.base / 'requests'
        cls.reqdir.mkdir()
        mock_server.REQDIR = str(cls.reqdir)
        cls.http = ThreadingHTTPServer(('127.0.0.1', 0), mock_server.Handler)
        cls.https = ThreadingHTTPServer(('127.0.0.1', 0), mock_server.Handler)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(cls.cert, cls.key)
        cls.https.socket = ctx.wrap_socket(cls.https.socket, server_side=True)
        cls.api = 'http://127.0.0.1:%d/v1' % cls.http.server_port
        cls.tls_api = 'https://127.0.0.1:%d/v1' % cls.https.server_port
        mock_server.SELF_PORT[0] = cls.http.server_port
        cls.threads = []
        for server in (cls.http, cls.https):
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            cls.threads.append(thread)

    @classmethod
    def tearDownClass(cls):
        for server in (cls.http, cls.https):
            server.shutdown()
            server.server_close()
        for thread in cls.threads:
            thread.join()
        mock_server.REQDIR, mock_server.SELF_PORT[0] = cls.old_reqdir, cls.old_port
        cls.temp.cleanup()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=self.base, prefix='case-')
        self.addCleanup(self.tmp.cleanup)
        self.ws = Path(self.tmp.name) / 'ws'
        self.env = {k: v for k, v in os.environ.items()
                    if not (k.startswith('CLAWC_') or k in ('OPENAI_API_KEY', 'ANTHROPIC_API_KEY'))}
        self.env['HOME'] = self.tmp.name
        self.env['CLAWC_TELEGRAM_USERS'] = '42,777,888,666,300,400,701,702,703,704,444,999,778'
        self.env['CLAWC_MATRIX_USERS'] = '@alice:localhost,@bob:localhost'
        self.env['CLAWC_DISCORD_USERS'] = 'u1,u2'
        self.env['CLAWC_BRIDGE_TOKEN'] = '0123456789abcdef' * 4
        self.env['CLAWC_SERVE_TLS'] = self.env['CLAWC_A2A_TLS'] = '0'
        self.old_risky = mock_server.RISKY
        mock_server.RISKY = str(Path(self.tmp.name) / 'risky')
        self.addCleanup(setattr, mock_server, 'RISKY', self.old_risky)
        for f in self.reqdir.glob('req-*.json'):
            f.unlink()
        p = subprocess.run([str(self.binary), '-w', str(self.ws), 'init'], cwd=ROOT,
                           env=self.env, capture_output=True, text=True, timeout=60)
        self.assertEqual(p.returncode, 0, p.stderr)
        (self.ws / 'history').mkdir(exist_ok=True)

    def agent(self, *args, api=None, flags=(), env=None, stdin=None):
        command = [str(self.binary), '-q', *flags, '-w', str(self.ws), '-u', api or self.api,
                   '-k', 'testkey', *args]
        return subprocess.run(command, cwd=ROOT, env=env or self.env, input=stdin,
                              capture_output=True, text=True, timeout=60)

    def requests(self):
        return '\n'.join(p.read_text() for p in self.reqdir.glob('req-*.json'))

    def test_workspace_initialized(self):
        self.assertTrue((self.ws / 'SOUL.md').is_file())
        self.assertTrue((self.ws / 'MEMORY.md').is_file())

    def test_init_creates_nested_workspace_without_shell(self):
        ws = Path(self.tmp.name) / "it's" / 'deep' / 'ws'
        p = subprocess.run([str(self.binary), '-w', str(ws), 'init'], cwd=ROOT,
                           env=self.env, capture_output=True, text=True, timeout=60)
        self.assertEqual((p.returncode, p.stderr), (0, ''))
        self.assertTrue((ws / 'SOUL.md').is_file())
        self.assertTrue((ws / 'skills' / 'example' / 'SKILL.md').is_file())

    def test_openai_tool_round_trip(self):
        p = self.agent('run', 'please run the tool')
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertRegex(p.stdout, r'(?s)FINAL:.*hello-from-tool')
        body = self.requests()
        self.assertIn('"tools"', body)
        self.assertIn('"tool_call_id"', body)
        self.assertIn('"stream":true', body)

    def test_openai_stream_preserves_first_delta(self):
        p = self.agent('run', 'stream-test now')
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(p.stdout, 'FINAL: [exit 0]\nhello-from-tool\n')
        self.assertIn('"stream":true', self.requests())

    def test_anthropic_tool_round_trip(self):
        p = self.agent('run', 'skill-test please', flags=('-a', 'anthropic'))
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertRegex(p.stdout, r'(?s)FINAL:.*Skills load from workspace')

    def test_empty_heartbeat_is_suppressed(self):
        p = self.agent('heartbeat')
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(p.stdout, '')

    def test_remember_writes_memory(self):
        p = self.agent('run', 'use-remember now')
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn('pizza is the favorite food', (self.ws / 'MEMORY.md').read_text())

    def test_path_sandbox_and_symlinks(self):
        for link, target in [('link-out', '/home/user'), ('link-in', 'real-target.txt')]:
            (self.ws / link).symlink_to(target)
        (self.ws / 'real-target.txt').write_text('hi\n')
        for prompt in ('use-sandbox-write now', 'use-sandbox-symlink-out now', 'use-sandbox-symlink-in now'):
            with self.subTest(prompt=prompt):
                p = self.agent('run', prompt, flags=('-x', str(self.ws)))
                self.assertEqual(p.returncode, 0, p.stderr)
                self.assertIn('path not allowed', p.stdout)
        self.assertEqual((self.ws / 'real-target.txt').read_text(), 'hi\n')
        self.assertFalse(Path('/home/user/evil').exists())

    def test_fetch_url(self):
        p = self.agent('run', 'use-fetch now')
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn('hello web page', p.stdout)

    def test_https_round_trip_and_certificate_rejection(self):
        p = self.agent('run', 'tls tool', api=self.tls_api, flags=('--insecure',))
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertRegex(p.stdout, r'(?s)FINAL:.*hello-from-tool')
        p = self.agent('run', 'tls tool', api=self.tls_api)
        self.assertRegex(p.stdout, r'TLS|verify|certificate')

    def last_request(self):
        files = sorted(self.reqdir.glob('req-*.json'),
                       key=lambda p: int(p.stem.split('-')[1]))
        self.assertTrue(files, 'no requests captured')
        return files[-1].read_text()

    def clear_requests(self):
        for path in self.reqdir.glob('req-*.json'):
            path.unlink()

    def test_chat_history_restored_and_reset(self):
        p = self.agent('chat', stdin='first turn alpha\n/quit\n')
        self.assertEqual(p.returncode, 0, p.stderr)
        history = self.ws / 'history/chat.jsonl'
        self.assertTrue(history.is_file() and history.stat().st_size)
        self.clear_requests()
        p = self.agent('chat', stdin='second turn beta\n/quit\n')
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn('first turn alpha', self.last_request())
        self.assertIn('second turn beta', self.last_request())
        self.clear_requests()
        p = self.agent('chat', stdin='/reset\ngamma after reset\n/quit\n')
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertNotIn('first turn alpha', self.last_request())

    def test_history_sanitization(self):
        (self.ws / 'history').mkdir(exist_ok=True)
        (self.ws / 'history/chat.jsonl').write_text(
            '{"role":"user","content":"valid earlier message"}\n'
            '{"role":"tool","content":"orphan tool output zzz","tool_call_id":"nobody"}\n'
            '{"role":"assistant","content":"hi there","tool_calls":'
            '[{"id":"c9","name":"run_shell","args":{"command":"echo unprompted"}}]}\n')
        p = self.agent('chat', stdin='new message now\n/quit\n')
        self.assertEqual(p.returncode, 0, p.stderr)
        body = self.last_request()
        self.assertNotIn('orphan tool output zzz', body)
        self.assertNotIn('unprompted', body)
        self.assertIn('valid earlier message', body)

    def test_audit_records_tool_calls(self):
        p = self.agent('run', 'please run the tool')
        self.assertEqual(p.returncode, 0, p.stderr)
        audit = self.ws / 'history/audit.jsonl'
        self.assertTrue(audit.is_file() and audit.stat().st_size)
        self.assertIn('"tool":"run_shell"', audit.read_text())

    def test_telegram_http_and_https(self):
        for tls in (False, True):
            with self.subTest(tls=tls):
                for path in (self.ws / 'history').glob('tg-offset-*'):
                    path.unlink()
                for path in self.reqdir.glob('req-*.json'):
                    path.unlink()
                env = self.env.copy()
                env['CLAWC_TELEGRAM_API'] = (('https://127.0.0.1:%d' % self.https.server_port) if tls else
                                             ('http://127.0.0.1:%d' % self.http.server_port))
                p = self.agent('telegram', '-token', 'TESTTOKEN', '--once', '-poll', '1',
                               flags=('--insecure',) if tls else (), env=env)
                self.assertEqual(p.returncode, 0, p.stderr)
                body = self.requests()
                self.assertIn('"chat_id":42', body)
                if not tls:
                    self.assertIn('"text":"FINAL', body)
                    self.assertRegex(body, r'"message_id":[0-9]*,"text":"FINAL')
                    self.assertNotIn('✓', body)
                    self.assertIn('"offset"', body)

    def telegram(self, token, *, extra=None, timeout=60):
        env = self.env.copy()
        env['CLAWC_TELEGRAM_API'] = 'http://127.0.0.1:%d' % self.http.server_port
        if extra:
            env.update(extra)
        p = self.agent('telegram', '-token', token, '--once', '-poll', '1', env=env)
        self.assertEqual(p.returncode, 0, p.stderr)
        return p

    def test_telegram_fast_stream_final_message(self):
        # The model streams the reply in many fast chunks. Telegram throttles
        # edits (>=TG_STREAM_EDIT_MIN_S apart, max 24), so mid-stream edits drop
        # tokens and the visible message lags the full reply. Finalization must
        # correct that, so the last visible message equals the full reply.
        env = self.env.copy()
        env['CLAWC_TELEGRAM_API'] = 'http://127.0.0.1:%d' % self.http.server_port
        for path in (self.ws / 'history').glob('tg-offset-*'):
            path.unlink()
        for path in self.reqdir.glob('req-*.json'):
            path.unlink()
        p = self.agent('telegram', '-token', 'STREAMTOKEN', '--once', '-poll', '1', env=env)
        self.assertEqual(p.returncode, 0, p.stderr)
        texts = []
        for req in sorted(self.reqdir.glob('req-*.json'),
                          key=lambda q: int(re.match(r'req-(\d+)\.json$', q.name).group(1))):
            body = json.loads(req.read_text())
            if 'text' in body and 'chat_id' in body:
                texts.append(body['text'])
        self.assertTrue(texts, 'no telegram messages sent')
        # Finalization must correct the throttle lag: the last visible message is
        # the complete reply, not the strict prefix a throttled edit left behind.
        self.assertGreater(len(texts), 1, 'expected throttled mid-stream edits')
        self.assertTrue(texts[-1].endswith('hello-from-tool'),
                        'final visible message %r is not the complete reply' % (texts[-1],))
        self.assertIn('[exit 0]', texts[-1])
        # Prove the throttle actually lagged (an earlier edit is a strict prefix
        # of the final text) so the regression is not inert.
        self.assertTrue(any(t != texts[-1] and texts[-1].startswith(t) for t in texts[:-1]),
                        'no throttled edit lags the final reply; regression is inert')

    def reset_offsets(self, pattern='tg-offset-*'):
        for path in (self.ws / 'history').glob(pattern):
            path.unlink()

    def test_telegram_session_offset_and_compaction(self):
        self.telegram('TESTTOKEN')
        offsets = list((self.ws / 'history').glob('tg-offset-*'))
        self.assertTrue(offsets)
        self.assertEqual(offsets[0].read_text().strip(), '101')
        model = [p for p in self.reqdir.glob('req-*.json') if '"messages"' in p.read_text()]
        first = max(model, key=lambda p: int(p.stem.split('-')[1]))
        syslen = len(json.loads(first.read_text())['messages'][0].get('content') or '')
        self.clear_requests()
        self.telegram('TESTTOKEN')
        body = self.requests()
        self.assertIn('"offset":101', body)
        self.assertIn('hello again telegram', body)
        self.assertEqual(offsets[0].read_text().strip(), '201')
        model = [p for p in self.reqdir.glob('req-*.json') if '"messages"' in p.read_text()]
        last = max(model, key=lambda p: int(p.stem.split('-')[1])).read_text()
        self.assertIn('hello telegram', last)
        self.assertIn('hello again telegram', last)
        # Run the separate compaction scenario from a fresh session.
        self.reset_offsets()
        for path in (self.ws / 'history').glob('tg-*.jsonl'):
            path.unlink()
        self.clear_requests()
        self.telegram('TESTTOKEN')
        self.telegram('TESTTOKEN')  # compaction needs at least two earlier turn groups
        offsets = list((self.ws / 'history').glob('tg-offset-*'))
        offsets[0].write_text('101\n')
        self.clear_requests()
        self.telegram('TESTTOKEN', extra={'CLAWC_CONTEXT_CHARS': str(syslen + 200)})
        model = [p for p in self.reqdir.glob('req-*.json') if '"messages"' in p.read_text()]
        last = max(model, key=lambda p: int(p.stem.split('-')[1])).read_text()
        self.assertIn('hello again telegram', last)
        self.assertNotIn('FINAL: hello-from-tool', last)
        self.assertIn('compacted earlier conversation', last)

    def test_telegram_confirmations(self):
        risky = Path(mock_server.RISKY)
        for token, approved, expected in (
            ('CONFIRMTOKEN', True, '"callback_query_id"'),
            ('CONFIRMNOTOKEN', False, None),
            ('CONFIRMTEXTTOKEN', True, None),
            ('CONFIRMSTALETOKEN', False, '"text":"expired"'),
            ('CONFIRMNOMIDTOKEN', False, '"text":"expired"'),
            ('TGOTHERBTN', False, 'cancelled'),
            ('TGOTHERTEXT', False, 'cancelled'),
            ('TGSTALETEXT', False, 'cancelled'),
        ):
            with self.subTest(token=token):
                shutil.rmtree(risky, ignore_errors=True)
                risky.mkdir()
                (risky / 'marker').touch()
                self.reset_offsets()
                self.clear_requests()
                audit = self.ws / 'history/audit.jsonl'
                prior_refusals = audit.read_text().count('refused') if audit.exists() else 0
                self.telegram(token, extra={'CLAWC_CONFIRM_SECS': '3' if 'STALE' in token or 'NOMID' in token else '6'})
                body = self.requests()
                self.assertEqual(not (risky / 'marker').exists(), approved, body)
                if approved:
                    self.assertFalse(risky.exists(), body)
                if expected:
                    self.assertIn(expected, body)
                if token == 'CONFIRMTOKEN':
                    self.assertIn('"callback_data":"conf:yes"', body)
                    self.assertIn('u2705 Approve', body)
                    self.assertIn('rm -rf ' + str(risky), (self.ws / 'history/audit.jsonl').read_text())
                if token in ('CONFIRMNOTOKEN', 'CONFIRMSTALETOKEN', 'CONFIRMNOMIDTOKEN'):
                    self.assertGreater(audit.read_text().count('refused'), prior_refusals)
        shutil.rmtree(risky, ignore_errors=True)
        risky.mkdir()
        (risky / 'marker').touch()
        self.reset_offsets()
        env = self.env.copy()
        env['CLAWC_TELEGRAM_API'] = 'http://127.0.0.1:%d' % self.http.server_port
        p = self.agent('telegram', '-token', 'CONFIRMTOKEN', '--once', '-poll', '1', flags=('--yes',), env=env)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertFalse(risky.exists())

    def test_heartbeat_refuses_risky_tool(self):
        risky = Path(mock_server.RISKY)
        risky.mkdir()
        (risky / 'marker').touch()
        (self.ws / 'HEARTBEAT.md').write_text('- [ ] run the command rm -rf %s\n' % risky)
        p = self.agent('heartbeat')
        self.assertIn('refused', p.stdout)
        self.assertTrue((risky / 'marker').is_file())

    def test_telegram_status_goals_media_and_groups(self):
        self.telegram('STATUSTOKEN')
        self.assertIn('clawc 0', self.requests())
        self.assertNotIn('"messages"', self.requests())
        self.reset_offsets()
        self.clear_requests()
        self.telegram('PHOTOTOKEN')
        self.assertTrue((self.ws / 'media/ph-big.jpg').is_file())
        self.assertIn('data:image/jpeg;base64', self.requests())
        self.assertIn('describe this picture', self.requests())
        self.reset_offsets()
        self.clear_requests()
        self.telegram('GROUPTOKEN')
        self.assertIn('hello group', self.requests())
        self.assertNotIn('ignore me completely', self.requests())
        self.reset_offsets()
        self.clear_requests()
        self.telegram('GOALCMDTOKEN')
        goals = self.ws / 'GOALS.md'
        self.assertIn('(g1) learn the drums', goals.read_text())
        self.clear_requests()
        self.telegram('GOALCMDTOKEN')
        self.assertIn('learn the drums', self.requests())
        self.telegram('GOALCMDTOKEN')
        self.assertIn('[x] (g1) [done', goals.read_text())
        self.telegram('GOALCMDTOKEN')
        self.assertNotIn('learn the drums', goals.read_text())

    def matrix(self, token, *, extra=None, idle=False):
        env = self.env.copy()
        env.update(CLAWC_MATRIX_HOMESERVER='http://127.0.0.1:%d' % self.http.server_port,
                   CLAWC_MATRIX_TOKEN=token)
        if extra:
            env.update(extra)
        if idle:
            cmd = [str(self.binary), '-q', '-w', str(self.ws), '-u', self.api, '-k', 'testkey', 'matrix']
            p = subprocess.Popen(cmd, cwd=ROOT, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                end = time.monotonic() + 5
                while time.monotonic() < end:
                    # Skip clawc's atomic-write temp cursor (util.c writes
                    # "<name>.tmp" then renames it); reading it mid-rename races
                    # and raises FileNotFoundError.
                    if any(path.read_text().startswith('s2')
                           for path in (self.ws / 'history').glob('matrix-sync-*')
                           if not path.name.endswith('.tmp')):
                        break
                    if p.poll() is not None:
                        self.fail('matrix exited before consuming sync')
                    time.sleep(.02)
                else:
                    self.fail('matrix did not advance its sync cursor')
            finally:
                p.terminate()
                try: p.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    p.kill(); p.wait()
        else:
            p = self.agent('matrix', env=env)
            self.assertEqual(p.returncode, 0, p.stderr)

    def test_matrix_roundtrip_rooms_and_authorization(self):
        self.matrix('MXTEST')
        self.assertIn('hello matrix', self.requests())
        self.assertIn('"msgtype":"m.text"', self.requests())
        self.assertIn('FINAL', self.requests())
        cursors = list((self.ws / 'history').glob('matrix-sync-*'))
        self.assertTrue(cursors)
        self.assertTrue(cursors[0].read_text().startswith('s1'))
        self.clear_requests()
        self.matrix('MXTEST', idle=True)
        self.assertNotIn('hello matrix', self.requests())
        self.reset_offsets('matrix-sync-*')
        self.clear_requests()
        self.matrix('MXROOMS', extra={'CLAWC_MATRIX_ROOMS': '!ok:localhost'})
        self.assertIn('hello allowed', self.requests())
        self.assertNotIn('ignore me', self.requests())
        self.reset_offsets('matrix-sync-*')
        self.clear_requests()
        self.matrix('MXAUTH')
        self.assertIn('hello matrix', self.requests())
        self.assertNotIn('attacker matrix', self.requests())
        self.assertNotIn('missing matrix', self.requests())
        env = self.env.copy()
        env.update(CLAWC_MATRIX_HOMESERVER='http://localhost', CLAWC_MATRIX_TOKEN='X')
        out = subprocess.run([str(self.binary), 'config'], env=env, capture_output=True, text=True).stdout
        self.assertRegex(out, '(?s)gateways:.*matrix')

    def test_matrix_confirmation_bound_to_requester(self):
        risky = Path(mock_server.RISKY)
        for token in ('MXOTHER', 'MXSTALE', 'MXYES'):
            with self.subTest(token=token):
                shutil.rmtree(risky, ignore_errors=True)
                risky.mkdir()
                (risky / 'marker').touch()
                self.reset_offsets('matrix-sync-*')
                self.clear_requests()
                self.matrix(token, extra={'CLAWC_CONFIRM_SECS': '6'})
                if token == 'MXYES':
                    self.assertFalse((risky / 'marker').exists())
                else:
                    self.assertTrue((risky / 'marker').exists())
                    self.assertIn('cancelled', self.requests())

    def test_agent_self_improvement_goals_and_subagents(self):
        out = self.agent('run', 'use-many-tools now')
        self.assertEqual(out.returncode, 0, out.stderr)
        skill = self.ws / 'skills/autoskill/SKILL.md'
        self.assertTrue(skill.is_file())
        shutil.rmtree(skill.parent)
        env = self.env.copy(); env['CLAWC_AUTO_LEARN'] = '0'
        self.agent('run', 'use-many-tools now', env=env)
        self.assertFalse(skill.exists())
        self.agent('run', 'use-write-skill now')
        self.assertTrue((self.ws / 'skills/greet/SKILL.md').is_file())
        self.assertIn('refine this skill with write_skill', self.agent('run', 'use-skill-nudge now').stdout)
        self.agent('run', 'use-reflect now')
        lessons = self.ws / 'LESSONS.md'
        self.assertIn('pizza protocol', lessons.read_text())
        self.clear_requests()
        self.agent('run', 'just do it')
        self.assertIn('pizza protocol', self.last_request())
        self.assertIn('greet - greeting procedure', self.last_request())
        self.agent('run', 'use-write-skill-bad now')
        self.assertFalse((self.ws / 'evil').exists())
        self.assertFalse((self.ws / 'skills/../evil').exists())
        self.agent('improve')
        self.assertIn('improve loop works', lessons.read_text())
        goals = self.ws / 'GOALS.md'
        self.agent('run', 'use-goal-add now')
        self.assertIn('(g1) learn the ukulele', goals.read_text())
        self.clear_requests()
        self.agent('run', 'just do it')
        self.assertIn('learn the ukulele', self.last_request())
        self.agent('run', 'use-goal-done now')
        self.assertIn('[x] (g1)', goals.read_text())
        self.assertIn('FINAL', self.agent('autonomous').stdout)
        self.assertIn('made autonomous progress', goals.read_text())
        cfg = Path(self.tmp.name) / 'autonomous.json'
        cfg.write_text('{"autonomous_interval": 600}')
        self.assertIn('autonomous', subprocess.run([str(self.binary), '-c', str(cfg), 'config'], env=self.env, capture_output=True, text=True).stdout)
        self.clear_requests()
        self.assertIn('pong-via-sub', self.agent('run', 'please use-a-subagent now').stdout)
        self.assertNotIn('"echo pong-via-sub"', self.last_request())
        self.assertIn('cannot nest', self.agent('run', 'please use-nested-subagent now').stdout)
        self.assertIn('cannot nest', (self.ws / 'history/audit.jsonl').read_text())
        out = self.agent('run', 'please use-par-subagents now').stdout
        self.assertGreaterEqual(out.count('pong-via-sub'), 2)
        self.assertIn('subagent report [1/2]', out)
        self.assertIn('SOUL.md', self.agent('run', 'use-listdir now').stdout)
        self.agent('run', 'use-bad-args now')
        self.assertIn('malformed JSON arguments', (self.ws / 'history/audit.jsonl').read_text())

    def test_search_session_history(self):
        self.telegram('TESTTOKEN')
        output = self.agent('run', 'use-search-history now').stdout
        self.assertRegex(output, '(?s)jsonl:.*hello telegram')

    def test_cron_jobs(self):
        self.assertIn('added j1', self.agent('cron', 'add', 'say the word mango', '--at', '1000000').stdout)
        self.assertRegex(self.agent('cron', 'list').stdout, '(?s)j1.*one-shot')
        self.assertIn('gateways: cron', self.agent('config').stdout)
        self.assertIn('mango-delivered', self.agent('cron', '--run-due').stdout)
        self.assertIn('spent', self.agent('cron', 'list').stdout)
        self.assertIn('added j2', self.agent('cron', 'add', 'say the word mango', '--at', '1000000', '--every', '3600').stdout)
        self.assertIn('mango-delivered', self.agent('cron', '--run-due').stdout)
        self.assertRegex(self.agent('cron', 'list').stdout, '(?s)j2.*recurring')
        self.agent('cron', 'rm', 'j2')
        self.assertNotIn('j2', self.agent('cron', 'list').stdout)
        for args, every in [(('time test daily', '--daily', '09:00'), 86400),
                            (('time test weekly', '--weekly', 'mon', '09:00'), 604800),
                            (('time test once', '--at', '23:59'), 0)]:
            self.agent('cron', 'add', *args)
            text = (self.ws / 'cron.json').read_text()
            self.assertRegex(text, r'"task": "%s", "at": [0-9]+, "every": %d' % (args[0], every))
        self.assertEqual(self.agent('cron', 'add', 'x', '--daily', '99:99').returncode, 2)
        self.agent('cron', 'add', 'future', '--at', '2208988800')
        self.assertIn('2208988800', (self.ws / 'cron.json').read_text())

    def test_doctor(self):
        out = self.agent('doctor').stdout
        self.assertRegex(out, r'(?s)\[llm\]       ok.*doctor: all good\.')
        p = subprocess.run([str(self.binary), '-q', '-w', str(Path(self.tmp.name) / 'none'),
                            '-u', self.api, '-k', 'testkey', 'doctor'], env=self.env, capture_output=True, text=True, timeout=60)
        self.assertIn('[workspace] FAIL', p.stdout)

    def test_telegram_authorization(self):
        env = {'CLAWC_TELEGRAM_API': 'http://127.0.0.1:%d' % self.http.server_port}
        e = self.env.copy(); e.update(env)
        p = self.agent('telegram', '-token', 'TGAUTH', '-allow', '777', '--once', '-poll', '1', env=e)
        self.assertEqual(p.returncode, 0, p.stderr)
        body = self.requests()
        self.assertIn('clawc agent online', body)
        self.assertNotIn('unauthorized-image', body)
        self.assertNotIn('wrong-chat-image', body)
        self.assertNotIn('missing sender attack', body)
        goals = self.ws / 'GOALS.md'
        self.assertTrue(not goals.exists() or 'attacker goal' not in goals.read_text())

    def test_missing_or_invalid_sender_configuration(self):
        for channel, extra in [
            ('telegram', {'CLAWC_TELEGRAM_TOKEN': 'TGAUTH'}),
            ('matrix', {'CLAWC_MATRIX_HOMESERVER': 'http://localhost', 'CLAWC_MATRIX_TOKEN': 'MXAUTH'}),
            ('discord', {'CLAWC_DISCORD_TOKEN': 'DCAUTH', 'CLAWC_DISCORD_GATEWAY': 'ws://127.0.0.1:1', 'CLAWC_DISCORD_API': 'http://localhost'}),
        ]:
            with self.subTest(channel=channel):
                env = self.env.copy(); env.update(extra); env.pop('CLAWC_' + channel.upper() + '_USERS')
                p = self.agent(channel, env=env)
                self.assertEqual(p.returncode, 2, p.stdout + p.stderr)
                self.assertIn(channel + '_users required', p.stdout + p.stderr)
        env = self.env.copy(); env.pop('CLAWC_TELEGRAM_USERS'); env['CLAWC_TELEGRAM_TOKEN'] = 'TGAUTH'
        p = self.agent('daemon', env=env)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn('telegram_users required', p.stdout + p.stderr)
        cfg = Path(self.tmp.name) / 'senders.json'
        cfg.write_text(json.dumps({'telegram_users':'777,778', 'matrix_users':'@alice:localhost,@bob:localhost', 'discord_users':'u1,u2'}))
        env = self.env.copy()
        for name in ('TELEGRAM', 'MATRIX', 'DISCORD'):
            env.pop('CLAWC_' + name + '_USERS')
        p = subprocess.run([str(self.binary), '-c', str(cfg), 'config'], env=env, capture_output=True, text=True)
        for text in ('telegram_users: 777,778 (file)', 'matrix_users: @alice:localhost,@bob:localhost (file)', 'discord_users: u1,u2 (file)'):
            self.assertIn(text, p.stdout)
        cfg.write_text('{"discord_users":123456789012345678}')
        p = subprocess.run([str(self.binary), '-c', str(cfg), 'config'], env=env, capture_output=True, text=True)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn('discord_users must be a string', p.stdout + p.stderr)
        env = self.env.copy(); env.update(CLAWC_TELEGRAM_USERS=' , , ', CLAWC_TELEGRAM_TOKEN='TGAUTH')
        p = self.agent('telegram', env=env)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn('telegram_users required', p.stdout + p.stderr)

    def cli(self, *args, env=None, stdin=None, timeout=60):
        return subprocess.run([str(self.binary), *map(str, args)], cwd=ROOT,
                              env=env or self.env, input=stdin, capture_output=True,
                              text=True, timeout=timeout)

    def test_config_precedence_and_daemon_plan(self):
        cfg = Path(self.tmp.name) / 'base.json'
        cfg.write_text(json.dumps({'_note': 'fixture', 'workspace': str(self.ws),
            'model': 'filemodel', 'api_url': self.api, 'api_key': 'filekey', 'serve_port': 12345,
            'heartbeat_interval': 2, 'auto_approve': True, 'telegram_token': 'DAEMONTOKEN',
            'telegram_api': 'http://127.0.0.1:%d' % self.http.server_port, 'telegram_poll': 1,
            'telegram_allow': '42', 'timeout': 30}))
        p = self.cli('-c', cfg, 'config')
        for text in ('model: filemodel (file)', 'gateways: telegram serve heartbeat', 'auto_approve: on (file)'):
            self.assertIn(text, p.stdout)
        legacy = Path(self.tmp.name) / 'legacy.conf'
        legacy.write_text('workspace = %s\nmodel = legacymodel\n' % self.ws)
        self.assertEqual(self.cli('-c', legacy, 'config').returncode, 2)
        env = self.env.copy(); env['CLAWC_MODEL'] = 'envmodel'
        self.assertIn('model: envmodel (env)', self.cli('-c', cfg, 'config', env=env).stdout)
        self.assertIn('model: flagmodel (flag)', self.cli('-c', cfg, '-m', 'flagmodel', 'config', env=env).stdout)
        env = self.env.copy(); env['CLAWC_CONFIG'] = str(cfg)
        self.assertIn('model: filemodel (file)', self.cli('config', env=env).stdout)
        cfg.write_text('{"serve_bind": "0.0.0.0", "context_chars": 400}')
        out = self.cli('-c', cfg, 'config').stdout
        self.assertIn('serve_bind: 0.0.0.0 (file)', out)
        self.assertIn('context_chars: 400 (file)', out)
        self.assertEqual(self.cli('-c', Path(self.tmp.name) / 'missing.json', 'config').returncode, 2)
        cfg.write_text('{"bogus_key": 1}')
        self.assertEqual(self.cli('-c', cfg, 'config').returncode, 2)
        self.assertEqual(self.cli('daemon').returncode, 2)

    def test_setup_wizard(self):
        tapi = 'http://127.0.0.1:%d' % self.http.server_port
        cfg = Path(self.tmp.name) / 'deep/nest/wizard.conf'
        env = self.env.copy(); env.update(CLAWC_TELEGRAM_API=tapi, CLAWC_CONFIG=str(cfg))
        input_text = '\n'.join([str(self.ws), tapi, 'wizardmodel', 'wizkey', 'TESTTOKEN', '42', '42', '0', '0', 'no', ''])
        p = self.cli('setup', env=env, stdin=input_text)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertTrue(cfg.is_file() and cfg.stat().st_size)
        data = cfg.read_text()
        self.assertIn('"telegram_users": "42"', data)
        self.assertTrue(data.startswith('{'))
        self.assertIn('"api_url": "%s"' % self.api, data)
        self.assertIn('verified: @mock_bot', p.stdout + p.stderr)
        self.assertEqual(cfg.stat().st_mode & 0o777, 0o600)
        out = self.cli('-c', cfg, 'config').stdout
        self.assertIn('model: wizardmodel (file)', out)
        self.assertIn('gateways: telegram', out)
        bad = Path(self.tmp.name) / 'bad.json'
        env['CLAWC_CONFIG'] = str(bad)
        input_text = '\n'.join([str(self.ws), self.api, 'wizardmodel', 'wizkey', 'BADTOKEN', '', '42', '0', '0', 'no', ''])
        p = self.cli('setup', env=env, stdin=input_text)
        self.assertNotEqual(p.returncode, 0)
        self.assertFalse(bad.exists())

    def discord(self, token, *, extra=None, idle=False):
        gateway = mock_server.WsGateway(0)
        gateway.start()
        self.addCleanup(gateway.close)
        env = self.env.copy()
        env.update(CLAWC_DISCORD_TOKEN=token,
                   CLAWC_DISCORD_GATEWAY='ws://127.0.0.1:%d' % gateway.port,
                   CLAWC_DISCORD_API='http://127.0.0.1:%d' % self.http.server_port)
        if extra:
            env.update(extra)
        if idle:
            cmd = [str(self.binary), '-q', '-w', str(self.ws), '-u', self.api, '-k', 'testkey', 'discord']
            p = subprocess.Popen(cmd, cwd=ROOT, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                end = time.monotonic() + 5
                while not gateway.clients and time.monotonic() < end:
                    if p.poll() is not None: self.fail('discord exited before gateway connection')
                    time.sleep(.02)
                self.assertTrue(gateway.clients, 'discord did not connect')
                self.assertTrue(gateway.events_sent.wait(5), 'gateway did not send the fixture event')
                time.sleep(.5)  # allow rejected event to be processed
                self.assertIsNone(p.poll(), 'discord unexpectedly exited')
            finally:
                p.terminate()
                try: p.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    p.kill(); p.wait()
        else:
            p = self.agent('discord', env=env)
            self.assertEqual(p.returncode, 0, p.stderr)

    def test_discord_roundtrip_slash_and_media(self):
        self.discord('DCTEST')
        self.assertIn('hello discord', self.requests())
        self.assertIn('"content"', self.requests())
        self.assertIn('FINAL', self.requests())
        env = self.env.copy(); env['CLAWC_DISCORD_TOKEN'] = 'X'
        self.assertRegex(self.cli('config', env=env).stdout, '(?s)gateways:.*discord')
        self.clear_requests()
        self.discord('DCBOT', idle=True)
        self.assertNotIn('hello discord', self.requests())
        self.clear_requests()
        self.discord('DCSLASH')
        self.assertIn('mango-delivered', self.requests())
        self.clear_requests()
        self.reset_offsets('discord-cursor-*')
        self.discord('DCSLASH', extra={'CLAWC_DISCORD_CHANNELS': '999'}, idle=True)
        self.assertNotIn('mango-delivered', self.requests())
        self.clear_requests()
        self.discord('DCMEDIA')
        self.assertTrue((self.ws / 'media/test-pic.png').is_file())
        self.assertIn('mock-file:note.txt', (self.ws / 'media/note.txt').read_text())
        self.assertIn('data:image/png;base64', self.requests())
        self.assertIn('what is this picture', self.requests())
        self.clear_requests()
        self.discord('DCMEDIA2')
        self.assertTrue((self.ws / 'media/solo.bin').is_file())
        self.assertIn('solo.bin', self.requests())

    def test_discord_authorization(self):
        self.discord('DCAUTH')
        body = self.requests()
        self.assertIn('clawc online', body)
        self.assertFalse((self.ws / 'media/evil.png').exists())
        self.assertNotIn('attacker slash', body)
        goals = self.ws / 'GOALS.md'
        if goals.exists():
            for text in ('attacker goal', 'missing goal', 'spoofed guild goal'):
                self.assertNotIn(text, goals.read_text())
        self.clear_requests()
        self.discord('DCGUILD')
        self.assertIn('mango-delivered', self.requests())

    def test_discord_confirmation_bound_to_requester(self):
        risky = Path(mock_server.RISKY)
        for token in ('DCFLOW', 'DCSTALE', 'DCOTHERBTN', 'DCOTHERTEXT', 'DCSTALETEXT', 'DCTEXTYES'):
            with self.subTest(token=token):
                shutil.rmtree(risky, ignore_errors=True)
                risky.mkdir(); (risky / 'marker').touch()
                self.reset_offsets('discord-cursor-*')
                self.clear_requests()
                self.discord(token, extra={'CLAWC_CONFIRM_SECS': '3' if token == 'DCSTALE' else '6'})
                if token in ('DCFLOW', 'DCTEXTYES'):
                    self.assertFalse((risky / 'marker').exists())
                else:
                    self.assertTrue((risky / 'marker').exists())
                if token == 'DCFLOW':
                    self.assertIn('(approved', self.requests())
                elif token == 'DCSTALE':
                    self.assertIn('(expired)', self.requests())
                elif token != 'DCTEXTYES':
                    self.assertIn('cancelled', self.requests())

    def unused_port(self):
        with socket.socket() as s:
            s.bind(('127.0.0.1', 0))
            return s.getsockname()[1]

    def start_bridge(self, *, binary=None, env=None, bind=None, ws=None):
        binary = binary or self.binary
        ws = ws or self.ws
        port = self.unused_port()
        logfile = open(Path(self.tmp.name) / ('bridge-%d.log' % port), 'w+')
        p = subprocess.Popen([str(binary), '-q', '-w', str(ws), '-u', self.api, '-k', 'testkey',
                              'serve', '-p', str(port)] + (['-b', bind] if bind else []), cwd=ROOT,
                             env=env or self.env, stdout=logfile, stderr=subprocess.STDOUT)
        def cleanup():
            if p.poll() is None:
                p.terminate()
                try: p.wait(timeout=5)
                except subprocess.TimeoutExpired: p.kill(); p.wait()
            logfile.close()
        self.addCleanup(cleanup)
        end = time.monotonic() + 10
        while time.monotonic() < end:
            logfile.flush(); logfile.seek(0)
            log = logfile.read()
            if 'listening on ' in log:
                return port, p, logfile
            if p.poll() is not None:
                self.fail('bridge exited during startup: ' + log)
            time.sleep(.03)
        self.fail('bridge failed to start: ' + log)

    def bridge_request(self, port, request, context=None):
        with socket.create_connection(('127.0.0.1', port), timeout=10) as raw:
            sock = context.wrap_socket(raw, server_hostname='localhost') if context else raw
            with contextlib.closing(sock):
                sock.settimeout(10)
                sock.sendall(json.dumps(request).encode() + b'\n')
                b = b''
                while b'\n' not in b:
                    chunk = sock.recv(4096)
                    if not chunk: break
                    b += chunk
                return json.loads(b)

    def test_serve_auth_sessions_and_protocol(self):
        token = self.env['CLAWC_BRIDGE_TOKEN']
        for val in ('', 'short'):
            env = self.env.copy(); env['CLAWC_BRIDGE_TOKEN'] = val
            self.assertNotEqual(self.cli('serve', '-p', self.unused_port(), env=env).returncode, 0)
        port, proc, log = self.start_bridge()
        for identity, text in [('alice', 'alice secret one'), ('bob', 'bob hello there'), ('alice', 'alice again now')]:
            response = self.bridge_request(port, {'from': identity, 'text': text, 'token': token})
            self.assertTrue(response['ok'])
        last = self.last_request()
        self.assertIn('alice secret one', last)
        self.assertIn('alice again now', last)
        self.assertNotIn('bob hello there', last)
        for bad in (None, 'f' * 64, 'short', 123):
            req = {'text': 'auth-negative-test'}
            if bad is not None: req['token'] = bad
            self.assertEqual(self.bridge_request(port, req), {'ok': False, 'error': 'unauthorized'})
        self.assertNotIn('auth-negative-test', self.requests())
        config = self.cli('config').stdout
        self.assertIn('bridge_token: (set)', config)
        self.assertNotIn(token, config)
        cfg = Path(self.tmp.name) / 'bridge.json'; cfg.write_text(json.dumps({'bridge_token':token}))
        env = self.env.copy(); env.pop('CLAWC_BRIDGE_TOKEN')
        self.assertIn('bridge_token: (set) (file)', self.cli('-c', cfg, 'config', env=env).stdout)
        with socket.create_connection(('127.0.0.1', port), timeout=3) as s:
            s.sendall(b'{"text":"partial"}'); s.shutdown(socket.SHUT_WR)
            self.assertEqual(s.recv(1024), b'')
        with socket.create_connection(('127.0.0.1', port), timeout=8) as s:
            started = time.monotonic()
            self.assertEqual(s.recv(1), b'')
            self.assertLess(time.monotonic() - started, 7)
        r = self.bridge_request(port, {'text':'ping via bridge', 'token':token, 'pad':{}, 'nested':{'a':{}, 'b':{}}})
        self.assertTrue(r['ok'])
        self.assertIn('hello-from-tool', r['reply'])
        log.flush(); log.seek(0)
        self.assertIn('listening on 127.0.0.1:%d' % port, log.read())
        p = self.cli('-w', self.ws, '-u', self.api, '-k', 'testkey', 'serve', '-p', self.unused_port(), '-b', '0.0.0.0')
        self.assertNotEqual(p.returncode, 0)

    def test_a2a_and_tls_bridge(self):
        token = self.env['CLAWC_BRIDGE_TOKEN']
        port, _, _ = self.start_bridge()
        env = self.env.copy(); env['CLAWC_A2A_PEERS'] = 'buddy=127.0.0.1:%d' % port
        self.assertIn('mango-delivered', self.agent('run', 'use-a2a now', env=env).stdout)
        wrong = env.copy(); wrong['CLAWC_BRIDGE_TOKEN'] = 'f' * 64
        self.assertIn('peer authentication failed', self.agent('run', 'use-a2a now', env=wrong).stdout)
        self.assertIn('unknown peer', self.agent('run', 'use-a2a-bad now').stdout)
        env = self.env.copy(); env.update(CLAWC_A2A_TLS='0', CLAWC_A2A_PEERS='buddy=192.0.2.1:9')
        self.assertIn('plaintext is loopback-only', self.agent('run', 'use-a2a now', env=env).stdout)
        otherkey = Path(self.tmp.name) / 'other-key.pem'; othercert = Path(self.tmp.name) / 'other-cert.pem'
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '2', '-subj', '/CN=other', '-keyout', str(otherkey), '-out', str(othercert)], check=True, capture_output=True)
        env = self.env.copy(); env.update(CLAWC_SERVE_TLS='1', CLAWC_SERVE_CERT='', CLAWC_SERVE_KEY='')
        self.assertNotEqual(self.cli('serve', '-p', self.unused_port(), env=env).returncode, 0)
        env.update(CLAWC_SERVE_CERT=str(self.cert), CLAWC_SERVE_KEY=str(otherkey))
        self.assertNotEqual(self.cli('serve', '-p', self.unused_port(), env=env).returncode, 0)
        default = self.env.copy(); default.pop('CLAWC_SERVE_TLS')
        cfg = Path(self.tmp.name) / 'tls-default.json'; cfg.write_text('{}')
        self.assertNotEqual(self.cli('-c', cfg, 'serve', '-p', self.unused_port(), env=default).returncode, 0)
        env.update(CLAWC_SERVE_KEY=str(self.key))
        tlsport, _, log = self.start_bridge(env=env, bind='0.0.0.0')
        log.flush(); log.seek(0)
        self.assertIn('listening on 0.0.0.0:%d' % tlsport, log.read())
        ctx = ssl.create_default_context(cafile=str(self.cert))
        response = self.bridge_request(tlsport, {'text':'ping via encrypted bridge', 'token':token}, ctx)
        self.assertTrue(response['ok']); self.assertIn('hello-from-tool', response['reply'])
        self.assertEqual(self.bridge_request(tlsport, {'text':'bad token', 'token':'f'*64}, ctx), {'ok':False, 'error':'unauthorized'})
        for cafile, host in [(self.cert, 'wrong.example'), (othercert, 'localhost')]:
            with self.subTest(cafile=cafile, host=host):
                context = ssl.create_default_context(cafile=str(cafile))
                with self.assertRaises(ssl.SSLCertVerificationError):
                    with socket.create_connection(('127.0.0.1', tlsport), timeout=5) as raw:
                        with context.wrap_socket(raw, server_hostname=host): pass
        with socket.create_connection(('127.0.0.1', tlsport), timeout=5) as plain:
            plain.settimeout(7)
            try:
                plain.sendall(b'{"text":"plaintext-must-fail"}\n')
                self.assertFalse(plain.recv(100))
            except (ConnectionResetError, BrokenPipeError): pass
        for host, ca, bypass, expected in [
            ('127.0.0.1', self.cert, False, 'mango-delivered'),
            ('127.0.0.2', self.cert, False, 'TLS verification or handshake failed'),
            ('127.0.0.1', othercert, False, 'TLS verification or handshake failed'),
            ('127.0.0.1', othercert, True, 'TLS verification or handshake failed')]:
            env = self.env.copy(); env.update(CLAWC_A2A_TLS='1', CLAWC_A2A_CA=str(ca), CLAWC_A2A_PEERS='buddy=%s:%d' % (host, tlsport))
            if bypass: env['CLAWC_INSECURE'] = '1'
            self.assertIn(expected, self.agent('run', 'use-a2a now', env=env).stdout)
        # An unrelated plaintext peer must receive only a TLS ClientHello, not the token.
        listener = socket.socket(); listener.bind(('127.0.0.1', 0)); listener.listen(1); listener.settimeout(10)
        self.addCleanup(listener.close)
        captured = []
        def capture():
            with listener.accept()[0] as conn:
                conn.settimeout(5); captured.append(conn.recv(8192))
        thread = threading.Thread(target=capture, daemon=True); thread.start()
        env = self.env.copy(); env.update(CLAWC_A2A_TLS='1', CLAWC_A2A_CA=str(self.cert), CLAWC_A2A_PEERS='buddy=127.0.0.1:%d' % listener.getsockname()[1])
        out = self.agent('run', 'use-a2a now', env=env).stdout
        thread.join(timeout=10)
        self.assertIn('TLS verification or handshake failed', out)
        self.assertTrue(captured and captured[0])
        self.assertNotIn(token.encode(), captured[0])

    def build_variant(self, name, *args):
        binary = Path(self.tmp.name) / name
        p = subprocess.run(['make', '-s', 'TARGET=' + str(binary), *args], cwd=ROOT,
                           env=self.env, capture_output=True, text=True, timeout=180)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertTrue(binary.is_file() and os.access(binary, os.X_OK))
        return binary

    def test_size_contract_and_variants(self):
        budget = self.build_variant('budget', 'CHANNELS=telegram')
        self.assertLess(budget.stat().st_size, 196608)
        self.assertLess(self.binary.stat().st_size, 196608)
        self.assertIn('telegram', self.cli('help').stdout)
        minimal = self.build_variant('minimal', 'CHANNELS=serve')
        notls = self.build_variant('notls', 'TLS=0')
        def runbinary(binary, *args, env=None):
            return subprocess.run([str(binary), *map(str, args)], cwd=ROOT, env=env or self.env, capture_output=True, text=True, timeout=90)
        self.assertTrue(runbinary(notls, 'version').stdout.startswith('clawc'))
        out = '\n'.join(runbinary(minimal, 'doctor').stdout.splitlines()[:3])
        self.assertRegex(out, r'(?s)\[features\].*serve.*cron.*heartbeat.*\[config\]')
        self.assertIn('hello-from-tool', runbinary(minimal, '-q', '-w', self.ws, '-u', self.api, '-k', 'testkey', 'run', 'just do it').stdout)
        self.assertEqual(runbinary(minimal, 'telegram').returncode, 2)
        self.assertNotIn('Telegram bot', runbinary(minimal).stdout + runbinary(minimal).stderr)
        env = self.env.copy(); env.update(CLAWC_SERVE_TLS='1', CLAWC_SERVE_CERT=str(self.cert), CLAWC_SERVE_KEY=str(self.key))
        self.assertNotEqual(runbinary(notls, 'serve', '-p', self.unused_port(), env=env).returncode, 0)
        port, _, _ = self.start_bridge(binary=notls)
        response = self.bridge_request(port, {'text':'ping via bridge', 'token':self.env['CLAWC_BRIDGE_TOKEN']})
        self.assertTrue(response['ok']); self.assertIn('hello-from-tool', response['reply'])
        env = self.env.copy(); env.update(CLAWC_A2A_TLS='1', CLAWC_A2A_PEERS='buddy=127.0.0.1:%d' % port)
        out = runbinary(notls, '-q', '-w', self.ws, '-u', self.api, '-k', 'testkey', 'run', 'use-a2a now', env=env).stdout
        self.assertIn('peer TLS unavailable in TLS=0 build', out)

    def test_expired_bridge_cert_rejected(self):
        key = Path(self.tmp.name) / 'expired-key.pem'
        csr = Path(self.tmp.name) / 'expired.csr'
        ext = Path(self.tmp.name) / 'expired-ext.cnf'
        cert = Path(self.tmp.name) / 'expired.pem'
        subprocess.run(['openssl', 'req', '-new', '-newkey', 'rsa:2048', '-nodes', '-keyout', str(key), '-out', str(csr), '-subj', '/CN=localhost'], check=True, capture_output=True)
        ext.write_text('subjectAltName=DNS:localhost,IP:127.0.0.1\n')
        # Backdate the certificate so it is already expired. `openssl x509
        # -not_before/-not_after` only exist in OpenSSL >= 3.3, but CI is Ubuntu
        # 24.04 with OpenSSL 3.0.13; `openssl ca -startdate/-enddate` works on
        # both and yields a cert whose notAfter is in the past.
        index = Path(self.tmp.name) / 'expired-index.txt'
        index.write_text('')
        serial = Path(self.tmp.name) / 'expired-serial'
        serial.write_text('01\n')
        newcerts = Path(self.tmp.name) / 'expired-cert'
        newcerts.mkdir()
        cnf = Path(self.tmp.name) / 'expired-ca.cnf'
        cnf.write_text(
            "[ca]\n"
            "default_ca = ca_default\n\n"
            "[ca_default]\n"
            f"certificate = {self.cert}\n"
            f"private_key = {self.key}\n"
            f"database = {index}\n"
            f"serial = {serial}\n"
            f"new_certs_dir = {newcerts}\n"
            "default_md = sha256\n"
            "policy = policy_any\n"
            "unique_subject = no\n\n"
            "[policy_any]\n"
            "commonName = supplied\n"
        )
        subprocess.run(['openssl', 'ca', '-config', str(cnf), '-batch',
                        '-in', str(csr), '-extfile', str(ext),
                        '-startdate', '20190101000000Z', '-enddate', '20200101000000Z',
                        '-notext', '-out', str(cert)], check=True, capture_output=True)
        self.assertGreater(cert.stat().st_size, 0)
        env = self.env.copy(); env.update(CLAWC_SERVE_TLS='1', CLAWC_SERVE_CERT=str(cert), CLAWC_SERVE_KEY=str(key))
        port, _, log = self.start_bridge(env=env)
        log.flush(); log.seek(0)
        self.assertIn('listening on 127.0.0.1:%d' % port, log.read())
        env = self.env.copy(); env.update(CLAWC_A2A_TLS='1', CLAWC_A2A_CA=str(self.cert), CLAWC_A2A_PEERS='buddy=127.0.0.1:%d' % port)
        self.assertIn('TLS verification or handshake failed', self.agent('run', 'use-a2a now', env=env).stdout)

    def test_sandbox_backends_and_jail_fallback(self):
        # /tmp is intentionally writable in both backends: place the external
        # canary directly under the workspace root, with an unpredictable name.
        # Pick a writable directory outside /tmp, which is allowed by the jail.
        candidates = (Path.home(), ROOT.parent, Path('/var/tmp'))
        parent = next((p for p in candidates if p.is_dir() and os.access(p, os.W_OK)
                       and os.path.commonpath((str(p.resolve()), '/tmp')) != '/tmp'), None)
        if parent is None:
            self.skipTest('no writable canary directory outside /tmp')
        canary_dir = Path(tempfile.mkdtemp(prefix='clawc-jail-canary-', dir=parent))
        self.addCleanup(shutil.rmtree, canary_dir, ignore_errors=True)
        old = (mock_server.CANARY, mock_server.RAW_ESCAPE, mock_server.WORKSPACE)
        def restore():
            mock_server.CANARY, mock_server.RAW_ESCAPE, mock_server.WORKSPACE = old
        self.addCleanup(restore)
        mock_server.CANARY = str(canary_dir / 'escape')
        mock_server.WORKSPACE = str(self.ws)
        raw = Path(self.tmp.name) / 'rawsys'
        p = subprocess.run(['cc', '-o', str(raw), 'test/rawsys_escape.c'], cwd=ROOT, capture_output=True, text=True)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertTrue(os.access(raw, os.X_OK))
        mock_server.RAW_ESCAPE = str(raw)
        env = self.env.copy(); env['CLAWC_SANDBOX'] = 'landlock'
        p = self.agent('run', 'use-landlock-escape now', env=env)
        self.assertFalse(Path(mock_server.CANARY).exists(), p.stdout + p.stderr)
        fallback = 'clawc jail active' in p.stderr
        p = self.agent('run', 'use-landlock-write now', env=env)
        self.assertIn('landlock-ok', (self.ws / 'landlock-canary2').read_text())
        cfg = Path(self.tmp.name) / 'landlock.json'; cfg.write_text('{"sandbox":"landlock"}')
        self.assertIn('sandbox: landlock (file)', self.cli('-c', cfg, 'config').stdout)
        if not fallback:
            env['CLAWC_SANDBOX'] = 'os'
            self.agent('run', 'use-landlock-write now', env=env)
            self.assertIn('landlock-ok', (self.ws / 'landlock-canary2').read_text())
        self.assertFalse(Path('/etc/evil').exists())
        stub = self.build_variant('stubs', 'SANDBOX_STUBS=1', 'TLS=0')
        self.assertGreater((Path(str(stub) + '-jail.so')).stat().st_size, 0)
        self.assertGreater((Path(str(stub) + '-jaild')).stat().st_size, 0)
        once = Path(self.tmp.name) / 'jail-once'
        p = subprocess.run(['cc', '-std=c99', '-O1', '-o', str(once), 'test/jail_once.c', 'src/jail.c', 'src/util.c'], cwd=ROOT, capture_output=True, text=True)
        self.assertEqual(p.returncode, 0, p.stderr)
        output = Path(self.tmp.name) / 'once-output'
        env = self.env.copy(); env['CLAWC_JAILD'] = str(stub) + '-jaild'
        p = subprocess.run([str(once), str(output)], cwd=ROOT, env=env, capture_output=True, text=True, timeout=60)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertEqual(output.read_text().splitlines(), ['once'])
        env = self.env.copy(); env['CLAWC_SANDBOX'] = 'os'
        def runstub(prompt, *, e=env):
            return subprocess.run([str(stub), '-q', '-w', str(self.ws), '-u', self.api, '-k', 'testkey', 'run', prompt], cwd=ROOT, env=e, capture_output=True, text=True, timeout=60)
        p = runstub('use-landlock-escape now')
        self.assertIn('clawc jail active', p.stderr)
        self.assertFalse(Path(mock_server.CANARY).exists())
        p = runstub('use-jail-rawsys now')
        self.assertFalse(Path(mock_server.CANARY).exists())
        self.assertIn('blocked ' + mock_server.CANARY, p.stdout)
        runstub('use-landlock-write now')
        self.assertIn('landlock-ok', (self.ws / 'landlock-canary2').read_text())
        self.assertIn('path not allowed', runstub('use-sandbox-write now').stdout)
        self.assertIn('hello-from-tool', runstub('please run the tool', e=self.env).stdout)
        p = subprocess.run([str(stub), 'doctor'], cwd=ROOT, env=self.env, capture_output=True, text=True, timeout=60)
        self.assertIn('no OS sandbox on this platform', p.stdout + p.stderr)
        self.assertFalse(Path('/etc/evil').exists())

    def test_daemon_supervises_only_configured_gateways_and_restarts(self):
        port = self.unused_port()
        cfg = Path(self.tmp.name) / 'daemon.json'
        cfg.write_text(json.dumps({'workspace':str(self.ws), 'model':'filemodel',
            'api_url':self.api, 'api_key':'filekey', 'serve_port':port,
            'heartbeat_interval':2, 'auto_approve':True, 'telegram_token':'DAEMONTOKEN',
            'telegram_api':'http://127.0.0.1:%d' % self.http.server_port,
            'telegram_poll':1, 'telegram_allow':'42', 'timeout':30}))
        (self.ws / 'HEARTBEAT.md').write_text('# Heartbeat checklist\n\n- [ ] idle\n')
        logpath = Path(self.tmp.name) / 'daemon.log'
        logfile = logpath.open('w+')
        proc = subprocess.Popen([str(self.binary), '-q', 'daemon', '-c', str(cfg)], cwd=ROOT,
                                env=self.env, stdout=logfile, stderr=subprocess.STDOUT,
                                start_new_session=True)
        def stop():
            if proc.poll() is None:
                os.killpg(proc.pid, signal.SIGTERM)
                try: proc.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    os.killpg(proc.pid, signal.SIGKILL); proc.wait()
            logfile.close()
        self.addCleanup(stop)
        def wait_for(predicate, what, timeout=12):
            end = time.monotonic() + timeout
            while time.monotonic() < end:
                if proc.poll() is not None:
                    self.fail('daemon exited while waiting for %s: %s' % (what, logpath.read_text()))
                if predicate(): return
                time.sleep(.05)
            self.fail('timed out waiting for %s: %s' % (what, logpath.read_text()))
        wait_for(lambda: all(t in logpath.read_text() for t in ('gateway telegram: started', 'gateway serve: started', 'gateway heartbeat: started', 'HEARTBEAT_OK')) and '"chat_id":42' in self.requests(), 'startup and first heartbeat')
        log = logpath.read_text()
        self.assertIn('supervising 3 gateway(s)', log)
        r = self.bridge_request(port, {'text':'ping via bridge', 'token':self.env['CLAWC_BRIDGE_TOKEN']})
        self.assertTrue(r['ok'])
        pids = dict((name, int(pid)) for name, pid in re.findall(r'gateway (\w+): started \(pid (\d+)\)', log))
        self.assertEqual(set(pids), {'telegram', 'serve', 'heartbeat'})
        os.kill(pids['serve'], signal.SIGKILL)
        wait_for(lambda: 'restart in' in logpath.read_text() and len(re.findall(r'gateway serve: started \(pid \d+\)', logpath.read_text())) >= 2, 'gateway restart')
        newpids = [int(x) for x in re.findall(r'gateway \w+: started \(pid (\d+)\)', logpath.read_text())]
        self.assertGreaterEqual(len(newpids), 4)
        latest = dict((name, int(pid)) for name, pid in re.findall(r'gateway (\w+): started \(pid (\d+)\)', logpath.read_text()))
        self.assertEqual(set(latest), {'telegram', 'serve', 'heartbeat'})
        for pid in latest.values():
            os.kill(pid, 0)
        (self.ws / 'HEARTBEAT.md').write_text('- [ ] run the command rm -rf %s\n' % mock_server.RISKY)
        wait_for(lambda: '"chat_id":42,"text":"FINAL: refused' in self.requests(), 'heartbeat notification', timeout=12)
        proc.terminate(); proc.wait(timeout=10)
        self.assertIn('[daemon] stopped', logpath.read_text())
        for pid in newpids:
            try: os.kill(pid, 0)
            except ProcessLookupError: continue
            self.fail('daemon child %d still exists' % pid)

if __name__ == '__main__':
    unittest.main()
