#!/usr/bin/env python3
"""Closed-loop REST benchmark. Synthetic results measure orchestration, not OCR quality.

Uses only the Python standard library. Starts an isolated local server per repeat;
with --config/--input it uses the supplied model config instead of the HTTP fixture.
"""
import argparse
import concurrent.futures
import hashlib
import json
import math
import os
import platform
import socket
import subprocess
import tempfile
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


def request(base, path, body=None):
    req = urllib.request.Request(base + path, data=body,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=30) as response:
        return response.status, json.load(response)


def distribution(values):
    values = sorted(values)
    def p(q):
        return values[max(0, math.ceil(len(values) * q) - 1)] if values else None
    return {"count": len(values), "p50_ms": p(.5), "p95_ms": p(.95), "p99_ms": p(.99)}


class Fixture(ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 128

    def __init__(self, args):
        self.args = args
        self.lock = threading.Lock()
        self.connections = self.calls = self.active = self.peak = 0
        super().__init__(("127.0.0.1", 0), Handler)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def setup(self):
        super().setup()
        self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        with self.server.lock:
            self.server.connections += 1
        time.sleep(self.server.args.connection_delay_ms / 1000)

    def log_message(self, *_):
        pass

    def do_POST(self):
        self.rfile.read(int(self.headers["Content-Length"]))
        with self.server.lock:
            self.server.calls += 1
            self.server.active += 1
            self.server.peak = max(self.server.peak, self.server.active)
        try:
            time.sleep(self.server.args.model_delay_ms / 1000)
            body = b'{"text":"synthetic benchmark output"}'
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        finally:
            with self.server.lock:
                self.server.active -= 1


def sample_resources(pid, stop, peaks):
    while not stop.is_set():
        try:
            fields = {}
            for line in Path(f"/proc/{pid}/status").read_text().splitlines():
                key, _, value = line.partition(":")
                parts = value.strip().split()
                if parts: fields[key] = parts[0]
            peaks["rss_kib"] = max(peaks["rss_kib"], int(fields.get("VmRSS", 0)))
            peaks["threads"] = max(peaks["threads"], int(fields.get("Threads", 0)))
            peaks["fds"] = max(peaks["fds"], len(list(Path(f"/proc/{pid}/fd").iterdir())))
        except (OSError, ValueError, IndexError):
            pass
        stop.wait(.01)


def run_once(args, repeat):
    fixture = None
    if not args.config:
        fixture = Fixture(args)
        threading.Thread(target=fixture.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix="omniocr-benchmark-") as directory:
            root = Path(directory)
            if args.config:
                config_path, source = Path(args.config).resolve(), Path(args.input).resolve()
            else:
                source = root / "input.ppm"
                # Deterministic pixels; no external image or rendering dependency.
                source.write_bytes(b"P6\n128 128\n255\n" + bytes(range(256)) * 192)
                config = {
                    "version": 1, "execution": {"workers": 8},
                    "layout": {"provider": "normalized", "model": "layout", "coordinates": "normalized"},
                    "models": {
                        "layout": {"backend": "mock", "response": {"boxes": [
                            {"type": "text", "bbox": [0, i / args.boxes, 1, (i+1) / args.boxes], "order": i}
                            for i in range(args.boxes)]}},
                        "ocr": {"backend": "http_json", "instances": args.model_instances,
                                "endpoint": f"http://127.0.0.1:{fixture.server_port}/ocr"}},
                    "routes": {"text": {"model": "ocr"}}}
                config_path = root / "config.json"
                config_path.write_text(json.dumps(config))
            with socket.socket() as sock:
                sock.bind(("127.0.0.1", 0))
                port = sock.getsockname()[1]
            base = f"http://127.0.0.1:{port}"
            command = [str(Path(args.server_binary).resolve()), "--config", str(config_path),
                       "--data-dir", str(root / "state"), "--allowed-input-root", str(source.parent),
                       "--port", str(port), "--page-workers", str(args.page_workers),
                       "--box-workers", str(args.box_workers),
                       "--document-workers", str(args.document_workers), "--max-queued-pages", str(args.queued_pages),
                       "--max-jobs", str(args.requests + args.warmup + args.history_jobs)]
            peaks = {"rss_kib": 0, "threads": 0, "fds": 0}
            stop = threading.Event()
            with open(root / "server.log", "w+") as log:
                proc = subprocess.Popen(command, stdout=log, stderr=log)
                monitor = threading.Thread(target=sample_resources, args=(proc.pid, stop, peaks))
                monitor.start()
                try:
                    deadline = time.monotonic() + args.timeout
                    while True:
                        if proc.poll() is not None:
                            log.seek(0)
                            raise RuntimeError("server startup failed: " + log.read()[-2000:])
                        try:
                            if request(base, "/healthz")[0] == 200:
                                break
                        except OSError:
                            pass
                        if time.monotonic() > deadline:
                            raise TimeoutError("server startup timeout")
                        time.sleep(.05)

                    def job(index):
                        start = time.perf_counter()
                        row = {"index": index, "status": "request_error", "id": None}
                        try:
                            code, accepted = request(base, "/v1/jobs", json.dumps({"path": str(source)}).encode())
                            row["admission_ms"] = (time.perf_counter() - start) * 1000
                            if code != 202:
                                raise RuntimeError(f"unexpected admission status {code}")
                            row["id"] = accepted["id"]
                            while time.perf_counter() - start < args.timeout:
                                _, status = request(base, "/v1/jobs/" + row["id"])
                                if status["status"] in ("succeeded", "failed"):
                                    row["status"] = status["status"]
                                    row["pages"] = status["pages_completed"]
                                    row["completion_ms"] = (time.perf_counter() - start) * 1000
                                    if status.get("error"):
                                        row["error"] = status["error"]
                                    return row
                                time.sleep(args.poll_ms / 1000)
                            row["status"] = "timeout"
                        except Exception as exc:
                            row["error"] = str(exc)
                        return row

                    with concurrent.futures.ThreadPoolExecutor(max_workers=args.concurrency) as pool:
                        warm = list(pool.map(job, range(args.warmup + args.history_jobs)))
                        if any(x["status"] != "succeeded" for x in warm):
                            raise RuntimeError("warmup/history failed")
                        before_calls = fixture.calls if fixture else None
                        before_connections = fixture.connections if fixture else None
                        start = time.perf_counter()
                        rows = list(pool.map(job, range(args.requests)))
                        elapsed = time.perf_counter() - start
                    success = [x for x in rows if x["status"] == "succeeded"]
                    counts = {state: sum(x["status"] == state for x in rows)
                              for state in ("succeeded", "failed", "timeout", "request_error")}
                    return {
                        "repeat": repeat, "elapsed_seconds": elapsed, "counts": counts,
                        "completed_docs_per_second": len(success) / elapsed,
                        "completed_pages_per_second": sum(x["pages"] for x in success) / elapsed,
                        "admission": distribution([x["admission_ms"] for x in rows if "admission_ms" in x]),
                        "completion": distribution([x["completion_ms"] for x in success]),
                        "process_peaks_including_warmup": peaks,
                        "synthetic_backend": None if fixture is None else {
                            "measured_requests": fixture.calls - before_calls,
                            "measured_new_connections": fixture.connections - before_connections,
                            "total_connections": fixture.connections, "peak_inflight": fixture.peak},
                        "jobs": rows}
                finally:
                    proc.terminate()
                    try:
                        proc.wait(timeout=30)
                    except subprocess.TimeoutExpired:
                        proc.kill(); proc.wait()
                    stop.set(); monitor.join()
    finally:
        if fixture:
            fixture.shutdown(); fixture.server_close()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--server-binary", required=True)
    p.add_argument("--output", required=True)
    p.add_argument("--config", help="Real model config; requires --input")
    p.add_argument("--input")
    for name, default in (("requests", 256), ("concurrency", 16), ("page-workers", 8),
                          ("box-workers", 1),
                          ("document-workers", 2), ("queued-pages", 8), ("model-instances", 8),
                          ("boxes", 8), ("warmup", 16), ("history-jobs", 0), ("repeats", 3)):
        p.add_argument("--" + name, type=int, default=default)
    p.add_argument("--model-delay-ms", type=float, default=2)
    p.add_argument("--connection-delay-ms", type=float, default=0)
    p.add_argument("--poll-ms", type=float, default=5)
    p.add_argument("--timeout", type=float, default=120)
    args = p.parse_args()
    if bool(args.config) != bool(args.input): p.error("--config and --input must be provided together")
    limits = {"requests": 100000, "concurrency": 48, "page_workers": 128, "box_workers": 128, "document_workers": 32,
              "queued_pages": 256, "model_instances": 128, "boxes": 128, "repeats": 100}
    for name, upper in limits.items():
        if not 1 <= getattr(args, name) <= upper: p.error(f"{name} must be 1..{upper}")
    if min(args.warmup, args.history_jobs, args.model_delay_ms, args.connection_delay_ms) < 0:
        p.error("warmup/history/delays must be nonnegative")
    if args.poll_ms <= 0 or args.timeout <= 0: p.error("poll interval and timeout must be positive")
    if args.requests + args.warmup + args.history_jobs > 100000: p.error("total jobs exceeds server maximum")
    binary = Path(args.server_binary)
    report = {"schema_version": 1, "mode": "configured" if args.config else "synthetic",
              "load_model": "closed-loop fixed client concurrency",
              "notes": "Completion includes queueing and polling delay; startup/warmup excluded from elapsed. "
                       "RSS/threads/fds sampled every 10ms for the server process only. "
                       "No OCR quality assertion. Synthetic delays are explicit, not device latency.",
              "environment": {"platform": platform.platform(), "cpu_count": os.cpu_count(),
                              "server_sha256": hashlib.sha256(binary.read_bytes()).hexdigest()},
              "settings": vars(args), "runs": []}
    if args.config:
        report["environment"]["config_sha256"] = hashlib.sha256(Path(args.config).read_bytes()).hexdigest()
        report["environment"]["input_sha256"] = hashlib.sha256(Path(args.input).read_bytes()).hexdigest()
    out = Path(args.output); out.parent.mkdir(parents=True, exist_ok=True)
    for repeat in range(args.repeats):
        run = run_once(args, repeat)
        report["runs"].append(run)
        out.write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps({k: v for k, v in run.items() if k != "jobs"}), flush=True)
    if any(r["counts"]["succeeded"] != args.requests for r in report["runs"]):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
