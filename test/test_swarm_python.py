"""Swarm routing and concurrency integration tests with isolated fixtures."""
import http.server
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parent.parent
TOKEN = '0123456789abcdef' * 4

class ModelHandler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        with self.server.lock:
            self.server.active += 1
            self.server.peak = max(self.server.peak, self.server.active)
        try:
            text = [m['content'] for m in body['messages'] if m['role'] == 'user'][-1]
            if self.server.mode == 'load':
                time.sleep(.05)
                result = {'report': 'ok', 'messages': []}
            elif 'You are swarm agent alice.' in text and 'Message from user' in text:
                result = {'report': 'alice received', 'messages': [{'to': '@bob', 'text': 'direct from alice'}]}
            elif 'You are swarm agent bob.' in text and 'direct from alice' in text:
                result = {'report': 'bob received direct', 'messages': [{'to': '#team', 'text': 'group from bob'}]}
            elif 'You are swarm agent alice.' in text and 'group from bob' in text:
                result = {'report': 'alice received group', 'messages': []}
            else:
                result = {'report': 'unexpected', 'messages': []}
            data = json.dumps({'choices': [{'message': {'role': 'assistant', 'content': json.dumps(result)}}]}).encode()
            self.send_response(200)
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        finally:
            with self.server.lock:
                self.server.active -= 1

class ModelServer:
    def __init__(self, mode):
        self.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), ModelHandler)
        self.server.mode = mode
        self.server.lock = threading.Lock()
        self.server.active = self.server.peak = 0
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    @property
    def api(self):
        return 'http://127.0.0.1:%d/v1' % self.server.server_port

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()


def run(args, **kwargs):
    return subprocess.run(list(map(str, args)), cwd=ROOT, check=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True, timeout=90, **kwargs)

class SwarmTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='clawc-py-swarm-')
        cls.base = Path(cls.tmp.name)
        cls.full = Path(os.environ.get('CLAWC_TEST_BIN', ROOT / 'clawc')).resolve()
        cls.budget = cls.base / 'clawc-budget'
        run(['make', '-s'])
        run(['make', '-s', 'CHANNELS=telegram', 'TARGET=' + str(cls.budget)])

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def workspace(self, name):
        ws = self.base / name
        run([self.full, '-w', ws, 'init'])
        return ws

    def test_direct_group_and_remote_for_both_builds(self):
        server = ModelServer('routing')
        self.addCleanup(server.close)
        local = self.workspace('routing-local')
        remote = self.workspace('routing-remote')
        spec = self.base / 'route.json'
        spec.write_text(json.dumps({'workers': 2,
            'agents': [{'id': 'alice', 'role': 'planner'}, {'id': 'bob', 'role': 'reviewer', 'peer': 'buddy'}],
            'groups': [{'id': 'team', 'members': ['alice', 'bob']}],
            'messages': [{'to': '@alice', 'text': 'start'}]}))
        # The C server accepts a port number, not an inherited listening socket.
        # Release the ephemeral port immediately before spawning; a race with
        # unrelated processes is possible, but no fixed shared port is used.
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            port = sock.getsockname()[1]
        env = os.environ.copy()
        env.update(CLAWC_BRIDGE_TOKEN=TOKEN, CLAWC_SERVE_TLS='0', CLAWC_A2A_TLS='0',
                   CLAWC_A2A_PEERS='buddy=127.0.0.1:%d' % port)
        proc = subprocess.Popen([str(self.full), '-w', str(remote), '-u', server.api,
                                 '-k', 'test', 'serve', '-p', str(port)],
                                cwd=ROOT, env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)
        def stop():
            if proc.poll() is None:
                proc.terminate()
                try: proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
            proc.stdout.close()
        self.addCleanup(stop)
        deadline = time.monotonic() + 10
        while True:
            if proc.poll() is not None:
                self.fail('bridge exited: ' + proc.stdout.read())
            try:
                with socket.create_connection(('127.0.0.1', port), timeout=.2):
                    break
            except OSError:
                if time.monotonic() >= deadline:
                    self.fail('bridge did not start')
                time.sleep(.05)
        for binary in (self.full, self.budget):
            with self.subTest(binary=binary.name):
                for pattern, ws in [('swarm-*.jsonl', local), ('srv-*.jsonl', remote)]:
                    for p in (ws / 'history').glob(pattern):
                        p.unlink()
                result = run([binary, '-w', local, '-u', server.api, '-k', 'test',
                              'swarm', spec], env=env)
                output = result.stdout
                self.assertIn('"from":"alice","to":"@bob"', output)
                self.assertIn('"from":"bob","to":"#team"', output)
                self.assertIn('"report":"alice received group"', output)
                warning = "[swarm] bob: peer unavailable in this build, running locally"
                if binary is self.budget:
                    # Budget (CLAWC_NO_SERVE) cannot reach a remote peer, so the
                    # swarm runs the peer member locally instead of over a2a.
                    self.assertIn(warning, result.stderr)
                else:
                    # Full build reaches the remote peer over the bridge.
                    self.assertNotIn(warning, result.stderr)

    def test_100_agents_obey_worker_limit(self):
        server = ModelServer('load')
        self.addCleanup(server.close)
        ws = self.workspace('load')
        spec = self.base / 'load.json'
        spec.write_text(json.dumps({'workers': 8,
            'agents': [{'id': 'a%d' % i, 'role': 'test'} for i in range(100)],
            'messages': [{'to': '@a%d' % i, 'text': 'test'} for i in range(100)]}))
        output = run([self.full, '-w', ws, '-u', server.api, '-k', 'test', 'swarm', spec]).stdout
        self.assertEqual(output.count('"report":"ok"'), 100)
        self.assertGreaterEqual(server.server.peak, 2)
        self.assertLessEqual(server.server.peak, 8)

if __name__ == '__main__':
    unittest.main()
