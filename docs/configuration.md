# 配置与模型适配

[项目首页](../README.md) · [文档目录](README.md)

配置采用 JSON，支持兼容的 `version: 1` 和新增 `version: 2`（执行池、模型绑定、pipeline 分离）。v2 示例及 PP-DocLayoutV3 多边形/可选阅读顺序、外部 C ABI 插件见 [插件与 V3 专题](plugins.md)。其余字段说明中的旧式布局依然适用于 v1。模型路径、字典路径相对于配置文件；输入和输出路径相对于调用者工作目录。

## 顶层配置

| 字段 | 默认值 | 含义 |
|---|---|---|
| `execution.workers` | 4 | 每次文档处理的 BOX 线程上限，范围 1–128 |
| `execution.on_error` | `fail` | `fail` 或 `record`，仅控制 BOX 错误 |
| `document.dpi` | 150 | PDF 渲染分辨率请求；像素上限会约束最终分辨率 |
| `document.max_pixels` | 40000000 | 图片最大像素数；PDF 最大边长设为其平方根 |
| `document.max_pages` | 1000 | 超过则整份拒绝，不静默截断 |
| `document.timeout_seconds` | 120 | 每次转换/渲染命令的超时 |
| `document.soffice/pdfinfo/pdftoppm` | 对应命令名 | 可指定可执行文件绝对路径 |
| `document.ebook_convert` | `ebook-convert` | EPUB → PDF，可执行文件路径（不是 shell 命令） |
| `document.ofd_converter` | `omniocr-ofd-to-pdf` | OFD → PDF，见 [输入格式](input-formats.md) |
| `document.csv_delimiter` | `,` | CSV 分隔符：逗号、分号、tab 或竖线 |
| `document.max_csv_bytes` | 16777216 | CSV 解析前的输入字节上限 |
| `layout.provider` | 必填 | `paddle`、`mineru`、`normalized` 或注册的 `paddle.doclayout_v3.http`/其他布局插件 |
| `layout.model` | 必填 | 模型池 ID |
| `layout.type_map` | 空 | 原始 label → 业务 BOX 类型 |
| `layout.coordinates` | `pixel` | JSON 布局为 `pixel` 或 `normalized`（0–1） |
| `layout.image_size` | 不缩放 | MinerU 请求图像 `[宽,高]`，示例为 1036×1036 |
| `layout.max_boxes` | 2000 | 单页 BOX 数上限 |
| `layout.score_threshold` | 0 | 置信度过滤阈值 |

`routes` 的键是经过 type_map 的类型名；`*` 是兜底。未命中且无兜底时按 on_error 处理。每个 route 的 `action` 默认为 `recognize`，须在 `model`（单个模型 ID）和 `models`（非空、不可重复的模型 ID 数组）中**二选一**；可设置 route 级 `prompt` 和 `save_crop`。`image` 只保存裁剪，`skip` 跳过识别但仍在 JSON 保留 BOX。

`models` 按给定顺序尝试推理；仅在模型推理/结果格式异常时切换到下一个模型，成功后记录实际使用的模型 ID。所有候选都失败时，`on_error: record` 保留 BOX 和包含候选模型错误的 `error`；`on_error: fail` 抛错。同一模型 ID 无论在多少种 BOX 的候选列表中引用，都复用同一个有界实例池；候选模型池会在 Pipeline 初始化时全部创建，因此配置本地 OM 模型时要预留各候选实例的设备内存。建议将该机制用于主备模型容错，而不是将任意错误隐式掩盖。

```json
"routes": {
  "text": {"models": ["primary_vllm", "backup_onnx"], "prompt": "Text Recognition:"},
  "title": {"model": "primary_vllm"},
  "image": {"action": "image"}
}
```

## 模型池

`instances` 默认 1，范围 1–128；`acquire_timeout_ms` 默认 60000。实例只被一个调用独占使用。总模型内存近似为各本地模型的 `instances × 单实例内存` 之和，不能把增大实例数当成免费并发。对 ACL，`device_ids: [0,1]` 按实例序号轮转分配设备。

vLLM/HTTP 的 `instances` 是请求槽位，不能创建远端模型副本；同一个 endpoint 后面的实际模型数由服务部署控制。连接不同部署可定义多个模型 ID 或使用服务端负载均衡地址。

## vLLM

`backend: vllm`，`endpoint` 填完整 `/v1/chat/completions` URL，`model` 必须匹配 served model name。可用 `api_key_env` 指定密钥环境变量，不把密钥写进配置。

`parameters` 可指定 `temperature`、`top_p`、`max_tokens` 及目标 vLLM 支持的扩展采样字段。框架固定 `stream=false`，总是发送 PNG data URL；模型名和 messages 不允许被 parameters 覆盖。`finish_reason=length` 按截断错误处理。

`connect_timeout_seconds` 默认 10、`timeout_seconds` 默认 120、`max_response_bytes` 默认 16 MiB。TLS 校验保持开启；不自动跟随重定向。

