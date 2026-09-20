# 昇腾部署与验证

## 组件部署

推荐先用远端 VLM 完成真实文档基线，再逐个替换适合本地执行的小模型：

1. C++ 进程负责文件转换、布局结果标准化、BOX 路由与结果输出。
2. vLLM-Ascend 进程提供 MinerU/PaddleOCR-VL 服务；客户端通过 OpenAI 兼容接口访问。
3. 需要降低小模型服务开销时，将已验证的版面或文字识别模型导出 ONNX / 编译为 OM，通过对应 C++ TensorEngine 执行。

MinerU2.5-Pro 权重可参考 [官方模型卡](https://huggingface.co/opendatalab/MinerU2.5-Pro-2605-1.2B)，PaddleOCR-VL 的 Ascend 部署参考 [vLLM-Ascend 官方教程](https://docs.vllm.ai/projects/ascend/en/latest/tutorials/PaddleOCR-VL.html)。权重能由上游 vLLM 加载，不等于所有 vLLM-Ascend/CANN/驱动组合都已支持；以目标环境的支持矩阵与实测为准。

服务命令的最小形式（设备和图模式参数按已验证的镜像调整）：

```bash
# 在匹配驱动、CANN、torch_npu、vllm-ascend 的环境中运行
vllm serve /models/MinerU2.5-Pro-2605-1.2B \
  --served-model-name mineru --host 127.0.0.1 --port 8000
```

客户端配置使用 `configs/mineru-vllm.json`。不同模型通常各自运行服务进程；框架的 `instances` 不会启动这些进程。

## OM 适配

使用与你设备匹配的 ATC/CANN 工具链，把已验证的 ONNX 图编译为静态 OM。示意命令：

```bash
# 输入名/shape 和 soc_version 必须替换为真实模型与芯片；不要原样复制占位值
atc --framework=5 --model=MODEL.onnx --output=MODEL \
  --input_format=NCHW --input_shape='INPUT_NAME:1,3,HEIGHT,WIDTH' \
  --soc_version=YOUR_ASCEND_SOC
```

编译后逐项核对：输入名、dtype、shape、RGB/BGR、归一化、是否已经包含前处理、输出索引和 shape、坐标尺度、字典。内置适配只接受 float32 输入；如果导出是 FP16/INT8 或带 AIPP，需要增加对应适配，不能按字节强行拷贝。

模型池可配置 `instances: 2, device_ids: [0,1]`，分别在两张设备上创建实例；`instances: 4, device_ids: [0,1]` 则每张卡两个实例，权重与中间缓冲可能重复占用内存。

## 验证状态

| 验证项 | 当前结果 |
|---|---|
| 默认 C++ 构建与核心测试 | 已通过 |
| Paddle/MinerU HTTP 请求协议、裁剪与输出 | 使用本地模拟服务，已通过 |
| Word/PPT/Excel 与 PDF 的实际转换 | 使用 LibreOffice/Poppler，已通过 |
| ONNX C++ 编译和真实推理 | 已通过，测试使用生成的小图，不代表真实 OCR 精度 |
| ACL 编译、模型加载、NPU 推理、显存回收 | 当前环境无 CANN SDK/NPU，待目标环境验证 |
| 真实 PaddleLayout/MinerU2.5-Pro 权重与精度 | 待连接部署后的真实模型验证 |

上线前应在实际机器补齐：同模型多实例、跨 BOX 共享、异常后释放、长 PDF 内存稳定性、表格/公式质量、并发吞吐和尾延迟。没有通过这些测量之前不承诺具体吞吐、显存占用或工业级识别精度。
