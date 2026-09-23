"""Generate tiny real ONNX graphs and execute the C++ ONNX Runtime path."""
import json
import subprocess
import sys
import tempfile
from pathlib import Path
import onnx
from onnx import TensorProto, helper


def graph(path, shape, data):
    output = helper.make_tensor('constant', TensorProto.FLOAT, shape, data)
    model = helper.make_model(helper.make_graph(
        [helper.make_node('Constant', [], ['output'], value=output)], 'fixture',
        [helper.make_tensor_value_info('image', TensorProto.FLOAT, [1, 3, 2, 2])],
        [helper.make_tensor_value_info('output', TensorProto.FLOAT, shape)]),
        opset_imports=[helper.make_opsetid('', 13)], ir_version=8)
    onnx.checker.check_model(model)
    onnx.save(model, path)


def batch_graph(path, batch):
    dimension = batch if batch else 'dynamic_batch'
    value = helper.make_tensor_value_info('logits', TensorProto.FLOAT, [dimension, 4, 3])
    output = helper.make_tensor_value_info('output', TensorProto.FLOAT, [dimension, 4, 3])
    model = helper.make_model(helper.make_graph(
        [helper.make_node('Identity', ['logits'], ['output'])], 'batch_fixture', [value], [output]),
        opset_imports=[helper.make_opsetid('', 13)], ir_version=8)
    onnx.checker.check_model(model)
    onnx.save(model, path)


def run(binary):
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        graph(root / 'layout.onnx', [1, 6], [0, .95, 0, 0, 20, 10])
        graph(root / 'rec.onnx', [1, 4, 3], [0,1,0, 0,1,0, 1,0,0, 0,0,1])
        (root / 'image.ppm').write_bytes(b'P6\n20 10\n255\n' + b'\xff' * 600)
        def local(path, decoder, instances=1):
            return {'backend': 'onnx', 'path': path, 'instances': instances,
                    'preprocess': {'width': 2, 'height': 2},
                    'inputs': [{'name': 'image', 'source': 'image'}], 'decoder': decoder}
        config = {
            'version': 1, 'layout': {'provider': 'paddle', 'model': 'layout'},
            'models': {'layout': local('layout.onnx', {'type': 'paddle_layout', 'labels': ['text']}),
                       'recognizer': local('rec.onnx', {'type': 'ctc', 'vocabulary': ['', 'A', '中']}, 2)},
            'routes': {'text': {'model': 'recognizer'}}}
        (root / 'config.json').write_text(json.dumps(config))
        # Different cwd verifies config-relative paths.
        subprocess.run([binary, '--config', str(root / 'config.json'), '--input', str(root / 'image.ppm'),
                        '--output', str(root / 'out')], cwd='/', check=True, timeout=30)
        result = json.loads((root / 'out/result.json').read_text())
        assert result['pages'][0]['blocks'][0]['text'] == 'A中'
        config['models']['recognizer']['preprocess']['width'] = 3
        (root / 'config.json').write_text(json.dumps(config))
        failure = subprocess.run([binary, '--config', str(root / 'config.json'), '--input', str(root / 'image.ppm'),
                                  '--output', str(root / 'bad')], capture_output=True, timeout=30)
        assert failure.returncode == 1 and b'shape mismatch' in failure.stderr
        logits = [0,1,0, 0,1,0, 1,0,0, 0,0,1]
        batch_graph(root/'dynamic.onnx', 0)
        batch_graph(root/'static4.onnx', 4)
        boxes = [{'type': 'text', 'bbox': [0,0,1,1]} for _ in range(4)]
        batched = {'version': 1, 'execution': {'workers': 4},
            'layout': {'provider': 'normalized', 'model': 'layout', 'coordinates': 'normalized'},
            'models': {'layout': {'backend': 'mock', 'response': {'boxes': boxes}},
                'ocr': local('dynamic.onnx', {'type': 'ctc', 'vocabulary': ['', 'A', '中']})},
            'routes': {'text': {'model': 'ocr'}}}
        batched['models']['ocr']['inputs'] = [{'name': 'logits', 'source': 'constant',
            'shape': [1,4,3], 'data': logits}]
        batched['models']['ocr'].update(batch_size=4, max_batch_wait_ms=100)
        (root/'config.json').write_text(json.dumps(batched))
        subprocess.run([binary, '--config', str(root/'config.json'), '--input', str(root/'image.ppm'),
                        '--output', str(root/'dynamic')], check=True, timeout=30)
        result = json.loads((root/'dynamic/result.json').read_text())
        assert [block['text'] for block in result['pages'][0]['blocks']] == ['A中'] * 4
        # A static exported batch of four must also accept a one-item tail.
        batched['models']['ocr']['path'] = 'static4.onnx'
        batched['models']['layout']['response']['boxes'] = boxes[:1]
        (root/'config.json').write_text(json.dumps(batched))
        subprocess.run([binary, '--config', str(root/'config.json'), '--input', str(root/'image.ppm'),
                        '--output', str(root/'static')], check=True, timeout=30)
        result = json.loads((root/'static/result.json').read_text())
        assert result['pages'][0]['blocks'][0]['text'] == 'A中'
        batched['models']['ocr']['path'] = 'rec.onnx'
        (root/'config.json').write_text(json.dumps(batched))
        failure = subprocess.run([binary, '--config', str(root/'config.json'), '--validate'],
                                 capture_output=True, timeout=30)
        # Validation checks JSON; native shape compatibility is checked at model construction.
        assert failure.returncode == 0
        failure = subprocess.run([binary, '--config', str(root/'config.json'), '--input', str(root/'image.ppm'),
                                  '--output', str(root/'bad-batch')], capture_output=True, timeout=30)
        assert failure.returncode == 1 and b'static input batch dimension' in failure.stderr
    print('PASS: native ONNX layout/CTC, full dynamic batch, static padded tail and shape validation')


if __name__ == '__main__':
    run(str(Path(sys.argv[1]).resolve()))
