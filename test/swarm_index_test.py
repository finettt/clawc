#!/usr/bin/env python3
"""Exercise indexed swarm lookup, duplicate rejection and prompt ordering."""
import http.server
import json
import pathlib
import subprocess
import sys
import tempfile
import threading

binary = str(pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else './clawc').resolve())
requests = []

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        requests.append(body)
        content = json.dumps({'report': 'ok', 'messages': []})
        response = json.dumps({'choices': [{'message': {'role': 'assistant', 'content': content}}]}).encode()
        self.send_response(200)
        self.send_header('Content-Length', str(len(response)))
        self.end_headers()
        self.wfile.write(response)

server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
try:
    with tempfile.TemporaryDirectory(prefix='clawc-index-') as td:
        root = pathlib.Path(td)
        workspace = root / 'workspace'
        base = [binary, '-q', '-w', str(workspace), '-u', f'http://127.0.0.1:{server.server_port}/v1', '-k', 'test']
        subprocess.run(base + ['init'], check=True, stdout=subprocess.DEVNULL)
        specpath = root / 'spec.json'
        def run(spec):
            specpath.write_text(json.dumps(spec))
            result = subprocess.run(base + ['swarm', str(specpath)], text=True, capture_output=True, timeout=60)
            assert result.returncode >= 0, result.stderr
            assert 'AddressSanitizer' not in result.stderr and 'runtime error:' not in result.stderr, result.stderr
            return result

        agents = [{'id': x, 'role': 'test'} for x in ('z', 'a', 'k', 'b')]
        spec = {'workers': 2, 'agents': agents,
                'groups': [{'id': 'team', 'members': ['z', 'a']}],
                'messages': [{'to': '@a', 'text': 'hello'}, {'to': '#team', 'text': 'group'}]}
        result = run(spec)
        assert result.returncode == 0, result.stderr
        lines = [json.loads(line) for line in result.stdout.splitlines()]
        assert sum(x.get('report') == 'ok' for x in lines) == 3
        assert len(requests) == 3
        for request in requests:
            prompt = next(m['content'] for m in request['messages'] if m['role'] == 'user')
            assert 'Available agents: @z @a @k @b #team\n' in prompt

        for bad in (
            {'agents': agents + [{'id': 'a', 'role': 'duplicate'}], 'messages': []},
            {'agents': agents + [{'id': 'user', 'role': 'reserved'}], 'messages': []},
            {'agents': agents, 'messages': [{'to': '@not-found', 'text': 'hello'}]},
            {'agents': agents, 'groups': [{'id': 'g', 'members': ['z', 'missing']}],
             'messages': [{'to': '#g', 'text': 'hello'}]},
            {'agents': agents, 'queue_limit': 1, 'groups': [{'id': 'g', 'members': ['z', 'a']}],
             'messages': [{'to': '#g', 'text': 'hello'}]},
        ):
            assert run(bad).returncode != 0, bad

        many = [{'id': f'a{i:05d}', 'role': 'test'} for i in range(10000, 0, -1)]
        assert run({'agents': many, 'messages': []}).returncode == 0
        assert run({'agents': many + [many[0]], 'messages': []}).returncode != 0
        print('ok: indexed lookup, order, invalid destinations, limits, and 10,000 agents')
finally:
    server.shutdown()
    server.server_close()
    thread.join()
