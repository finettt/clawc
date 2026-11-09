#!/usr/bin/env python3
"""Bounded bridge workers, session isolation/persistence and TLS smoke."""
import concurrent.futures
import http.server
import json
import os
import pathlib
import socket
import ssl
import subprocess
import tempfile
import threading
import time

BIN = str(pathlib.Path(os.environ.get('CLAWC_TEST_BIN', './clawc')).resolve())
TOKEN = '0123456789abcdef' * 4
mu = threading.Lock()
active = peak = 0
requests = []
entered = threading.Event()

class Model(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass
    def do_POST(self):
        global active, peak
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        text = [m.get('content', '') for m in body['messages'] if m['role'] == 'user'][-1]
        with mu:
            active += 1
            peak = max(peak, active)
            requests.append((text, body))
        if text.startswith('slow'):
            entered.set()
            time.sleep(0.5)
        data = json.dumps({'choices': [{'message': {'role': 'assistant', 'content': 'ok'}}]}).encode()
        try:
            self.send_response(200)
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            with mu:
                active -= 1

def port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]

def request(p, name, text, context=None):
    with socket.create_connection(('127.0.0.1', p), timeout=8) as raw:
        sock = context.wrap_socket(raw, server_hostname='localhost') if context else raw
        sock.settimeout(8)
        obj = {'token': TOKEN, 'text': text}
        if name is not None:
            obj['from'] = name
        sock.sendall(json.dumps(obj).encode() + b'\n')
        b = b''
        while b'\n' not in b:
            piece = sock.recv(4096)
            if not piece:
                break
            b += piece
        return json.loads(b)

model = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Model)
th = threading.Thread(target=model.serve_forever, daemon=True)
th.start()
server = None
try:
    with tempfile.TemporaryDirectory(prefix='clawc-bridge-workers-') as tmp:
        ws = os.path.join(tmp, 'ws')
        subprocess.run([BIN, '-w', ws, 'init'], check=True, stdout=subprocess.DEVNULL)
        p = port()
        env = dict(os.environ, CLAWC_BRIDGE_TOKEN=TOKEN, CLAWC_SERVE_TLS='0', CLAWC_SERVE_WORKERS='2')
        args = [BIN, '-q', '-w', ws, '-u', f'http://127.0.0.1:{model.server_port}/v1', '-k', 'test', 'serve', '-p', str(p)]
        def start():
            global server
            server = subprocess.Popen(args, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            end = time.monotonic() + 5
            while time.monotonic() < end:
                if server.poll() is not None:
                    raise RuntimeError('bridge failed to start')
                try:
                    with socket.create_connection(('127.0.0.1', p), timeout=.1):
                        return
                except OSError:
                    time.sleep(.03)
            raise RuntimeError('bridge startup timeout')
        def stop():
            global server
            server.terminate()
            server.wait(timeout=5)
            server = None
        start()
        # Forty identities; the former in-memory cache had only 32 entries.
        for i in range(40):
            assert request(p, f'id{i}', f'first-{i}')['ok']
        assert request(p, 'id0', 'second-0')['ok']
        with mu:
            body = next(body for text, body in requests if text == 'second-0')
        serial = json.dumps(body)
        assert 'first-0' in serial and 'first-1' not in serial
        stop(); start()
        assert request(p, 'id0', 'after-restart')['ok']
        with mu:
            body = next(body for text, body in requests if text == 'after-restart')
        assert 'first-0' in json.dumps(body) and 'first-1' not in json.dumps(body)
        # Reject identities which formerly collapsed to the same sanitized name.
        for name in ('a/b', 'a b', 'x' * 33, 99):
            assert request(p, name, 'bad')['error'] == 'invalid from'
        assert not request(p, 'ab\0suffix', 'bad')['ok']
        assert request(p, 'ab', 'valid')['ok']
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            with mu:
                peak = 0
            results = list(pool.map(lambda i: request(p, f'parallel{i}', f'slow-{i}'), range(4)))
            assert all(r['ok'] for r in results)
            with mu:
                assert peak == 2, f'peak {peak}, expected exactly 2'
            entered.clear()
            first = pool.submit(request, p, 'same', 'slow-same')
            assert entered.wait(3)
            assert request(p, 'same', 'overlap')['error'] == 'session busy'
            assert first.result()['ok']
        assert request(p, 'same', 'later')['ok']
        # One assistant can have multiple unfinished calls. Sanitization must
        # remove them in one pass while preserving a completed sibling call.
        history = pathlib.Path(ws) / 'history'
        calls = [{'id': f'call{i}', 'name': 'read_file', 'args': {'path': 'x'}} for i in range(3)]
        for session, trailing in [('sanitize-mid', False), ('sanitize-end', True)]:
            messages = [
                {'role': 'user', 'content': 'before'},
                {'role': 'assistant', 'content': '', 'tool_calls': calls},
                {'role': 'tool', 'content': 'kept', 'tool_call_id': 'call1'},
            ]
            if not trailing:
                messages.append({'role': 'user', 'content': 'next turn'})
            target = history / f'srv-{session}.jsonl'
            target.write_text(''.join(json.dumps(m) + '\n' for m in messages))
            assert request(p, session, f'check-{session}')['ok']
            saved_messages = [json.loads(line) for line in target.read_text().splitlines()]
            assistant = next(m for m in saved_messages if m.get('tool_calls'))
            assert [c['id'] for c in assistant['tool_calls']] == ['call1']
            assert any(m.get('tool_call_id') == 'call1' for m in saved_messages)
        # Corrupt and non-regular histories must fail without replacement.
        corrupt = history / 'srv-corrupt.jsonl'
        corrupt.write_text('not JSON\n')
        assert request(p, 'corrupt', 'no model call')['error'] == 'session storage unavailable'
        assert corrupt.read_text() == 'not JSON\n'
        (history / 'srv-directory.jsonl').mkdir()
        assert request(p, 'directory', 'no model call')['error'] == 'session storage unavailable'
        os.mkfifo(history / 'srv-pipe.jsonl')
        assert request(p, 'pipe', 'no model call')['error'] == 'session storage unavailable'
        (history / 'srv-link.jsonl').symlink_to(corrupt)
        assert request(p, 'link', 'no model call')['error'] == 'session storage unavailable'
        (history / 'srv-nul.jsonl').write_bytes(b'{"role":"user"}\n\0')
        assert request(p, 'nul', 'no model call')['error'] == 'session storage unavailable'
        # A server shutdown mid-model-call must leave the last committed file intact.
        assert request(p, 'crash', 'committed')['ok']
        saved = (history / 'srv-crash.jsonl').read_bytes()
        entered.clear()
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            pending = pool.submit(request, p, 'crash', 'slow-uncommitted')
            assert entered.wait(3)
            assert (history / 'srv-crash.jsonl').read_bytes() == saved
            stop()
            try:
                pending.result(timeout=5)
            except (OSError, ValueError):
                pass
        assert (history / 'srv-crash.jsonl').read_bytes() == saved
        start()
        assert request(p, 'crash', 'after-kill')['ok']
        with mu:
            body = next(body for text, body in requests if text == 'after-kill')
        assert 'committed' in json.dumps(body) and 'slow-uncommitted' not in json.dumps(body)
        stop()
        # Inject a real commit failure, then verify the previous history survives.
        # Native LD_PRELOAD injection is intentionally skipped for QEMU guests.
        if 'CLAWC_TEST_BIN' not in os.environ:
            source = pathlib.Path(tmp) / 'fail_fsync.c'
            so = pathlib.Path(tmp) / 'fail_fsync.so'
            source.write_text('#include <errno.h>\nint fsync(int fd) { (void)fd; errno=ENOSPC; return -1; }\n')
            subprocess.run(['cc', '-shared', '-fPIC', str(source), '-o', str(so)], check=True)
            saved = (history / 'srv-crash.jsonl').read_bytes()
            env['LD_PRELOAD'] = str(so)
            start()
            assert request(p, 'crash', 'uncommitted-on-fsync')['error'] == 'session storage unavailable'
            stop()
            assert (history / 'srv-crash.jsonl').read_bytes() == saved
            del env['LD_PRELOAD']
            start()
            assert request(p, 'crash', 'after-fsync-failure')['ok']
            stop()
        # TLS uses the same bounded worker and session implementation.
        cert, key = os.path.join(tmp, 'cert.pem'), os.path.join(tmp, 'key.pem')
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                        '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost',
                        '-keyout', key, '-out', cert], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        env.update(CLAWC_SERVE_TLS='1', CLAWC_SERVE_CERT=cert, CLAWC_SERVE_KEY=key)
        start()
        context = ssl.create_default_context(cafile=cert)
        assert request(p, 'id0', 'tls-after-restart', context)['ok']
        with mu:
            body = next(body for text, body in requests if text == 'tls-after-restart')
        assert 'first-0' in json.dumps(body)
        stop()
        print('ok: bounded workers, isolation, restart, busy handling, invalid IDs, storage failure, interrupted request, and TLS')
finally:
    if server:
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill(); server.wait()
    model.shutdown(); model.server_close(); th.join()
