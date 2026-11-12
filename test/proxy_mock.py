#!/usr/bin/env python3
"""Mock proxies for the clawc proxy tests: HTTP CONNECT and SOCKS5.

Both relay to a real target, so a test can assert that a request reached the
origin *through* the proxy rather than directly.

Usage (import):
    import proxy_mock
    http_srv, http_port = proxy_mock.start_http_connect(stats)
    socks_srv, socks_port = proxy_mock.start_socks5(stats)
    ...
    http_srv.shutdown(); socks_srv.shutdown()

`stats` is a dict the servers mutate: {"connect": n, "socks": n, "socks_atyp": set()}
"""
import socket
import threading


def _pump(src, dst):
    try:
        while True:
            data = src.recv(65536)
            if not data:
                break
            dst.sendall(data)
    except Exception:
        pass


def _tunnel(a, b):
    threading.Thread(target=_pump, args=(a, b), daemon=True).start()
    _pump(b, a)
    # shutdown() sends FIN immediately; a bare close() is deferred while the
    # other pump thread is still blocked in recv() on the same socket, which
    # leaves the client waiting forever.
    for s in (a, b):
        try:
            s.shutdown(socket.SHUT_RDWR)
        except Exception:
            pass
        try:
            s.close()
        except Exception:
            pass


def _serve(port, handler):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('127.0.0.1', port))
    srv.listen(16)

    def loop():
        while True:
            try:
                conn, _ = srv.accept()
            except OSError:
                return          # shutdown()
            threading.Thread(target=handler, args=(conn,), daemon=True).start()

    threading.Thread(target=loop, daemon=True).start()
    return srv


def start_http_connect(stats, port=0, refuse=False, require_auth=None):
    """HTTP proxy speaking CONNECT. `refuse` replies 502 to every CONNECT;
    `require_auth` is an expected 'user:pass' (replies 407 when it does not
    match the Proxy-Authorization header)."""
    import base64

    def handler(conn):
        head = b''
        try:
            while b'\r\n\r\n' not in head:
                chunk = conn.recv(4096)
                if not chunk:
                    return
                head += chunk
            lines = head.split(b'\r\n')
            request = lines[0].decode('latin-1')
            if not request.startswith('CONNECT '):
                conn.sendall(b'HTTP/1.1 405 Method Not Allowed\r\n\r\n')
                return
            if require_auth is not None:
                got = None
                for line in lines[1:]:
                    if line.lower().startswith(b'proxy-authorization:'):
                        token = line.split(b' ', 2)[-1].strip()
                        try:
                            got = base64.b64decode(token).decode('latin-1')
                        except Exception:
                            got = None
                if got != require_auth:
                    conn.sendall(b'HTTP/1.1 407 Proxy Authentication Required\r\n'
                                 b'Proxy-Authenticate: Basic realm="mock"\r\n\r\n')
                    return
            if refuse:
                conn.sendall(b'HTTP/1.1 502 Bad Gateway\r\n\r\n')
                return
            stats['connect'] = stats.get('connect', 0) + 1
            host, port_s = request.split(' ')[1].rsplit(':', 1)
            upstream = socket.create_connection((host, int(port_s)), timeout=10)
            conn.sendall(b'HTTP/1.1 200 Connection established\r\n\r\n')
            _tunnel(conn, upstream)
        except Exception:
            try:
                conn.close()
            except Exception:
                pass

    return _serve(port, handler)


def start_socks5(stats, port=0, fail_code=0x00):
    """SOCKS5 proxy, no-auth only. `fail_code` != 0 makes every CONNECT fail
    with that reply code (e.g. 0x05 = connection refused)."""

    def handler(conn):
        try:
            greet = conn.recv(2)
            if len(greet) < 2 or greet[0] != 0x05:
                conn.close()
                return
            conn.recv(greet[1])
            conn.sendall(b'\x05\x00')                 # no auth
            head = conn.recv(4)
            atyp = head[3]
            stats.setdefault('socks_atyp', []).append(atyp)
            if atyp == 0x03:
                ln = conn.recv(1)[0]
                host = conn.recv(ln).decode()
            elif atyp == 0x01:
                host = socket.inet_ntoa(conn.recv(4))
            elif atyp == 0x04:
                host = socket.inet_ntop(socket.AF_INET6, conn.recv(16))
            else:
                conn.sendall(b'\x05\x08\x00\x01' + b'\x00' * 6)
                return
            port_n = int.from_bytes(conn.recv(2), 'big')
            stats['socks'] = stats.get('socks', 0) + 1
            stats.setdefault('socks_targets', []).append((host, port_n))
            if fail_code:
                conn.sendall(bytes([0x05, fail_code]) + b'\x00\x01' + b'\x00' * 6)
                return
            upstream = socket.create_connection((host, port_n), timeout=10)
            conn.sendall(b'\x05\x00\x00\x01' + socket.inet_aton('127.0.0.1') + b'\x00\x00')
            _tunnel(conn, upstream)
        except Exception:
            try:
                conn.close()
            except Exception:
                pass

    srv = _serve(port, handler)
    return srv


def port_of(srv):
    return srv.getsockname()[1]


# Convenience: start both on ephemeral ports and return the ports.
def start_all(stats):
    http_srv = start_http_connect(stats)
    socks_srv = start_socks5(stats)
    return http_srv, port_of(http_srv), socks_srv, port_of(socks_srv)