MinerU 的版面需要保留特殊 token，示例设置 `skip_special_tokens=false`。服务端若过滤 `<|box_start|>` 等 token，解析会失败。接口依据 [官方 MinerU 客户端](https://github.com/opendatalab/mineru-vl-utils/blob/405520b8fe6be61725537984eb4a00d404dc272a/mineru_vl_utils/mineru_client.py)；当前采用双线性缩放，未复刻官方所有裁剪增强、重复抑制、嵌套块去重和跨页后处理，不能推断具有相同评测精度。

## Paddle HTTP 桥接协议

`backend: http_json` 发出以下自定义协议；它不是声称所有 Paddle 部署原生使用此端点：

```json
{"image":"data:image/png;base64,...","width":1000,"height":1400,"prompt":""}
```

Paddle 布局返回：

```json
{"boxes":[{"label":"text","coordinate":[10,20,500,100],"score":0.99,"order":0}]}
```

也支持 `{"res":{"boxes":[...]}}`。字段对齐 [PaddleOCR 布局结果](https://www.paddleocr.ai/latest/en/version3.x/module_usage/layout_detection.html)。`tools/paddle_layout_server.py` 提供可选桥接。V2 的排序输出须由实际 Paddle pipeline 保留；框架不凭检测类别推断顺序。

自定义服务也可返回 `normalized` 布局：`{"boxes":[{"type":"text","bbox":[...],"order":0}]}`。识别服务返回 `{"text":"..."}`。

### 启动 Paddle 桥接服务

在仓库根目录启动桥接服务，再在另一个终端运行 CLI：

```bash
# 先安装适合当前设备的 PaddlePaddle，再安装 paddleocr、Pillow、numpy
python tools/paddle_layout_server.py --model PP-DocLayoutV2 --device cpu --port 8001
./build/omniocr --config configs/paddle-http-vllm.json --input scan.png --output out/paddle
```

桥接程序只承载 Paddle 模型，主流水线、调度、裁剪、本地推理与输出均为 C++。桥接服务默认监听 localhost，串行承载一个模型实例；真实 Paddle 权重未在当前开发环境下载验证。

## OM/ONNX 本地模型

`backend` 为 `acl` 或 `onnx`。输入和输出名称、shape、dtype、前处理、类别表、字典必须从你实际导出的模型确认。框架只内置以下适配，其他模型通过 `Model`/`TensorEngine` 扩展。

### 输入

`preprocess` 必须包含固定 `width/height`。当前把 RGB 图像双线性拉伸到该尺寸，布局为 NCHW，默认 `scale=1/255`、`mean=[0,0,0]`、`std=[1,1,1]`，计算 `(pixel * scale - mean) / std`。`color` 可选 `rgb/bgr`。不隐式 letterbox、自动 AIPP 或处理动态多档 shape。

`inputs` 按模型名称匹配：

| source | float32 张量 |
|---|---|
| `image` | `[1,3,H,W]` |
| `original_shape` | `[1,2]`，原图 `[H,W]` |
| `scale_factor` | `[1,2]`，`[目标H/原H,目标W/原W]` |
| `constant` | 显式 `shape` 和 `data` |

例如某模型 `im_shape` 期望的是 resize 后的形状，应使用 constant `[H,W]`；不要仅凭输入名字误选 original_shape。

### 输出

`decoder.type: paddle_layout` 选择 float32 `[N,6]` 输出，每行 `[class_id,score,x1,y1,x2,y2]`；`output_index` 默认 0。`labels` 必须逐项对应模型类别；`coordinates` 为 `pixel` 表示原图坐标，为 `input` 表示 resize 后坐标。该适配要求模型已输出检测框，**不包含通用 RT-DETR 原始 logits/bbox 解码、NMS、V2 指针排序网络**。

`decoder.type: ctc` 选择 `[1,T,C]` float32 logits/概率。提供 `vocabulary` 数组或 UTF-8 `dictionary` 文件（每行一个 token），词表必须包括 blank 位置且条目数等于 C；`blank_index` 默认 0。例如首行空行代表 blank，不自动添加空格类。解码执行 argmax、去重复和去 blank。

CTC 通常是文字行识别，不能把多行段落 BOX 直接缩成一行就当成完整 OCR。段落宜路由到 VLM，或为指定模型增加“文本行检测 → 行识别 → 汇总”的适配器。示例 `paddle-local.json` 用来展示混合后端路由，实际投入使用前要确认你的 BOX 与模型输入语义一致。

不支持的 dtype、shape、词表或输出格式会明确失败，不回退到 mock。非 float32 辅助输出保留输出序号但不可作为识别输出解码。

`output.schema_version` 与配置版本无关，可显式选择 `1`（矩形兼容、有损）或 `2`（轮廓、可选阅读顺序、来源和原始索引）；不设置时旧结果维持 v1、V3 轮廓结果自动采用 v2。具体字段和映射规则见 [插件与 V3](plugins.md)。

## JSON 结果

```json
{
  "schema_version": 1,
  "coordinate_system": "page_pixels_xyxy",
  "source": "/data/example.pdf",
  "pages": [{
    "page": 1, "width": 1000, "height": 1400,
    "blocks": [{
      "id": "p1-b0", "type": "text", "raw_type": "text",
      "bbox": [10,20,500,100], "order": 0, "rotation": 0, "score": 0.99,
      "model": "shared_ocr", "text": "识别结果", "raw_text": "", "asset": "", "error": ""
    }]
  }]
}
```

表格的 `raw_text` 保存模型原文，`text` 保存转换后的 HTML（OTSL）或原 Markdown/HTML。异常块保留 BOX 与 error，文档页顺序和模型阅读顺序不随推理完成先后改变。

## 相关文档

[快速上手](getting-started.md) · [输入格式](input-formats.md) · [昇腾部署](ascend.md)
