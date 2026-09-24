"""A single-page document can send several BOX requests to one VLM concurrently."""
import json
import socket
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from server_integration import finished, ready, request


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *_):
        pass

    def do_POST(self):
        try:
            payload = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            assert self.path == '/v1/chat/completions'
            prompt = next(part['text'] for part in payload['messages'][-1]['content']
                          if part['type'] == 'text')
            with self.server.lock:
                self.server.active += 1
                self.server.peak = max(self.server.peak, self.server.active)
                self.server.calls += 1
                concurrent = self.server.concurrent
            try:
                if concurrent:
                    self.server.gate.wait(timeout=8)
                # Finish in a different order from reading order.
                time.sleep(.01 * (4 - int(prompt[-1])))
                body = json.dumps({'choices': [{'finish_reason': 'stop',
                    'message': {'content': prompt}}]}).encode()
                self.send_response(500 if prompt == self.server.fail_prompt else 200)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            finally:
                with self.server.lock:
                    self.server.active -= 1
        except Exception as exc:
            with self.server.lock:
                self.server.errors.append(str(exc))
            self.send_error(500)


def run(binary, server_binary):
    backend = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    backend.daemon_threads = True
    backend.lock = threading.Lock()
    backend.errors = []
    backend.fail_prompt = None
    thread = threading.Thread(target=backend.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            sample = root / 'one-page.ppm'
            sample.write_bytes(b'P6\n16 16\n255\n' + b'\xff' * 768)
            labels = [f'box-{i}' for i in range(4)]
            config = {'version': 1, 'execution': {'workers': 1},
                'layout': {'provider': 'normalized', 'model': 'layout',
                           'coordinates': 'normalized'},
                'models': {
                    'layout': {'backend': 'mock', 'response': {'boxes': [
                        {'type': label, 'bbox': [0, i / 4, 1, (i + 1) / 4], 'order': i}
                        for i, label in enumerate(labels)]}},
                    'ocr': {'backend': 'vllm', 'instances': 1,
                            'max_concurrent_requests': 4, 'model': 'served-vlm',
                            'endpoint': f'http://127.0.0.1:{backend.server_port}/v1/chat/completions'}},
                'routes': {label: {'model': 'ocr', 'prompt': label} for label in labels}}
            cfg = root / 'config.json'
            cfg.write_text(json.dumps(config))

            def check(output):
                assert [block['text'] for block in output['pages'][0]['blocks']] == labels, output

            for box_workers in (1, 4):
                backend.active = backend.peak = backend.calls = 0
                backend.concurrent = box_workers > 1
                backend.gate = threading.Barrier(4, timeout=8)
                with socket.socket() as sock:
                    sock.bind(('127.0.0.1', 0))
                    port = sock.getsockname()[1]
                base = f'http://127.0.0.1:{port}'
                config['execution'].update(page_workers=1, box_workers=1,
                                           document_workers=1, max_queued_pages=2)
                config['server'] = {'port': port,
                    'data_dir': str(root / f'state-{box_workers}'),
                    'allowed_input_root': str(root), 'max_active_jobs': 2,
                    'max_jobs': 1,
                    'max_result_bytes': 1,
                    'max_queued_page_bytes': 16 * 1024 * 1024}
                cfg.write_text(json.dumps(config))
                with open(root / f'server-{box_workers}.log', 'w') as log:
                    # First boot uses config only; second overrides one setting
                    # on the command line without changing the config file.
                    command = [server_binary, '--config', str(cfg)]
                    if box_workers > 1: command += ['--box-workers', str(box_workers),
                                                   '--max-result-bytes', '65536']
                    proc = subprocess.Popen(command, stdout=log, stderr=log)
                    try:
                        ready(base, proc)
                        code, response = request(base, '/v1/jobs', method='POST',
                            data=json.dumps({'path': str(sample)}).encode())
                        assert code == 202, (code, response)
                        assert finished(base, response['id'])['status'] == 'succeeded'
                        code, output = request(base, f"/v1/jobs/{response['id']}/result")
                        if box_workers == 1:
                            assert code == 400 and 'exceeds response limit' in output['error'], (code, output)
                        else:
                            assert code == 200, (code, output)
                            check(output)
                        assert backend.calls == 4 and backend.peak == box_workers, (
                            box_workers, backend.calls, backend.peak)
                        assert request(base, '/v1/jobs', method='POST',
                            data=json.dumps({'path': str(sample)}).encode())[0] == 503
                    finally:
                        proc.terminate()
                        try: proc.wait(timeout=10)
                        except subprocess.TimeoutExpired: proc.kill(); proc.wait()
                        assert proc.returncode == 0, proc.returncode

            backend.active = backend.peak = backend.calls = 0
            backend.concurrent = True
            backend.gate = threading.Barrier(4, timeout=8)
            config['execution']['box_workers'] = 4
            cfg.write_text(json.dumps(config))
            manifest = {'options': {'page_workers': 1, 'max_active_documents': 1},
                        'jobs': [{'input': str(sample), 'output': str(root / 'batch-out')}]}
            jobs = root / 'jobs.json'
            jobs.write_text(json.dumps(manifest))
            done = subprocess.run([binary, '--config', str(cfg), '--batch', str(jobs)],
                                  capture_output=True, text=True, timeout=30)
            assert done.returncode == 0, (done.stdout, done.stderr)
            check(json.loads((root / 'batch-out/result.json').read_text()))
            assert backend.calls == 4 and backend.peak == 4, (backend.calls, backend.peak)

            backend.active = backend.peak = backend.calls = 0
            backend.gate = threading.Barrier(4, timeout=8)
            manifest['options'].update(page_workers=2, max_active_documents=2)
            manifest['jobs'] = [{'input': str(sample), 'output': str(root / f'multi-{i}')}
                                for i in range(2)]
            jobs.write_text(json.dumps(manifest))
            parallel = subprocess.run([binary, '--config', str(cfg), '--batch', str(jobs)],
                                      capture_output=True, text=True, timeout=30)
            assert parallel.returncode == 0, (parallel.stdout, parallel.stderr)
            for job in manifest['jobs']:
                check(json.loads((Path(job['output']) / 'result.json').read_text()))
            assert backend.calls == 8 and backend.peak == 4, (backend.calls, backend.peak)

            # A failed BOX must not release its page while other pool tasks
            # still use the crop/image. Other blocks retain their identities.
            config['execution']['on_error'] = 'record'
            cfg.write_text(json.dumps(config))
            backend.fail_prompt = labels[2]
            backend.active = backend.peak = backend.calls = 0
            backend.gate = threading.Barrier(4, timeout=8)
            manifest['jobs'] = [{'input': str(sample), 'output': str(root / 'partial-out')}]
            jobs.write_text(json.dumps(manifest))
            partial = subprocess.run([binary, '--config', str(cfg), '--batch', str(jobs)],
                                     capture_output=True, text=True, timeout=30)
            assert partial.returncode == 2, (partial.stdout, partial.stderr)
            blocks = json.loads((root / 'partial-out/result.json').read_text())['pages'][0]['blocks']
            assert [b['text'] for i, b in enumerate(blocks) if i != 2] == [
                labels[0], labels[1], labels[3]]
            assert blocks[2]['error'] and backend.calls == 4 and backend.peak == 4
            assert not backend.errors, backend.errors
    finally:
        backend.shutdown(); backend.server_close(); thread.join()
    print('PASS: single-page BOX requests execute concurrently in bounded shared pools')


if __name__ == '__main__':
    run(str(Path(sys.argv[1]).resolve()), str(Path(sys.argv[2]).resolve()))
