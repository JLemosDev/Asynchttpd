# asynchttpd

Servidor HTTP/1.1 assíncrono em C++20 para Linux.

## Arquitetura
- **Multi-reactor**: 1 thread = 1 loop `epoll` + 1 socket de escuta próprio (`SO_REUSEPORT`; o kernel balanceia os `accept`).
- **I/O não-bloqueante edge-triggered**, keep-alive, pipelining, backpressure no buffer de saída e timeout de conexões ociosas.
- **Parser HTTP incremental** próprio (limites de header/body, 400/413/431/501/505).
- **Roteador** por segmentos com parâmetros (`/users/:id`), 404/405 (+`Allow`), HEAD→GET, exceções→500.
- Desligamento gracioso com SIGINT/SIGTERM.
- Handlers rodam na thread do reactor e **não devem bloquear**.

## Build e testes
```
cmake -S . -B build && cmake --build build -j
./build/tests
```

## Uso
```
./build/httpd --port 8080 --threads 4 [--host 0.0.0.0] [--idle-timeout 30]
curl localhost:8080/health
curl -X PUT -d valor localhost:8080/kv/chave
curl localhost:8080/stats
python3 bench/load.py 8080 64 2000
```

Rotas de exemplo: `/`, `/health`, `/echo/:msg`, `POST /echo`, `/users/:id`, `GET|PUT|DELETE /kv/:key`, `/stats`.

## Limitações / próximos passos
Sem TLS, sem chunked encoding, sem IPv6; handlers síncronos. Ideias: thread pool para handlers lentos, `io_uring`, HTTP/2, coroutines C++20.
