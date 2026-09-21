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
| ACL 编译、OM 模型加载、NPU 推理 | 用户在 EulerOS 2.0 SP13 / Ascend 310P3 上报告 DocLayout_21label_infer OM 实机通过；详见下方验证记录 |
| 真实模型 OCR（DocLayout + OvisOCR2） | 用户报告端到端生成真实 OCR 结果；本记录不等于 MinerU/Paddle 权重已验证或精度评测完成 |
| ACL 清理、显存回收及正常退出 | 用户报告结果写出后进程 SIGSEGV（退出码 139），待定位与修复 |

上线前应在实际机器补齐：同模型多实例、跨 BOX 共享、异常后释放、长 PDF 内存稳定性、表格/公式质量、并发吞吐和尾延迟。没有通过这些测量之前不承诺具体吞吐、显存占用或工业级识别精度。

## 用户提供的 310P3 实机验证记录（2026-09-21）

本节是用户提交的验证报告，非仓库 CI / 维护者在设备上的独立复验；不能代替正常退出、长期稳定性和其他硬件/模型组合的验收。

- 系统：EulerOS 2.0 SP13，aarch64，4 × Ascend 310P3（每卡 44 GB）；报告列出的驱动/CANN 25.2.0、toolkit 8.2.RC1。
- vLLM 镜像：`quay.io/ascend/vllm-ascend:v0.23.0rc1-310p-openeuler`，该次运行使用 `--privileged --runtime ascend`；这是所测环境的部署条件，不是对所有服务器的通用权限建议。
- 构建：`-DOMNIOCR_WITH_ACL=ON`，采用该环境本地 nlohmann_json/stb；编译成功、CTest 3/3 通过（core、http_integration、fallback_http_integration）。
- 场景 A（ACL + mock）：`DocLayout_21label_infer.om` 在 NPU 0 加载执行，`paddle_layout` 解码 `[300,6]` 输出，报告共 7 个检测框（text 3 个、chart、figure_title、paragraph_title、number 各 1 个），置信度 0.69–0.98。
- 场景 B（ACL + vLLM）：DocLayout 检测 → BOX 裁剪 → OvisOCR2（Qwen3.5 VL）识别。vLLM 的 Chat Completions 服务使用端口 18070；报告称已从数学教材页提取正文、标题和 LaTeX 公式，并生成 `result.md` 和 `result.json`。这是单次真实样本的链路验证，不代表准确率、复杂页布局或并发吞吐达标。

### DocLayout 的 scale_factor 输入语义

用户报告该 `DocLayout_21label_infer.om` 的输入 `scale_factor` 需要固定 `[1.0, 1.0]`，而内置 `source: scale_factor` 表示预处理尺寸与原图尺寸之比（`target/original`），语义与这份 OM 的输入不匹配。针对**已验证的这份模型**，应在其输入配置中使用：

```json
{"name": "scale_factor", "source": "constant", "shape": [1, 2], "data": [1.0, 1.0]}
```

请以实际 OM 输入签名和导出时的预处理协议核对 `name`、`shape`、`dtype` 和坐标空间；不能把固定比例值推广到所有 DocLayout/Paddle OM。用户的 `configs/acl_local_layout.json`、`configs/acl_vllm_ocr.json` 为实机环境使用的文件，本仓库尚未收录，不假定其中的本地模型路径或服务地址可以复用。

### 尚未关闭：退出阶段的 SIGSEGV

报告记录了结果文件正常生成，但 OmniOCR 退出码为 **139（SIGSEGV）**，同时发现 `libunified_dlog.so` 缺失。**结果写出成功 != 进程正常退出**。缺失库可能与该故障有关，但尚无堆栈可以证明崩溃源仅在 CANN SDK，也未排除本项目的 ACL 释放顺序及动态库/环境兼容问题。

排查时请保持相同的容器、驱动、CANN 和模型配置，先复现并获取堆栈：

```bash
./build/omniocr --config configs/acl_vllm_ocr.json --input /path/to/input.png --output /tmp/ocr-debug
printf 'exit=%d\\n' "$?"
# 复现请改用新的空输出目录
# gdb --args ./build/omniocr --config configs/acl_vllm_ocr.json --input /path/to/input.png --output /tmp/ocr-gdb
# 在 gdb 内：run；崩溃后执行 bt full、thread apply all bt、info sharedlibrary
find /usr/local/Ascend -name 'libunified_dlog.so*' 2>/dev/null
```

需根据堆栈区分是否发生在 `AclEngine::clear()` 的 dataset/buffer/desc/model/context 释放，还是静态 `Runtime` 的 `aclrtResetDevice()` / `aclFinalize()`，或其他动态库卸载阶段。进一步用 ACL+mock、纯 HTTP/mock 两组流程对照，检查镜像内 CANN 运行库、驱动挂载、共享库搜索路径与权限。未确认正常退出及重复运行资源释放前，本验收项保持未通过。
