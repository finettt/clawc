#!/usr/bin/env python3
"""Accept one connection as a plaintext peer and capture bytes on the wire."""
import socket
import sys

port, path = int(sys.argv[1]), sys.argv[2]
with socket.socket() as listener:
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", port))
    listener.listen(1)
    listener.settimeout(15)
    with listener.accept()[0] as conn:
        conn.settimeout(5)
        data = conn.recv(8192)
        with open(path, "wb") as out:
            out.write(data)
