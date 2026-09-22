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
    print('PASS: native C++ ONNX layout, CTC recognition, relative paths and shape validation')


if __name__ == '__main__':
    run(str(Path(sys.argv[1]).resolve()))
