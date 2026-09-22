# 快速上手

[项目首页](../README.md) · [文档目录](README.md)

先用 Mock 配置确认程序和输入链路可用，再接入真实模型。以下命令除克隆步骤外，均在仓库根目录执行。

## 构建基础 CLI

支持 Linux x86_64 / aarch64，需要 C++17 编译器、CMake ≥ 3.20。Ubuntu 示例：

```bash
git clone https://github.com/Tan90degrees/OmniOCR.git
cd OmniOCR
sudo apt-get update
sudo apt-get install -y g++ cmake git libcurl4-openssl-dev libtiff-dev nlohmann-json3-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

默认启用 CLI 和测试，关闭 ACL、ONNX、REST 服务；vLLM/HTTP 客户端可直接使用。CMake 优先使用已安装的 JSON 依赖，stb 使用固定提交；依赖未提供时需要联网下载。

图片演示无需 PDF/Office 转换器。处理 PDF、Office 或其他格式时，按 [输入格式](input-formats.md) 安装对应依赖；例如 Ubuntu 的 PDF/Office：

```bash
sudo apt-get install -y poppler-utils libreoffice
```

### 离线依赖

提前准备依赖和工具链；以下选项用于避免 CMake 下载 JSON/stb，不代表已打包所有系统库：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DSTB_INCLUDE_DIR=/opt/deps/stb \
  -DNLOHMANN_JSON_INCLUDE_DIR=/opt/deps/json/include
cmake --build build -j4
```

`STB_INCLUDE_DIR` 中需有 `stb_image.h` / `stb_image_write.h`，JSON 目录中需有 `nlohmann/json.hpp`；仍需 libcurl、libtiff 等开发库。预构建分发见 [ARM64 离线包](../packaging/README.md)。

### 启用本地模型

ACL 和 ONNX 可独立启用；以下示例同时构建两种后端：

```bash
cmake -S . -B build-local -DCMAKE_BUILD_TYPE=Release \
  -DOMNIOCR_WITH_ACL=ON -DASCEND_HOME=/usr/local/Ascend/ascend-toolkit/latest \
  -DOMNIOCR_WITH_ONNX=ON -DONNXRUNTIME_ROOT=/opt/onnxruntime
cmake --build build-local -j4
```

`ASCEND_HOME` 下需有 `include/acl/acl.h` 和 `lib64/libascendcl.so`；`ONNXRUNTIME_ROOT` 下需有 `include/onnxruntime_cxx_api.h` 和 `lib/libonnxruntime.so`。SDK 必须匹配主机架构，运行时需能找到对应动态库。ONNX 当前使用 CPU EP，ACL 的输入与设备要求见 [模型适配](configuration.md) 和 [昇腾部署](ascend.md)。

## 第一个任务

可以直接使用自己的图片。也可以用 Python 3 标准库生成一张白色 PPM，验证解码、路由与输出，不需要下载测试文件或模型：

```bash
mkdir -p out
python3 - <<'PY'
from pathlib import Path
Path('out/example.ppm').write_bytes(b'P6\n200 120\n255\n' + bytes([255, 255, 255]) * 200 * 120)
PY
./build/omniocr --config configs/demo.json --input out/example.ppm --output out/demo --format both
```

查看 `out/demo/result.md`、`out/demo/result.json`，以及 `assets/` 中的图片裁剪。`demo.json` 返回固定布局和 `[MOCK]` 文本，即使白色图片也会输出模拟内容；这不是 OCR 准确率验证。

输出目录必须不存在或为空，重复运行时请使用新的输出目录。接入真实模型时，按 [首页配置选择表](../README.md#接入真实模型) 修改相应模板，再运行：

```bash
./build/omniocr --config configs/mineru-vllm.json --validate
./build/omniocr --config configs/mineru-vllm.json --input /path/to/document.pdf --output out/real
```

将输入路径替换为自己的文件，并先启动对应模型服务。`--validate` 只检查结构与引用关系，不加载权重、不探测服务；通过后仍需实际推理验证。

## CLI 参数与输出

| 参数 | 用途 |
|---|---|
| `--config FILE` | JSON 配置，模型与字典路径相对于配置文件 |
| `--input FILE` / `--output DIR` | 单文件输入与结果目录，相对于当前工作目录 |
| `--batch FILE` | 批处理任务清单，与 `--input/--output` 互斥；见 [批处理](batch.md) |
| `--format both\|json\|markdown` | 默认 `both` |
| `--validate` | 仅校验配置 |
| `--list-formats` | 列出支持后缀，不检测外部转换器 |
| `--help` | 查看完整命令帮助 |

JSON 的坐标、页码、BOX 和错误字段见 [JSON 结果](configuration.md#json-结果)。Markdown 支持模型输出的文本、表格与图片引用；当前不做跨页表格/段落合并。

退出码：成功为 `0`，致命错误为 `1`，`on_error: record` 下保留 BOX 部分结果为 `2`。转换/布局失败始终属于致命错误。失败可能留下裁剪文件，重试请使用新的输出目录。

## 启动 REST 演示

先完成上述构建与 PPM 生成。Ubuntu 需要额外安装 libmicrohttpd 开发包：

```bash
sudo apt-get install -y libmicrohttpd-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DOMNIOCR_WITH_SERVER=ON
cmake --build build -j4
./build/omniocr-server --config configs/demo.json \
  --data-dir "$PWD/out/rest-state" --host 127.0.0.1 --port 8080
```

保持服务运行，在另一个终端进入同一仓库目录，上传刚才的图片：

```bash
curl -X POST 'http://127.0.0.1:8080/v1/jobs/upload?extension=.ppm&priority=100' \
  -H 'Content-Type: application/octet-stream' --data-binary @out/example.ppm
```

返回 HTTP 202 和任务 `id`。将返回值填到下面变量，先查看状态；直到 `status` 为 `succeeded` 后再获取结果：

```bash
JOB_ID='替换为返回的任务id'
curl "http://127.0.0.1:8080/v1/jobs/$JOB_ID"
curl "http://127.0.0.1:8080/v1/jobs/$JOB_ID/result?format=json"
curl "http://127.0.0.1:8080/v1/jobs/$JOB_ID/result?format=markdown"
```

这是仅监听本机的 Mock 演示。上传的是原始文件字节，不是 multipart。实际部署需要模型配置、鉴权与容量设置；路径提交还需 `--allowed-input-root`。完整步骤见 [REST API](server.md)。

## 接下来

[配置真实模型与 BOX 路由](configuration.md) · [安装格式转换器](input-formats.md) · [提交批任务](batch.md) · [运行测试](testing.md)
