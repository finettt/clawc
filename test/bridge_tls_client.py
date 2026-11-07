#!/usr/bin/env python3
"""Exercise the TLS bridge with an independent Python TLS client."""
import json
import os
import socket
import ssl
import sys

mode, ca, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
addr = ("127.0.0.1", port)
if mode == "plaintext":
    sock = socket.create_connection(addr, timeout=5)
    sock.settimeout(7)
    try:
        sock.sendall(b'{"text":"plaintext-must-fail"}\n')
        assert not sock.recv(100), "plaintext unexpectedly accepted"
    except (ConnectionResetError, BrokenPipeError):
        pass
    finally:
        sock.close()
    sys.exit(0)
ctx = ssl.create_default_context(cafile=ca)
hostname = "wrong.example" if mode == "wrong-host" else "localhost"
try:
    with socket.create_connection(addr, timeout=5) as raw:
        with ctx.wrap_socket(raw, server_hostname=hostname) as sock:
            if mode in ("wrong-host", "wrong-ca"):
                raise AssertionError("bad certificate unexpectedly accepted")
            req = {"text": "ping via encrypted bridge", "token": os.environ["CLAWC_BRIDGE_TOKEN"]}
            if mode == "wrong-token":
                req["token"] = "f" * 64
            sock.sendall(json.dumps(req).encode() + b"\n")
            data = b""
            while not data.endswith(b"\n"):
                part = sock.recv(4096)
                if not part:
                    break
                data += part
            result = json.loads(data)
            if mode == "wrong-token":
                assert result == {"ok": False, "error": "unauthorized"}, result
            else:
                assert result.get("ok") and "hello-from-tool" in result.get("reply", ""), result
except ssl.SSLCertVerificationError:
    if mode not in ("wrong-host", "wrong-ca"):
        raise
