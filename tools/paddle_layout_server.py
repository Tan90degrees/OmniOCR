"""Optional PaddleOCR service bridge; OmniOCR's pipeline and inference clients are C++.

Install a PaddlePaddle build appropriate for the target device, then paddleocr, Pillow, numpy.
Run: python tools/paddle_layout_server.py --model PP-DocLayoutV2 --device cpu
This serialized server hosts one model instance. Deploy separate processes for more instances.
"""
import argparse
import base64
import io
import json
from http.server import BaseHTTPRequestHandler, HTTPServer


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--model', default='PP-DocLayoutV2')
    parser.add_argument('--device', default='cpu')
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=8001)
    args = parser.parse_args()
    import numpy as np
    from PIL import Image
    from paddleocr import LayoutDetection
    model = LayoutDetection(model_name=args.model, device=args.device)

    class Handler(BaseHTTPRequestHandler):
        def setup(self):
            super().setup()
            self.connection.settimeout(120)

        def do_POST(self):
            if self.path != '/layout':
                self.send_error(404)
                return
            try:
                length = int(self.headers.get('Content-Length', '0'))
                if not 0 < length <= 64 * 1024 * 1024:
                    self.send_error(413)
                    return
                request = json.loads(self.rfile.read(length))
                prefix, encoded = request['image'].split(',', 1)
                if prefix != 'data:image/png;base64':
                    raise ValueError('expected PNG data URL')
                with Image.open(io.BytesIO(base64.b64decode(encoded, validate=True))) as image:
                    if image.width * image.height > 40_000_000:
                        raise ValueError('image exceeds 40MP')
                    # Paddle's ndarray input convention is BGR.
                    pixels = np.asarray(image.convert('RGB'))[:, :, ::-1].copy()
                prediction = next(iter(model.predict(pixels, batch_size=1)))
                result = prediction.json
                if isinstance(result, str):
                    result = json.loads(result)
                boxes = result.get('res', result)['boxes']
                payload = json.dumps({'boxes': boxes}, ensure_ascii=False).encode()
                self.send_response(200)
                self.send_header('Content-Type', 'application/json; charset=utf-8')
                self.send_header('Content-Length', str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
            except (ValueError, KeyError) as exc:
                self.log_error('invalid request/result: %s', exc)
                self.send_error(400)
            except Exception as exc:
                self.log_error('layout inference failed: %s', exc)
                self.send_error(500)

    HTTPServer((args.host, args.port), Handler).serve_forever()


if __name__ == '__main__':
    main()
