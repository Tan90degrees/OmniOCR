# 测试与验证

[项目首页](../README.md) · [文档目录](README.md)

本页列出仓库已有测试及运行方式。功能测试、真实模型准确率和目标硬件性能分别验收；Mock 或生成的小型 ONNX 图通过，不代表真实 OCR 模型已达标。

## 已有测试

| 入口 | 覆盖内容 | 条件与边界 |
|---|---|---|
| `ctest` | 核心逻辑、实例池、HTTP 协议/连接复用、fallback、输出解码回归 | HTTP 测试使用本地模拟服务；需 Python 3 才会注册 Python 集成项 |
| [document_integration.py](../tests/document_integration.py) | 六种 Office 格式、12 页 PDF、页序和转换失败 | 真实 LibreOffice/Poppler，Mock 模型 |
| [input_formats_integration.py](../tests/input_formats_integration.py) | RTF、ODF、HTML、CSV、TIFF、EPUB/OFD、限制与混合批次 | `--external` 要求真实 Calibre/OFD；Mock 模型 |
| [onnx_integration.py](../tests/onnx_integration.py) | ONNX Runtime 执行、张量形状、CTC 等适配 | 实际执行生成的小图，不评估 OCR 精度 |
| [batch_integration.py](../tests/batch_integration.py) | 页调度、优先级、并发约束和任务隔离 | 模拟后端 |
| [server_performance_integration.py](../tests/server_performance_integration.py) | 等待文档优先级/FIFO、大文件并发下载、断开后 FD 回收 | 模拟推理，验证调度与资源行为 |
| [server_admission_integration.py](../tests/server_admission_integration.py) | 活跃任务/上传字节准入、断线归还、页面字节背压、超大页失败与计数核对 | 模拟推理，验证过载边界和配额恢复 |
| [server_integration.py](../tests/server_integration.py) | 路径/上传、鉴权、状态、结果、并发与格式 | `--formats` 增加格式用例，模型使用模拟服务 |

CI 定义见 [Linux / ONNX 工作流](../.github/workflows/ci.yml) 和 [ARM64 出包工作流](../.github/workflows/package-arm64.yml)。已发生的实机验证与环境信息统一记录在 [昇腾部署](ascend.md)。

## 本地运行

先按 [快速上手](getting-started.md) 构建 CLI/REST，并按 [输入格式](input-formats.md) 安装转换器、构建 OFD JAR。以下命令在仓库根目录运行；Python 依赖建议安装在测试虚拟环境中：

```bash
python3 -m pip install pillow python-docx python-pptx openpyxl reportlab
ctest --test-dir build --output-on-failure
python3 tests/document_integration.py build/omniocr
python3 tests/input_formats_integration.py build/omniocr \
  --external --ofd-converter tools/omniocr-ofd-to-pdf
python3 tests/batch_integration.py build/omniocr
python3 tests/server_integration.py build/omniocr-server --formats
python3 tests/server_performance_integration.py build/omniocr-server
python3 tests/server_admission_integration.py build/omniocr-server
```

`--external` 缺少依赖时会失败，不会将跳过记成通过。ONNX 测试单独使用启用了 ONNX 的二进制，例如：

```bash
python3 -m pip install onnx
cmake -S . -B build-onnx -DOMNIOCR_WITH_ONNX=ON -DONNXRUNTIME_ROOT=/opt/onnxruntime
cmake --build build-onnx -j4
python3 tests/onnx_integration.py build-onnx/omniocr
```

将 SDK 路径替换为本机路径。修改配置示例后，还应逐个检查结构与引用：

```bash
for config in configs/*.json; do
  ./build/omniocr --config "$config" --validate || exit 1
done
```

## 实机与发布验收

用户已报告 310P3 上 DocLayout OM + OvisOCR2 的真实 OCR 链路及服务模式跑通；这不等于其他权重、设备、驱动/CANN 组合已经验收。ACL 退出 SIGSEGV 由 [Issue #2](https://github.com/Tan90degrees/OmniOCR/issues/2) 跟踪，正常退出与资源回收仍需验证。

发布前至少补齐：

- 每个声明支持的文件格式与模型部署组合，在实际环境中执行并保留结果。
- 按正文、布局、阅读顺序、表格、公式分别评价准确率。
- 多实例、跨 BOX 共享、同卡/多卡、推理失败后的租约与资源释放。
- 冷启动、端到端 P50/P95/P99、完成吞吐、错误率和资源峰值。
- 长文档与 24/72 小时持续负载、退出/重启循环和过载恢复。

统计 REST 吞吐应以任务完成为准，HTTP 202 仅表示接收；长测须考虑 `max-jobs` 是累计任务上限。记录 Git SHA、数据与模型哈希、配置、依赖、硬件和原始计时，缺失条件标为未验证，不能标为通过。当前仓库未提供完整准确率评估和全组合性能基准工具；固定并发 REST 基准及其适用范围见 [并发与性能](performance.md)。

## 相关文档

[输入格式与真实转换器](input-formats.md) · [服务容量限制](server.md) · [调度与资源](architecture.md) · [昇腾实机记录](ascend.md)
