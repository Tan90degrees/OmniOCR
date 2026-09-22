"""V3 HTTP bridge fixture: polygon, null order, mask crop and schema-v2 output."""
import base64
import io
import json
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from PIL import Image


class Handler(BaseHTTPRequestHandler):
    calls = 0
    errors = []

    def log_message(self, *_):
        pass

    def do_POST(self):
        try:
            payload = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            if self.path == "/layout":
                response = {"res": {"boxes": [
                    {"label": "header", "coordinate": [12, 0, 20, 4], "score": .9, "order": None},
                    {"label": "text", "coordinate": [0, 0, 10, 8], "score": .96,
                     "polygon_points": [[0, 0], [10, 0], [5, 8]], "order": 0},
                    {"label": "paragraph_title", "coordinate": [10, 4, 20, 10], "score": .9, "order": 2}
                ]}}
            else:
                assert self.path == "/ocr", self.path
                binary = base64.b64decode(payload["image"].split(",", 1)[1])
                image = Image.open(io.BytesIO(binary)).convert("RGB")
                if payload["width"] == 10 and payload["height"] == 8:
                    assert image.getpixel((0, 7)) == (255, 255, 255)
                    assert image.getpixel((5, 2)) == (30, 30, 30)
                Handler.calls += 1
                response = {"text": "recognized"}
            content = json.dumps(response).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(content)))
            self.end_headers()
            self.wfile.write(content)
        except Exception as e:
            Handler.errors.append(repr(e))
            self.send_error(500)


def run(binary):
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "image.ppm").write_bytes(b"P6\n20 10\n255\n" + bytes([30] * 600))
            endpoint = f"http://127.0.0.1:{server.server_port}"
            conf = {
                "version": 1,
                "layout": {"provider": "paddle.doclayout_v3.http", "model": "layout",
                           "type_map": {"paragraph_title": "heading"}},
                "models": {"layout": {"backend": "http_json", "endpoint": endpoint + "/layout"},
                           "ocr": {"backend": "http_json", "endpoint": endpoint + "/ocr"}},
                "routes": {"header": {"action": "skip"},
                           "text": {"model": "ocr", "cropper": "polygon_mask_crop", "save_crop": True},
                           "heading": {"model": "ocr", "cropper": "bbox_crop"}}}
            (root / "config.json").write_text(json.dumps(conf))
            subprocess.run([binary, "--config", str(root / "config.json"), "--input",
                            str(root / "image.ppm"), "--output", str(root / "out")],
                           timeout=20, check=True)
            output = json.loads((root / "out/result.json").read_text())
            blocks = output["pages"][0]["blocks"]
            assert output["schema_version"] == 2
            assert [b["source_index"] for b in blocks] == [1, 2, 0]
            assert blocks[0]["polygon"] == [[0, 0], [10, 0], [5, 8]]
            assert blocks[0]["id"] == "p1-s1"
            assert blocks[2]["reading_order"] is None and blocks[2]["order"] is None
            assert blocks[0]["text"] == blocks[1]["text"] == "recognized"
            assert (root / "out" / blocks[0]["asset"]).is_file()
            assert Handler.calls == 2 and not Handler.errors, (Handler.calls, Handler.errors)
            md = (root / "out/result.md").read_text()
            assert "recognized" in md and "header" not in md
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
    print("PASS: V3 HTTP polygon, masked OCR crop, null order and schema-v2 output")


if __name__ == "__main__":
    run(str(Path(sys.argv[1]).resolve()))
