#!/usr/bin/env python3
"""Focused CLI/config mapping checks; no network, credentials or real workspace."""
import json
import os
import subprocess
import sys
import tempfile

binary = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else './clawc')
env = {k: v for k, v in os.environ.items()
       if not (k.startswith('CLAWC_') or k in ('OPENAI_API_KEY', 'ANTHROPIC_API_KEY'))}
with tempfile.TemporaryDirectory() as directory:
    env['HOME'] = directory
    conf = os.path.join(directory, 'config.json')
    with open(conf, 'w', encoding='utf-8') as f:
        json.dump({'workspace': directory, 'api_url': 'http://from-file',
                   'api_key': 'file-secret', 'model': 'file-model',
                   'max_steps': 3, 'timeout': 20, 'auto_approve': False}, f)
    def run(*args):
        return subprocess.run([binary, *args], text=True, capture_output=True, env=env)
    result = run('-c', conf, '-w', directory, '-m', 'flag-model',
                 '-u', 'http://flag-url', '-k', 'flag-secret', '-a', 'anthropic',
                 '-s', '9', '-t', '17', '-x', directory, '--yes', '--insecure', 'config')
    assert result.returncode == 0, result.stderr
    expected = [f'workspace: {directory} (flag)', 'api_url: http://flag-url (flag)',
                'api_key: (set) (flag)', 'api: anthropic (flag)', 'model: flag-model (flag)',
                'max_steps: 9 (flag)', 'timeout: 17 (flag)', f'sandbox: {directory} (flag)',
                'insecure: on (flag)', 'auto_approve: on (flag)']
    for line in expected: assert line in result.stdout, (line, result.stdout)
    assert 'flag-secret' not in result.stdout and 'file-secret' not in result.stdout
    result = run('config', '--config', conf, '-i', '31', '-p', '9991',
                 '-b', '127.0.0.1', '-token', 'bot-secret', '-allow', '5,6', '-poll', '7')
    assert result.returncode == 0, result.stderr
    for line in ['heartbeat_interval: 31 (flag)', 'telegram_token: (set) (flag)',
                 'telegram_allow: 5,6 (flag)', 'telegram_poll: 7 (flag)']:
        assert line in result.stdout, (line, result.stdout)
    if 'serve_port:' in result.stdout:
        assert 'serve_port: 9991 (flag)' in result.stdout
        assert 'serve_bind: 127.0.0.1 (flag)' in result.stdout
    assert 'bot-secret' not in result.stdout
    for args in [('config', '-w', 'x'), ('config', '-i'), ('-w',), ('--bad', 'config')]:
        assert run(*args).returncode == 2, args
print('ok: CLI overrides, local flags, secret redaction and invalid arguments')
