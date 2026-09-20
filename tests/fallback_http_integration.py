"""Exercise real HTTP failover, shared model slots, box ordering, and both failure policies."""
import base64
import json
import os
import struct
import subprocess
import sys
import tempfile
import threading
import time
from collections import Counter
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


class Handler(BaseHTTPRequestHandler):
    calls = Counter()
    active = Counter()
    peak = Counter()
    failures = []
    lock = threading.Lock()

    def log_message(self, *_args):
        pass

    def do_POST(self):
        try:
            request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            if self.path == "/layout":
                assert request["width"] == 20 and request["height"] == 10
                response = {"boxes": [
                    {"type": "text", "bbox": [0, 0, 10, 10], "order": 2},
                    {"type": "failure", "bbox": [0, 0, 10, 10], "order": 1},
                    {"type": "title", "bbox": [10, 0, 20, 10], "order": 0},
                ]}
                self.reply(200, response)
                return
            assert self.path in ("/primary", "/backup", "/broken"), self.path
            assert request["model"] == "fixture" and request["stream"] is False
            assert isinstance(request["messages"][-1]["content"][1]["text"], str)
            png_url = request["messages"][-1]["content"][0]["image_url"]["url"]
            assert png_url.startswith("data:image/png;base64,")
            png = base64.b64decode(png_url.split(",", 1)[1], validate=True)
            assert png.startswith(b"\\x89PNG\\r\\n\\x1a\\n")
            assert struct.unpack(">II", png[16:24]) == (10, 10)
            with self.lock:
                Handler.calls[self.path] += 1
                Handler.active[self.path] += 1
                Handler.peak[self.path] = max(Handler.peak[self.path], Handler.active[self.path])
            time.sleep(0.02)
            with self.lock:
                Handler.active[self.path] -= 1
            if self.path != "/backup":
                self.reply(503, {"error": {"message": "backend unavailable"}})
                return
            prompt = request["messages"][-1]["content"][1]["text"]
            self.reply(200, {"choices": [
                {"finish_reason": "stop", "message": {"content": "recovered:" + prompt}}
            ]})
        except Exception as exc:
            with self.lock:
                Handler.failures.append(repr(exc))
            self.send_error(500)

    def reply(self, status, value):
        data = json.dumps(value).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def run(binary):
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            image = root / "input.ppm"
            image.write_bytes(b"P6\\n20 10\\n255\\n" + b"\\xff" * 600)
            endpoint = f"http://127.0.0.1:{server.server_port}"
            cfg = {
                "version": 1,
                "execution": {"workers": 4, "on_error": "record"},
                "layout": {"provider": "normalized", "model": "layout"},
                "models": {
                    "layout": {"backend": "http_json", "endpoint": endpoint + "/layout"},
                    "primary": {"backend": "vllm", "model": "fixture",
                                "endpoint": endpoint + "/primary", "instances": 1},
                    "backup": {"backend": "vllm", "model": "fixture",
                               "endpoint": endpoint + "/backup", "instances": 1},
                    "broken": {"backend": "vllm", "model": "fixture",
                               "endpoint": endpoint + "/broken", "instances": 1},
                },
                "routes": {
                    "title": {"models": ["primary", "backup"], "prompt": "heading"},
                    "text": {"models": ["primary", "backup"], "prompt": "body"},
                    "failure": {"models": ["primary", "broken"], "prompt": "failed"},
                },
            }
            configuration = root / "config.json"
            configuration.write_text(json.dumps(cfg))
            invocation = [binary, "--config", str(configuration), "--input", str(image)]
            recorded = subprocess.run(
                invocation + ["--output", str(root / "record")],
                capture_output=True, text=True, timeout=30, env=os.environ.copy())
            assert recorded.returncode == 2, recorded.stderr + recorded.stdout
            result = json.loads((root / "record" / "result.json").read_text())
            blocks = result["pages"][0]["blocks"]
            assert [b["type"] for b in blocks] == ["title", "failure", "text"], blocks
            assert [blocks[0]["text"], blocks[2]["text"]] == [
                "recovered:heading", "recovered:body"], blocks
            assert blocks[0]["model"] == blocks[2]["model"] == "backup", blocks
            assert blocks[1]["model"] == "" and blocks[1]["text"] == "", blocks[1]
            assert "primary: HTTP status 503" in blocks[1]["error"], blocks[1]
            assert "broken: HTTP status 503" in blocks[1]["error"], blocks[1]
            markdown = (root / "record" / "result.md").read_text()
            assert "# recovered:heading" in markdown and "recovered:body" in markdown, markdown
            assert markdown.count("[OCR block failed]") == 1, markdown
            assert Handler.calls == Counter({
                "/primary": 3, "/backup": 2, "/broken": 1}), Handler.calls
            assert Handler.peak["/primary"] <= 1 and Handler.peak["/backup"] <= 1, Handler.peak
            assert not Handler.failures, Handler.failures
            cfg["execution"]["on_error"] = "fail"
            configuration.write_text(json.dumps(cfg))
            failed = subprocess.run(invocation + ["--output", str(root / "fail")],
                                    capture_output=True, text=True, timeout=30)
            assert failed.returncode == 1, failed.stderr + failed.stdout
            assert not (root / "fail" / "result.json").exists()
            assert not Handler.failures, Handler.failures
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
    print("PASS: HTTP model fallback, bounded shared slots, ordered JSON/Markdown, record/fail")


if __name__ == "__main__":
    run(str(Path(sys.argv[1]).resolve()))
