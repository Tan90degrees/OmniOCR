"""Real C++ HTTP/PNG/CLI integration against a local OpenAI-compatible fixture server."""
import base64
import json
import struct
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


class Handler(BaseHTTPRequestHandler):
    active = 0
    peak = 0
    calls = []
    failures = []
    lock = threading.Lock()

    def log_message(self, *_):
        pass

    def do_POST(self):
        try:
            req = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            if self.path == '/layout':
                assert req['width'] == 20 and req['height'] == 10
                assert req['image'].startswith('data:image/png;base64,')
                result = {'res': {'boxes': [
                    {'label': 'text', 'coordinate': [0, 0, 10, 10], 'order': 1},
                    {'label': 'doc_title', 'coordinate': [10, 0, 20, 10], 'order': 0}
                ]}}
            else:
                assert self.path == '/v1/chat/completions'
                if req['model'] == 'mineru':
                    content = req['messages'][-1]['content']
                    prompt = content[1]['text']
                    png = base64.b64decode(content[0]['image_url']['url'].split(',')[1])
                    if 'Layout Detection' in prompt:
                        assert struct.unpack('>II', png[16:24]) == (16, 16)
                        answer = '<|box_start|>0 0 500 1000<|box_end|><|ref_start|>table<|ref_end|><|rotate_up|>'
                    else:
                        assert struct.unpack('>II', png[16:24]) == (10, 10)
                        answer = '<fcel>合并<lcel><nl><ucel><xcel><nl>'
                    data = json.dumps({'choices': [{'finish_reason': 'stop', 'message': {'content': answer}}]}).encode()
                    self.send_response(200)
                    self.send_header('Content-Length', str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)
                    return
                assert req['model'] == 'test-model' and req['stream'] is False
                assert self.headers['Authorization'] == 'Bearer test-secret'
                content = req['messages'][-1]['content']
                png = base64.b64decode(content[0]['image_url']['url'].split(',')[1])
                assert png[:8] == b'\x89PNG\r\n\x1a\n'
                assert struct.unpack('>II', png[16:24]) == (10, 10)
                prompt = content[1]['text']
                with self.lock:
                    Handler.active += 1
                    Handler.peak = max(Handler.peak, Handler.active)
                    Handler.calls.append(prompt)
                time.sleep(.03 if prompt == 'title' else .01)
                with self.lock:
                    Handler.active -= 1
                result = {'choices': [{'finish_reason': 'stop', 'message': {'content': '中文 ' + prompt}}]}
            data = json.dumps(result).encode()
            self.send_response(200)
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        except Exception as exc:
            Handler.failures.append(repr(exc))
            self.send_error(500)


def run(binary):
    import os
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'in.ppm').write_bytes(b'P6\n20 10\n255\n' + b'\xff' * 600)
            endpoint = f'http://127.0.0.1:{server.server_port}'
            config = {
                'version': 1, 'execution': {'workers': 8},
                'layout': {'provider': 'paddle', 'model': 'layout', 'type_map': {'doc_title': 'title'}},
                'models': {
                    'layout': {'backend': 'http_json', 'endpoint': endpoint + '/layout'},
                    'ocr': {'backend': 'vllm', 'endpoint': endpoint + '/v1/chat/completions', 'model': 'test-model',
                            'instances': 1, 'api_key_env': 'TEST_API_KEY'}},
                'routes': {'title': {'model': 'ocr', 'prompt': 'title'}, 'text': {'model': 'ocr', 'prompt': 'text'}}}
            (root / 'config.json').write_text(json.dumps(config))
            env = {**os.environ, 'TEST_API_KEY': 'test-secret'}
            subprocess.run([binary, '--config', str(root / 'config.json'), '--input', str(root / 'in.ppm'),
                            '--output', str(root / 'out')], check=True, env=env, timeout=15)
            result = json.loads((root / 'out/result.json').read_text())
            blocks = result['pages'][0]['blocks']
            assert [b['text'] for b in blocks] == ['中文 title', '中文 text']
            assert Handler.peak == 1 and sorted(Handler.calls) == ['text', 'title']
            assert not Handler.failures, Handler.failures
            mineru = json.loads(json.dumps(config))
            mineru['models'].pop('layout')
            mineru['models']['ocr']['model'] = 'mineru'
            mineru['layout'] = {'provider': 'mineru', 'model': 'ocr', 'image_size': [16, 16]}
            mineru['routes'] = {'table': {'model': 'ocr', 'prompt': 'Table Recognition:'}}
            (root / 'mineru.json').write_text(json.dumps(mineru))
            subprocess.run([binary, '--config', str(root / 'mineru.json'), '--input', str(root / 'in.ppm'),
                            '--output', str(root / 'mineru')], check=True, env=env, timeout=15)
            block = json.loads((root / 'mineru/result.json').read_text())['pages'][0]['blocks'][0]
            assert block['bbox'] == [0, 0, 10, 10] and '<td rowspan="2" colspan="2">合并</td>' in block['text']
            assert '<fcel>' in block['raw_text']
            assert not Handler.failures, Handler.failures
            # HTTP failure must produce nonzero CLI exit, with no successful result file.
            config['models']['ocr']['endpoint'] = endpoint + '/bad'
            (root / 'config.json').write_text(json.dumps(config))
            failed = subprocess.run([binary, '--config', str(root / 'config.json'), '--input', str(root / 'in.ppm'),
                                     '--output', str(root / 'bad')], env=env, capture_output=True, timeout=15)
            assert failed.returncode == 1 and not (root / 'bad/result.json').exists()
            assert 'test-secret' not in failed.stderr.decode()
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
    print('PASS: HTTP image request, crop dimensions, shared bound, output order, HTTP failure')


if __name__ == '__main__':
    run(str(Path(sys.argv[1]).resolve()))
