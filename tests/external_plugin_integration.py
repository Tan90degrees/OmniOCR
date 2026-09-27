"""An external C shared library supplies OCR through CLI, batch and REST unchanged."""
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path


def run(binary, server_binary, source):
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        library = root / "libexample_backend.so"
        subprocess.run(["cc", "-shared", "-fPIC", "-std=c11", "-I",
                        str(source / "include"),
                        str(source / "tests/plugins/sample_backend.c"),
                        "-o", str(library)], check=True)
        (root / "input.ppm").write_bytes(b"P6\n20 10\n255\n" + b"\xff" * 600)
        config = {
            "version": 1,
            "plugins": [{"id": "example.external", "library": str(library)}],
            "layout": {"provider": "normalized", "model": "layout", "coordinates": "normalized"},
            "models": {
                "layout": {"backend": "mock", "response": {"boxes": [
                    {"type": "text", "bbox": [0, 0, 1, 1]}]}},
                "ocr": {"backend": "example.external", "instances": 2}},
            "routes": {"text": {"model": "ocr"}}
        }
        cfg = root / "config.json"
        cfg.write_text(json.dumps(config))
        subprocess.run([binary, "--config", str(cfg), "--validate"], check=True)
        subprocess.run([binary, "--config", str(cfg), "--input", str(root / "input.ppm"),
                        "--output", str(root / "out")], check=True)
        data = json.loads((root / "out/result.json").read_text())
        assert data["pages"][0]["blocks"][0]["text"] == "EXTERNAL_OK"
        config['models']['ocr']['batch_size'] = 2
        cfg.write_text(json.dumps(config))
        unsupported = subprocess.run([binary, '--config', str(cfg), '--input', str(root/'input.ppm'),
                                     '--output', str(root/'old-plugin-batch')], capture_output=True, timeout=30)
        assert unsupported.returncode == 1 and b'does not support native batching' in unsupported.stderr
        config['models']['ocr'].pop('batch_size')
        cfg.write_text(json.dumps(config))
        manifest = [{"input": "input.ppm", "output": "batch-1", "priority": 100},
                    {"input": "input.ppm", "output": "batch-2", "priority": 0}]
        (root / "jobs.json").write_text(json.dumps(manifest))
        subprocess.run([binary, "--config", str(cfg), "--batch", str(root / "jobs.json")],
                       check=True)
        assert all(json.loads((root / f"batch-{i}/result.json").read_text())
                   ["pages"][0]["blocks"][0]["text"] == "EXTERNAL_OK" for i in (1, 2))
        if server_binary:
            with socket.socket() as sock:
                sock.bind(("127.0.0.1", 0))
                port = sock.getsockname()[1]
            server = subprocess.Popen(
                [server_binary, "--config", str(cfg), "--data-dir", str(root / "server-data"),
                 "--allowed-input-root", str(root), "--port", str(port)],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            base = f"http://127.0.0.1:{port}"
            try:
                for _ in range(100):
                    if server.poll() is not None:
                        raise AssertionError(f"plugin server exit {server.returncode}")
                    try:
                        with urllib.request.urlopen(base + "/healthz", timeout=2):
                            break
                    except OSError:
                        time.sleep(.05)
                else:
                    raise AssertionError("plugin server failed to start")
                request = urllib.request.Request(base + "/v1/jobs",
                    data=json.dumps({"path": str(root / "input.ppm")}).encode(),
                    headers={"Content-Type": "application/json"}, method="POST")
                with urllib.request.urlopen(request, timeout=5) as response:
                    job = json.load(response)["id"]
                for _ in range(120):
                    with urllib.request.urlopen(base + "/v1/jobs/" + job, timeout=2) as response:
                        status = json.load(response)
                    if status["status"] == "succeeded":
                        break
                    if status["status"] == "failed":
                        raise AssertionError(status)
                    time.sleep(.05)
                else:
                    raise AssertionError("external plugin job never finished")
                with urllib.request.urlopen(base + "/v1/jobs/" + job + "/result") as response:
                    result = json.load(response)
                assert result["pages"][0]["blocks"][0]["text"] == "EXTERNAL_OK"
            finally:
                server.send_signal(signal.SIGTERM)
                try:
                    _, stderr = server.communicate(timeout=10)
                except subprocess.TimeoutExpired:
                    server.kill()
                    _, stderr = server.communicate()
                    raise AssertionError(stderr[-1000:])
                assert server.returncode == 0, stderr[-1000:]
        batch_library = root / 'libbatch_backend.so'
        subprocess.run(['cc', '-shared', '-fPIC', '-std=c11', '-I', str(source/'include'),
                        str(source/'tests/plugins/sample_batch_backend.c'), '-o', str(batch_library)],
                       check=True)
        config['plugins'] = [{'id': 'example.batch', 'library': str(batch_library)}]
        config['models']['ocr'] = {'backend': 'example.batch', 'instances': 1,
                                   'batch_size': 2, 'max_batch_wait_ms': 20}
        config['execution'] = {'workers': 2}
        config['models']['layout']['response']['boxes'] = [
            {'type': 'text', 'bbox': [0, 0, 1, .5]},
            {'type': 'text', 'bbox': [0, .5, 1, 1]}]
        cfg.write_text(json.dumps(config))
        subprocess.run([binary, '--config', str(cfg), '--input', str(root/'input.ppm'),
                        '--output', str(root/'batch-plugin')], check=True, timeout=30)
        result = json.loads((root/'batch-plugin/result.json').read_text())
        assert [block['text'] for block in result['pages'][0]['blocks']] == ['EXTERNAL_BATCH'] * 2
    print("PASS: independent C ABI plugin runs without pipeline changes in CLI, batch and REST")


if __name__ == "__main__":
    run(str(Path(sys.argv[1]).resolve()),
        str(Path(sys.argv[2]).resolve()) if len(sys.argv) > 2 else None,
        Path(__file__).resolve().parents[1])
