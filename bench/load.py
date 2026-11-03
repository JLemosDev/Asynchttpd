#!/usr/bin/env python3
"""Teste de carga: N conexões keep-alive, cada uma enviando M requisições."""
import socket, sys, threading, time

host, port = "127.0.0.1", int(sys.argv[1]) if len(sys.argv) > 1 else 8080
conns = int(sys.argv[2]) if len(sys.argv) > 2 else 64
per = int(sys.argv[3]) if len(sys.argv) > 3 else 2000
REQ = b"GET /health HTTP/1.1\r\nHost: x\r\n\r\n"
ok = 0; lock = threading.Lock()

def worker():
    global ok
    s = socket.create_connection((host, port))
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    n = 0
    for _ in range(per):
        s.sendall(REQ)
        data = b""
        while not data.endswith(b'{"status":"ok"}'):
            chunk = s.recv(4096)
            if not chunk: break
            data += chunk
        if data.startswith(b"HTTP/1.1 200"): n += 1
    s.close()
    with lock: ok += n

t0 = time.time()
ts = [threading.Thread(target=worker) for _ in range(conns)]
[t.start() for t in ts]; [t.join() for t in ts]
dt = time.time() - t0
print(f"{ok}/{conns*per} ok em {dt:.2f}s -> {ok/dt:,.0f} req/s")
