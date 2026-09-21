"""Real PDF page scheduling, multi-file isolation and concurrent HTTP OCR regression."""
import json
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from reportlab.pdfgen.canvas import Canvas


class Handler(BaseHTTPRequestHandler):
    lock = threading.Lock()
    barrier = threading.Barrier(2, timeout=12)
    active = 0
    peak = 0
    requests = 0
    failures = []

    def log_message(self, *_):
        pass

    def do_POST(self):
        try:
            json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            with Handler.lock:
                Handler.requests += 1
                call = Handler.requests
                Handler.active += 1
                Handler.peak = max(Handler.peak, Handler.active)
            try:
                # The first two OCR requests must run concurrently. A document-level
                # serial scheduler, or page_workers=1, cannot pass this barrier.
                if call <= 2:
                    Handler.barrier.wait()
                time.sleep(0.02)
                body = json.dumps({"text": "page OCR"}).encode()
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            finally:
                with Handler.lock:
                    Handler.active -= 1
        except Exception as error:
            Handler.failures.append(repr(error))
            self.send_error(500)


def run(binary):
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for name in ("low", "high"):
                pdf = Canvas(str(root / (name + ".pdf")), pagesize=(144, 144))
                for n in range(3):
                    pdf.drawString(12, 110, f"{name} page {n+1}")
                    pdf.showPage()
                pdf.save()
            (root / "image.ppm").write_bytes(b"P6\n20 10\n255\n" + bytes([255, 255, 255]) * 200)
            config = {
                "version": 1,
                "document": {"dpi": 72, "max_pages": 12, "max_pixels": 500000},
                "execution": {"workers": 4, "on_error": "fail"},
                "layout": {"provider": "normalized", "model": "layout", "coordinates": "normalized"},
                "models": {
                    "layout": {"backend": "mock", "instances": 2,
                               "response": {"boxes": [{"type": "text", "bbox": [0, 0, 1, 1]}]}},
                    "ocr": {"backend": "http_json", "instances": 2,
                            "endpoint": f"http://127.0.0.1:{server.server_port}/ocr"}
                },
                "routes": {"text": {"model": "ocr"}}
            }
            (root / "config.json").write_text(json.dumps(config))
            jobs = [
                {"input": "low.pdf", "output": "low-out", "priority": -10},
                {"input": "high.pdf", "output": "high-out", "priority": 20},
                {"input": "missing.pdf", "output": "missing-out", "priority": 0},
                {"input": "image.ppm", "output": "image-out", "priority": 5}
            ]
            manifest = {"options": {"page_workers": 2, "max_active_documents": 2,
                                    "max_queued_pages": 2}, "jobs": jobs}
            (root / "jobs.json").write_text(json.dumps(manifest))
            result = subprocess.run([binary, "--config", str(root / "config.json"),
                                     "--batch", str(root / "jobs.json")],
                                    capture_output=True, text=True, timeout=90)
            assert result.returncode == 1, (result.returncode, result.stderr, result.stdout)
            for name in ("low", "high"):
                data = json.loads((root / (name + "-out/result.json")).read_text())
                assert [p["page"] for p in data["pages"]] == [1, 2, 3], name
                assert all(p["blocks"][0]["text"] == "page OCR" for p in data["pages"])
                md = (root / (name + "-out/result.md")).read_text()
                assert [md.index(f"<!-- page: {i} -->") for i in (1, 2, 3)] == sorted(
                    md.index(f"<!-- page: {i} -->") for i in (1, 2, 3))
            assert len(json.loads((root / "image-out/result.json").read_text())["pages"]) == 1
            assert not (root / "missing-out/result.json").exists()
            assert Handler.peak >= 2 and not Handler.failures, (Handler.peak, Handler.failures)
            assert Handler.requests == 7, Handler.requests
            # A malformed priority must be rejected before any output directories are created.
            bad = {"jobs": [{"input": "image.ppm", "output": "bad-out", "priority": 1.5}]}
            (root / "bad.json").write_text(json.dumps(bad))
            invalid = subprocess.run([binary, "--config", str(root / "config.json"),
                                      "--batch", str(root / "bad.json")], capture_output=True, timeout=10)
            assert invalid.returncode == 1 and not (root / "bad-out").exists()
            overlap = {"jobs": [
                {"input": "image.ppm", "output": "overlap"},
                {"input": "image.ppm", "output": "overlap/child"}
            ]}
            (root / "overlap.json").write_text(json.dumps(overlap))
            invalid = subprocess.run([binary, "--config", str(root / "config.json"),
                                      "--batch", str(root / "overlap.json")], capture_output=True, timeout=10)
            assert invalid.returncode == 1 and not (root / "overlap").exists()
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
    print("PASS: batch PDF page concurrency, priorities, ordered results, failure isolation, invalid manifests")


if __name__ == "__main__":
    run(str(Path(sys.argv[1]).resolve()))
