"""Exercise persistent REST submission, raw upload, path admission, job status and PDF pages."""
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from reportlab.pdfgen.canvas import Canvas



class OCRHandler(BaseHTTPRequestHandler):
    lock = threading.Lock()
    active = 0
    peak = 0

    def log_message(self, *_):
        pass

    def do_POST(self):
        self.rfile.read(int(self.headers["Content-Length"]))
        with OCRHandler.lock:
            OCRHandler.active += 1
            OCRHandler.peak = max(OCRHandler.peak, OCRHandler.active)
        try:
            time.sleep(.12)
            data = b'{"text":"REST OCR"}'
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        finally:
            with OCRHandler.lock:
                OCRHandler.active -= 1


def request(base, route, method="GET", data=None, token="test-token", content_type=None):
    headers = {}
    if token is not None:
        headers["Authorization"] = "Bearer " + token
    if content_type:
        headers["Content-Type"] = content_type
    req = urllib.request.Request(base + route, data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=15) as response:
            payload = response.read()
            return response.status, json.loads(payload) if "json" in response.headers.get("Content-Type", "") else payload
    except urllib.error.HTTPError as error:
        payload = error.read()
        return error.code, json.loads(payload) if "json" in error.headers.get("Content-Type", "") else payload


def ready(base, process):
    for _ in range(150):
        if process.poll() is not None:
            raise AssertionError(f"server exited before ready: {process.returncode}")
        try:
            if request(base, "/healthz")[0] == 200:
                return
        except (OSError, TimeoutError):
            pass
        time.sleep(.05)
    raise AssertionError("REST server failed to become ready")


def finished(base, identifier):
    for _ in range(2400):
        code, status = request(base, f"/v1/jobs/{identifier}")
        assert code == 200, status
        if status["status"] in ("failed", "succeeded"):
            return status
        time.sleep(.05)
    raise AssertionError("job did not complete")


