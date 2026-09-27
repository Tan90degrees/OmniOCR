"""Persistent per-instance HTTP connections, response failure recovery and key isolation."""
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


class Fixture(ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 128

    def __init__(self):
        self.lock = threading.Lock()
        self.connections = self.calls = self.active = self.peak = 0
        self.errors = []
        super().__init__(("127.0.0.1", 0), Handler)


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def setup(self):
        super().setup()
        self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        with self.server.lock:
            self.server.connections += 1

    def log_message(self, *_):
        pass

    def do_POST(self):
        with self.server.lock:
            self.server.calls += 1
            self.server.active += 1
            self.server.peak = max(self.server.peak, self.server.active)
        try:
            value = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            prompt = value['prompt']
            expected = 'Bearer key-b' if prompt == 'private-b' else 'Bearer key-a'
            assert self.headers.get('Authorization') == expected
            assert value['width'] == 16 and value['height'] == 16
            assert value['image'].startswith('data:image/png;base64,')
            time.sleep(.002)
            status = 500 if prompt == 'http-error' else 200
            if prompt == 'bad-json': body = b'{broken'
            elif prompt == 'oversize': body = b'x' * 4096
            else: body = json.dumps({'text': prompt}).encode()
            self.send_response(status)
            self.send_header('Content-Length', str(len(body)))
            if prompt == 'close':
                self.send_header('Connection', 'close')
                self.close_connection = True
            self.end_headers()
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass  # expected if the client enforces the response limit
        except Exception as exc:
            self.server.errors.append(repr(exc))
            self.send_error(500)
        finally:
            with self.server.lock:
                self.server.active -= 1


def run(binary):
    server = Fixture()
    thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
    try:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            image = root / 'in.ppm'
            image.write_bytes(b'P6\n16 16\n255\n' + b'\xff' * 768)
            prompts = ['ok-1', 'http-error', 'ok-2', 'bad-json', 'ok-3', 'oversize', 'ok-4', 'close', 'ok-5', 'private-b']
            remote = {'backend': 'http_json', 'endpoint': f'http://127.0.0.1:{server.server_port}/ocr',
                      'api_key_env': 'REUSE_KEY_A', 'max_response_bytes': 1024}
            config = {
                'version': 1, 'execution': {'workers': 1, 'on_error': 'record'},
                'layout': {'provider': 'normalized', 'model': 'layout'},
                'models': {'layout': {'backend': 'mock', 'response': {'boxes': [
                    {'type': p, 'bbox': [0, 0, 16, 16], 'order': i} for i, p in enumerate(prompts)]}},
                    'a': remote, 'b': {**remote, 'api_key_env': 'REUSE_KEY_B'}},
                'routes': {p: {'model': 'b' if p == 'private-b' else 'a', 'prompt': p} for p in prompts}}
            path = root / 'config.json'; path.write_text(json.dumps(config))
            env = {**os.environ, 'REUSE_KEY_A': 'key-a', 'REUSE_KEY_B': 'key-b'}
            result = subprocess.run([binary, '--config', str(path), '--input', str(image), '--output', str(root/'out')],
                                    env=env, capture_output=True, timeout=30)
            assert result.returncode == 2, result.stderr
            blocks = json.loads((root/'out/result.json').read_text())['pages'][0]['blocks']
            assert len(blocks) == len(prompts)
            for p, block in zip(prompts, blocks):
                if p in ('http-error', 'bad-json', 'oversize'): assert block['error'], block
                else: assert block['text'] == p and not block['error'], block
            assert server.calls == 10 and server.connections <= 4, (server.calls, server.connections)
            assert not server.errors, server.errors
            # The same bounded clients migrate between concurrent page workers.
            with server.lock: server.connections = server.calls = server.peak = 0
            config['execution'] = {'workers': 8}
            config['models'].pop('b')
            config['models']['a']['instances'] = 3
            config['models']['layout']['response']['boxes'] = [
                {'type': 'text', 'bbox': [0, 0, 16, 16]} for _ in range(4)]
            config['routes'] = {'text': {'model': 'a', 'prompt': 'parallel'}}
            path.write_text(json.dumps(config))
            jobs = {'options': {'page_workers': 8}, 'jobs': [
                {'input': str(image), 'output': str(root / f'batch-{i}')} for i in range(24)]}
            manifest = root/'jobs.json'; manifest.write_text(json.dumps(jobs))
            subprocess.run([binary, '--config', str(path), '--batch', str(manifest)],
                           env=env, check=True, capture_output=True, timeout=30)
            assert server.calls == 96 and server.connections == 3, (server.calls, server.connections)
            assert 2 <= server.peak <= 3 and not server.errors, (server.peak, server.errors)
    finally:
        server.shutdown(); server.server_close(); thread.join()
    print('PASS: HTTP reuse, key isolation, error recovery, response bound and cross-thread instance leases')


if __name__ == '__main__':
    run(str(Path(sys.argv[1]).resolve()))