def run(binary, formats=False):
    with tempfile.TemporaryDirectory() as folder:
        root = Path(folder)
        permitted = root / "allowed"
        permitted.mkdir()
        (root / "data").mkdir()
        (permitted / "in.ppm").write_bytes(b"P6\n20 10\n255\n" + b"\x80" * 600)
        (root / "secret.ppm").write_bytes(b"P6\n20 10\n255\n" + b"\xff" * 600)
        (permitted / "escape.ppm").symlink_to(root / "secret.ppm")
        pdf = Canvas(str(permitted / "three.pdf"), pagesize=(144, 144))
        for page in range(3):
            pdf.drawString(10, 70, f"Page {page + 1}")
            pdf.showPage()
        pdf.save()
        inference = ThreadingHTTPServer(("127.0.0.1", 0), OCRHandler)
        inference_thread = threading.Thread(target=inference.serve_forever, daemon=True)
        inference_thread.start()
        config = {
            "version": 1,
            "execution": {"workers": 4, "on_error": "fail"},
            "document": {"dpi": 72, "max_pixels": 500000, "max_pages": 10},
            "layout": {"provider": "normalized", "model": "layout", "coordinates": "normalized"},
            "models": {
                "layout": {"backend": "mock", "response": {"boxes": [
                    {"type": "text", "bbox": [0, 0, 1, 1]}
                ]}},
                "ocr": {"backend": "http_json", "instances": 2,
                        "endpoint": f"http://127.0.0.1:{inference.server_port}/ocr"}
            },
            "routes": {"text": {"model": "ocr"}}
        }
        (root / "config.json").write_text(json.dumps(config))
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        base = f"http://127.0.0.1:{port}"
        env = {**os.environ, "OCR_SERVER_TOKEN": "test-token"}
        process = subprocess.Popen(
            [binary, "--config", str(root / "config.json"), "--data-dir", str(root / "data"),
             "--allowed-input-root", str(permitted), "--port", str(port),
             "--page-workers", "2", "--document-workers", "2", "--max-queued-pages", "2",
             "--max-upload-bytes", "8192", "--api-key-env", "OCR_SERVER_TOKEN"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
        try:
            ready(base, process)
            assert request(base, "/healthz", token=None)[0] == 401
            assert request(base, "/healthz", token="wrong")[0] == 401
            code, body = request(base, "/v1/jobs", method="POST",
                                 data=json.dumps({"path": str(permitted / "three.pdf"),
                                                  "priority": 10}).encode(),
                                 content_type="application/json")
            assert code == 202, (code, body)
            file_id = body["id"]
            code, response = request(base, "/v1/jobs/upload?extension=.ppm&priority=20",
                                     method="POST", data=(permitted / "in.ppm").read_bytes(),
                                     content_type="application/octet-stream")
            assert code == 202, (code, response)
            upload_id = response["id"]
            assert file_id != upload_id
            for identifier, expected in ((file_id, 3), (upload_id, 1)):
                status = finished(base, identifier)
                assert status["status"] == "succeeded", status
                code, result = request(base, f"/v1/jobs/{identifier}/result")
                assert code == 200 and [p["page"] for p in result["pages"]] == list(
                    range(1, expected + 1)), (code, result)
                assert all(p["blocks"][0]["text"] == "REST OCR" for p in result["pages"])
                code, md = request(base, f"/v1/jobs/{identifier}/result?format=markdown")
                assert code == 200 and md.count(b"<!-- page:") == expected
            if formats:
                payloads = {
                    '.CSV': b'ID,Value\n00123,hello\n',
                    '.rtf': br'{\rtf1\ansi REST FORMAT FIXTURE}',
                    '.html': b'<html><body><h1>REST HTML</h1></body></html>',
                }
                for ext, data in payloads.items():
                    code, body = request(base, '/v1/jobs/upload?extension='+ext, method='POST', data=data)
                    assert code == 202, (ext, code, body)
                    status = finished(base, body['id'])
                    assert status['status'] == 'succeeded', (ext, status)
                    code, result = request(base, f"/v1/jobs/{body['id']}/result")
                    assert code == 200 and result['pages'], (ext, result)
            assert request(base, "/v1/jobs/deadbeef")[0] == 404
            assert request(base, f"/v1/jobs/{file_id}/result?format=bad")[0] == 400
            assert request(base, "/v1/jobs", method="POST",
                           data=json.dumps({"path": str(root / "secret.ppm")}).encode())[0] == 400
            assert request(base, "/v1/jobs", method="POST",
                           data=json.dumps({"path": str(permitted / "escape.ppm")}).encode())[0] == 400
            assert request(base, "/v1/jobs", method="POST",
                           data=json.dumps({"path": str(permitted / "in.ppm"), "priority": 1.5}).encode())[0] == 400
            assert request(base, "/v1/jobs/upload?extension=.exe", method="POST", data=b"bad")[0] == 400
            assert request(base, "/v1/jobs/upload?extension=.ppm", method="POST",
                           data=b"x" * 8193)[0] == 413
            assert request(base, "/v1/jobs/upload?extension=.ppm", method="POST",
                           data=b"")[0] == 400
        finally:
            process.send_signal(signal.SIGTERM)
            try:
                out, err = process.communicate(timeout=12)
            except subprocess.TimeoutExpired:
                process.kill()
                out, err = process.communicate()
                raise AssertionError("server failed to terminate")
            inference.shutdown()
            inference.server_close()
            inference_thread.join()
            assert process.returncode == 0, (process.returncode, out[-1000:], err[-1000:])
            assert OCRHandler.peak >= 2, "server must perform multiple page inferences concurrently"
    print("PASS: REST server path + streamed binary, concurrent PDF pages, statuses, auth, path sandbox, limits")


if __name__ == "__main__":
    run(str(Path(sys.argv[1]).resolve()), "--formats" in sys.argv[2:])
