# 配置与模型适配

[项目首页](../README.md) · [文档目录](README.md)

配置采用 JSON，支持兼容的 `version: 1` 和新增 `version: 2`（执行池、模型绑定、pipeline 分离）。v2 示例及 PP-DocLayoutV3 多边形/可选阅读顺序、外部 C ABI 插件见 [插件与 V3 专题](plugins.md)。其余字段说明中的旧式布局依然适用于 v1。模型、字典、`server.data_dir` 和 `server.allowed_input_root` 的相对路径以配置文件为基准；待处理文件和输出目录仍以调用者工作目录为基准。

## 顶层配置

| 字段 | 默认值 | 含义 |
|---|---|---|
| `execution.workers` | 4 | 单文件 CLI 的 BOX 线程数；也是批处理/REST 的页面线程数回退值，范围 1–128 |
| `execution.page_workers` | `execution.workers` | REST 和批处理共享的页面线程数，范围 1–128 |
| `execution.box_workers` | 1 | REST 和批处理共享的 BOX 线程数，范围 1–128 |
| `execution.document_workers` | 2 | 同时读取/转换的文档数，范围 1–32 |
| `execution.max_queued_pages` | 2 | 待处理页面队列长度，范围 1–256 |
| `execution.on_error` | `fail` | `fail` 或 `record`，仅控制 BOX 错误 |
| `document.dpi` | 150 | PDF 渲染分辨率请求；像素上限会约束最终分辨率 |
| `document.max_pixels` | 40000000 | PDF/转换文档的页面像素上限；显式设置时也作为图片文件的兼容默认值 |
| `document.image_limits.max_pixels` | 100000000 | PNG/JPEG/BMP/PPM/PGM/TGA 和 TIFF 每页的像素上限，范围 1–200000000；优先于显式 `document.max_pixels` |
| `document.image_limits.max_width/max_height` | 不限制 | 图片文件宽/高上限，各可配置 1–100000 |
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
| `postprocess` | 全部关闭 | 可选的 BOX 后处理与跨页表格合并，见下节 |

服务专属的 `server` 配置可设置 `data_dir`、`allowed_input_root`、`host`、`port`、`api_key_env`、`max_upload_bytes`、`max_jobs`、`max_active_jobs`、`max_inflight_upload_bytes`、`max_queued_page_bytes`、`http_connections`、`connection_timeout_seconds`、`max_result_bytes` 和 `max_asset_bytes`。范围、启动样例和安全边界见[REST 服务](server.md#构建及启动)。批处理清单的 `options` 可覆盖 `execution` 中对应的调度设置；服务启动参数可覆盖配置文件。三种模式均复用模型池参数和 BOX 路由，无需改代码。未识别的 `execution`/`server` 键或越界值会在启动前报错。配置在进程启动时读取，修改工作线程数或模型实例数后须重启服务。

`document.image_limits` 在解码图片文件前检查原始宽、高与像素数，超限即拒绝；TIFF 逐页检查，**不会缩小源图片**。没有设置 `image_limits.max_pixels` 时，如果显式设置了旧字段 `document.max_pixels`，图片继续继承该值；否则图片默认上限为 1 亿像素。PDF 及其他文档渲染仍独立使用 `document.max_pixels`（默认 4000 万）。该输入限制与模型级 `input_resize` 分别作用于解码前和推理前；模型缩放不能规避输入限制。示例：`"document": {"image_limits": {"max_width": 12000, "max_height": 12000, "max_pixels": 100000000}}`。

`routes` 的键是经过 type_map 的类型名；`*` 是兜底。未命中且无兜底时按 on_error 处理。每个 route 的 `action` 默认为 `recognize`，须在 `model`（单个模型 ID）和 `models`（非空、不可重复的模型 ID 数组）中**二选一**；可设置 route 级 `prompt` 和 `save_crop`。`image` 只保存裁剪，`skip` 跳过识别但仍在 JSON 保留 BOX。

`models` 按给定顺序尝试推理；仅在模型推理/结果格式异常时切换到下一个模型，成功后记录实际使用的模型 ID。所有候选都失败时，`on_error: record` 保留 BOX 和包含候选模型错误的 `error`；`on_error: fail` 抛错。同一模型 ID 无论在多少种 BOX 的候选列表中引用，都复用同一个有界实例池；候选模型池会在 Pipeline 初始化时全部创建，因此配置本地 OM 模型时要预留各候选实例的设备内存。建议将该机制用于主备模型容错，而不是将任意错误隐式掩盖。

```json
"routes": {
  "text": {"models": ["primary_vllm", "backup_onnx"], "prompt": "Text Recognition:"},
  "title": {"model": "primary_vllm"},
  "image": {"action": "image"}
}
```

## 可选 BOX 与跨页后处理

v1 顶层和 v2 顶层都可设置 `postprocess`，所有开关默认为 `false`；布局解析完成后、BOX 识别前按顺序执行低分过滤、页眉页脚删除、复合框决策、重叠框消除。跨页表格在文档所有页面完成并恢复页序后处理，CLI、批处理和 REST 一致。

```json
"postprocess": {
  "low_score": {"enabled": true, "threshold": 0.3},
  "header_footer": {"enabled": true,
    "types": ["header", "footer", "page_header", "page_footer", "ignored_header", "ignored_footer"]},
  "overlap": {"enabled": true, "iou_threshold": 0.5, "score_margin": 0.3,
    "label_priority": ["table", "chart", "figure", "title", "heading", "formula", "equation", "text", "header", "page_header", "footer", "page_footer"]},
  "composite": {"enabled": true, "outer_types": ["chart", "figure", "table"],
    "expand_inner_types": ["table", "formula", "equation", "title", "heading"],
    "containment_threshold": 0.9, "retain_min_score": 0.7, "max_recovered_boxes": 100,
    "inspect_inner_text": false, "expand_if_uncovered_text": true, "min_inner_text_chars": 4},
  "cross_page_tables": {"enabled": true, "edge_margin_ratio": 0.1,
    "x_overlap_threshold": 0.75, "require_header_match": false}
}
```

`low_score` 在所有布局适配器返回后统一过滤；原有 `layout.score_threshold` 仍会在各内置适配器中更早过滤。`header_footer` 直接移除匹配类型的 BOX，不推理也不写入结果；与 route 的 `action: skip`（仍保留 BOX）不同。类型匹配发生在 `layout.type_map` 映射之后，请按部署模型的实际标签修改数组。

`overlap` 只比较同页 bbox 的 IoU。IoU 达阈值时，分数差 **≥ 0.3** 优先选高分；差值较小时按 `label_priority` 数组从左到右选，最后以高分和原始索引打破平局。默认优先级是表格 > 图表 > 标题 > 公式 > 文本 > 页眉页脚；数组可独立调整。该策略不跨页去重，部分相交但 IoU 较低的不同内容会同时保留。

`composite` 把包含比例达到阈值的 BOX 挂在**最小包围父框**下。置信度足够的图表/表格外框若只有普通文本子框则保留外框并抑制其所有后代；低分外框，或存在表格、公式、标题等语义后代时展开内部框。开启该功能且发现复合外框时，额外进行**一次**布局推理：将外框在原页上涂白，只吸收与涂白区域相交面积小于候选框 20% 的新增框，最多吸收 `max_recovered_boxes` 个。该补检寻找外框**周围**被遮蔽的遗漏内容，不会声称恢复涂白区域内的内容。模型可能在白块边缘产生误检，请结合低分过滤和重叠消除验证。启用后页面可能多一次布局推理，有额外延迟与算力开销。

可选的 `inspect_inner_text: true` 让原本要保留的复合外框及其内部框都完成 OCR，然后再决定输出：如果外框识别为空、报错，或 `expand_if_uncovered_text: true` 且外框文本未覆盖长度达到 `min_inner_text_chars` 的内框文本，则展开内框；否则保留外框。比较时忽略空白并采用不区分大小写的字符串包含判断，并非语义相似度；默认不开启以避免增加内框推理负载。被抑制框的临时裁剪资源会删除，最终输出中只保留被选中的框。

`cross_page_tables` 仅合并**相邻页**页尾与页首、水平位置对齐、同列数的简单 HTML `<table>`；默认要求表格分别位于距页边 10% 内、水平重合达到较宽表格宽度的 75%。重复表头行会移除；设 `require_header_match: true` 时只有两页表头相同才合并。合并结果放在首次出现的页，后续页表格 BOX 保留坐标与原始识别结果 `raw_text`，清空用于 Markdown 的 `text`，并通过 `extensions.omniocr.merged_into` 指向首个 BOX；首个 BOX 记录 `merged_pages`。含嵌套表格、`rowspan`/`colspan`、列数变化或非纯表格 HTML 的结果保持原样，避免猜测单元格关系。开启相应功能时，带扩展元数据的结果自动使用 JSON schema v2；显式指定 schema v1 会舍弃扩展元数据。

## 模型池

`instances` 默认 1，范围 1–128；`max_concurrent_requests` 默认等于 `instances`，范围 1–128，控制此模型 ID **同时在途的后端调用数**；`acquire_timeout_ms` 默认 60000。比如一套已部署的 vLLM 权重用 `instances: 1, max_concurrent_requests: 8`，框架建立 8 个可复用的 HTTP 客户端，同时最多发出 8 个请求，远端模型仍只有一套。小于 `instances` 时只启用前若干槽位；大于 `instances` 时为每个额外槽位构建独立模型句柄，以免一个句柄被并发访问。**本地 ACL/ONNX 及外部插件可能因此加载额外权重和设备缓冲**，资源须按 `max(instances, max_concurrent_requests)` 个句柄预算；不支持共享一个不可重入的本地推理句柄。对 ACL，`device_ids: [0,1]` 按句柄序号轮转分配设备。

### 每个模型的最大输入分辨率

可在每个模型 ID 下设置 `input_resize`；v2 配置写在 `executors.<id>`。`min_width`、`max_width`、`min_height`、`max_height`、`min_pixels`、`max_pixels` 全部可选，按实际模型约束组合；`factor` 可选，要求输出宽高为其整数倍。只设置上限时按比例缩小、不会放大小图；设置下限时允许按比例放大。像素范围约束的是最终输出的宽 × 高。缩放使用双线性插值，优先贴近原始宽高比；整除或上下限迫使取整时可能有细微比例误差。`factor` 默认 1，因此已有的仅设置上限的配置保持原行为。显式上限低于下限或量化后无可行尺寸时会报错。该处理在模型池入队和估算视觉 token **之前**进行，因此同一个模型实例被不同文件、BOX 类型或候选路由共享时使用相同约束；动态组批中的每个输入分别缩放。未设置时保持现有行为。

```json
"models": {
  "layout": {"backend": "http_json", "endpoint": "http://127.0.0.1:8001/layout",
             "input_resize": {"max_width": 1600, "max_height": 1600}},
  "ocr": {"backend": "vllm", "endpoint": "http://127.0.0.1:8000/v1/chat/completions",
          "model": "served-vlm", "input_resize": {"factor": 28,
              "min_width": 28, "min_height": 28, "min_pixels": 101920,
              "max_width": 1536, "max_height": 1536, "max_pixels": 1003520}}
}
```

布局输出使用 `coordinates: pixel` 时，框架把模型输入尺寸的检测框、polygon 和 crop_bbox 映射回原页；`normalized` 和 MinerU 0–1000 坐标始终以原页为基准。V3 的 `model_input` 坐标自动使用最终输入尺寸；显式 `layout.transform` 与模型级 `input_resize` 不能同时配置，因为预先给定的逆变换不一定适用于缩放后的输入。原有 `layout.image_size` 仍可用于需要固定大小输入的布局模型，然后再受模型级尺寸约束。ONNX/ACL 的固定 `preprocess.width/height` 仍控制导出模型的张量形状，`input_resize` 在其之前执行；如果依赖 `original_shape` 输入，请核对相应模型的坐标解码。`save_crop` 保存原始裁剪，和实际送模型的缩放图可能不同。缩小可能影响小字与表格精度，放大可能增加内存与推理开销，请按模型单独 A/B 验证吞吐和质量。

每个模型 ID（v2 为每个 `executor`）独立设置 `batch_size`，默认 1，范围 1–128；大于 1 时启用全 Pipeline 共享的组批队列，同一模型来自**不同文件、页面和 BOX 类型**的请求可进入一批。`max_batch_wait_ms` 默认 5、范围 0–1000：从队首请求到达起最多等待该时间，达到批大小则立即执行，尾批到时执行；必须小于 `acquire_timeout_ms`。`max_pending_requests` 默认 256，至少等于 `batch_size`，排满或排队超时都会明确失败；候选模型可按原路由规则回退。`max_concurrent_requests` 控制并行批次数上限，每个活跃槽位执行一次原生批量调用；结果按提交顺序归还给原 BOX，文档输出仍按页号和阅读顺序排列。

`instance_overrides` 可按活跃槽位编号覆盖 `batch_size` 和 `max_batch_wait_ms`，数组第 0 项对应实例 0，未列出的实例继承模型级设置。所有实例从**同一个模型 ID 的全局 BOX 队列**取任务；空闲实例按各自的批大小取队首请求，达到其窗口时执行尾批，避免预先将 BOX 固定分片到繁忙实例。队列上限至少覆盖模型级与各实例设置中最大的批大小；静态 batch ONNX/ACL 权重必须与相应实例的设置匹配，不同尺寸的实例要有相容的模型形状。`batch_size: 1` 的槽位直接执行单条推理。若所有实例都设为 1，沿用无后台组批线程的有界 FIFO 实例租赁路径。全局队列不会越过 `box_workers` 的输入并发上限，实测时须同时配置该值。

只有支持一次真实批量调用的后端接受 `batch_size>1`：ONNX、导出固定 batch 的 ACL OM、带显式 `batch_endpoint` 的 `http_json`、mock 及实现批量入口的 C ABI v2 插件。无批量能力的 C ABI v1/C++ 后端在初始化时报错，不会在组批后逐条串行执行。`vllm` 的 Chat Completions 单请求协议不接受一次多图多任务批量调用；请保持框架 `batch_size=1`，用 `max_concurrent_requests` 向同一服务发出并发请求，在 vLLM 服务中配置其自身的连续批处理容量。组批窗口会增加低负载单请求延迟；批大小也不是并发槽位或 vLLM `max_num_seqs` 的别名。

一次底层批量推理整体失败时，该批次内所有请求收到同一个模型错误，按各自 BOX 的候选模型或 `on_error` 策略处理；不会自动重试整个批次，以免重复执行外部推理。部署前应验证模型能接受批内不同文件的输入与 prompt，并单独测高低负载下的吞吐、P95/P99 和内存占用。

```json
"models": {
  "ocr": {
    "backend": "http_json", "endpoint": "http://127.0.0.1:8001/infer",
    "batch_endpoint": "http://127.0.0.1:8001/batch",
    "instances": 2, "max_concurrent_requests": 4,
    "batch_size": 8, "max_batch_wait_ms": 10,
    "instance_overrides": [{"batch_size": 4, "max_batch_wait_ms": 5}, {}],
    "max_pending_requests": 256
  }
}
```

在 v2 配置中把相同参数写到 `executors.<id>`，多个 model binding 共用这一批队列。原有 v2 `max_inflight` 仍映射到 `instances`，可以另设 `max_concurrent_requests` 调整实际在途上限。将不同任务绑定到同一执行池前须确认该模型、prompt 与返回协议兼容。

vLLM/HTTP 的并发槽位不会创建远端模型副本；同一个 endpoint 后面的实际模型数由服务部署控制。连接不同部署可定义多个模型 ID 或使用服务端负载均衡地址。内置 HTTP/vLLM 每个槽位复用自己的连接，不在多个线程同时使用同一客户端；实际在途上限还受 REST 的 `--box-workers`（默认 1）或批处理的 `options.box_workers`、BOX 数及后端自身容量约束。单文件 CLI 使用 `execution.workers`，详见 [并发与性能](performance.md)。

## vLLM

`backend: vllm`，`endpoint` 填完整 `/v1/chat/completions` URL，`model` 必须匹配 served model name。可用 `api_key_env` 指定密钥环境变量，不把密钥写进配置。

### vLLM 视觉 token 分桶投递（可选）

离线 BOX OCR 可在单个 vLLM 模型（v2 为 `executors.<id>`）启用 `vllm_visual_scheduler`。模型池收集来自不同文件、页面和 BOX 类型的请求，按**预处理后**的图片尺寸估算视觉 token，依据配置的边界分桶；在短等待窗口内优先选取同桶请求，使一波请求的估计 prefill 尽量填满服务端每轮 token 预算，并参考 CUDA graph 的 capture size 挑选投递波次。每个 BOX 仍是独立的 Chat Completions 请求，服务端负责连续批处理。不要设置框架 `batch_size>1`；此策略与 `adaptive_concurrency` 互斥。

```json
"ocr": {
  "backend": "vllm", "endpoint": "http://127.0.0.1:8000/v1/chat/completions",
  "model": "served-vlm", "instances": 1, "batch_size": 1,
  "max_concurrent_requests": 32, "max_pending_requests": 256,
  "vllm_visual_scheduler": {
    "enabled": true,
    "max_num_seqs": 32, "max_model_len": 8192,
    "max_num_batched_tokens": 4096,
    "cudagraph_capture_sizes": [1, 2, 4, 8, 16, 32],
    "visual_pixels_per_token": 784,
    "visual_token_overhead": 0, "max_visual_tokens": 4096,
    "prompt_token_overhead": 64, "expected_output_tokens": 512,
    "bucket_edges": [256, 512, 1024, 2048], "max_wait_ms": 2
  }
}
```

| 配置 | 用途 |
|---|---|
| `visual_pixels_per_token` / `visual_token_overhead` / `max_visual_tokens` | 模型专属估算：`visual = min(max_visual_tokens, visual_token_overhead + ceil(resized_width × resized_height / visual_pixels_per_token))`；按模型的 patch、动态分辨率、crop/tiling 规则实测后填写；上例仅为演示。 |
| `bucket_edges` | 严格递增的视觉 token 上界，`upper_bound` 分桶；可按实际 BOX 面积分位数设置。 |
| `max_model_len` | 根据 `prompt_token_overhead + ceil(prompt字节数/4) + visual + expected_output_tokens` 估算单请求长度；超出即拒绝。文本 token 估算是近似值，服务端还须正确配置截断和输入上限。 |
| `max_num_seqs` | 服务端配置快照，限制本模型池同时发起的调用数；还受 `max_concurrent_requests` 限制。若多个模型 ID 共用一个 vLLM 服务，须合并规划各自上限。 |
| `max_num_batched_tokens` | **每波新投递请求**估计 prefill 的软预算；单个超预算请求可独占这一波，交由 vLLM 处理分块 prefill。它不是所有在途 prompt 与输出 token 之和的硬上限。 |
| `cudagraph_capture_sizes` | 手动填写服务端已启用的升序 capture size；选不超过当前空闲槽和排队请求数的最大 size，若都不满足则选 1。它只决定一波的投递目标，**不保证** vLLM 实际形成相同的图批次。 |
| `max_wait_ms` | 低负载下最多等待这么久凑候选请求，默认 2 ms，范围 0–1000，必须小于 `acquire_timeout_ms`；队首到时优先服务以免某桶长期饥饿。 |

`GET /v1/metrics` 中可对照 `strategy=visual_bucket`、`visual_bucket_dispatched`（按桶计数）、`visual_waves_total`、`visual_wave_requests_total`、`visual_wave_prefill_tokens_total`、`queued`、`inflight` 与 `queue_wait_ms_total`。波次指标为**投递计划数**，请求失败或取消时可能不同于成功数。建议先用代表性的大小和输出长度分布，固定 `max_concurrent_requests` 做扫描，再交替 A/B 对比 `docs/min`、成功率、vLLM Running/Waiting、图像缓存命中与 p95 排队时间；这是启发式调度，无法仅由静态配置推算全局最优吞吐。`box_workers`、页面与文档 worker 必须能持续提供请求。

vLLM 参数需按实际服务版本核对：[调度器配置](https://docs.vllm.ai/en/latest/api/vllm/config/scheduler/)把 `max_num_batched_tokens` 定义为调度轮次 token 预算，`max_num_seqs` 控制序列容量；[CUDA graph 配置](https://docs.vllm.ai/en/latest/api/vllm/config/vllm/)中的 capture sizes 是图形状。框架只使用手动快照，不自动修改或抓取服务端配置。

### 离线任务自适应并发

单个 vLLM 模型（v2 为 `executors.<id>`）设置 `adaptive_concurrency.enabled: true` 后，每个 BOX 独立发送请求，vLLM 保持自身的连续批处理。控制器以完成的估算工作量/秒爬山：先增大在途 token 预算，吞吐增益接近平台时按小步探测，探测造成吞吐下降时温和回退。请求数由 `max_concurrent_requests` 硬限制，适配不同大小 BOX 的主控制量为 `current_token_budget`。页面/BOX worker 必须足够多且有持续排队需求，否则无法判断提升预算是否有收益。

```json
"ocr": {
  "backend": "vllm", "endpoint": "http://127.0.0.1:8000/v1/chat/completions",
  "model": "served-vlm", "instances": 1, "max_concurrent_requests": 32,
  "batch_size": 1, "max_pending_requests": 256,
  "adaptive_concurrency": {
    "enabled": true, "min_concurrency": 1, "initial_concurrency": 8,
    "window_ms": 5000, "min_samples": 8, "cooldown_ms": 3000,
    "token_budget": 32768, "min_token_budget": 1024,
    "initial_token_budget": 8192,
    "slow_start_gain": 0.10, "probe_gain": 0.03,
    "probe_step": 0.10, "backoff_ratio": 0.85,
    "image_pixels_per_token": 784, "expected_output_tokens": 512
  }
}
```

`token_budget` 是在途估算 token 的硬上限，`min_token_budget <= initial_token_budget <= token_budget`；单个 BOX 的估计超过当前预算时允许独占运行，不能据此把预算视为严格的设备显存上限。不填写初始/最小预算时，分别按 `token_budget × initial_concurrency / max_concurrent_requests` 和 `token_budget × min_concurrency / max_concurrent_requests` 推导；后两个旧字段仅用于默认预算计算。`max_concurrent_requests` 同时决定预创建的 HTTP 客户端个数。`window_ms` 默认 5000 ms，建议在真实后端上使用 5–10 秒以上且有足够完成样本的窗口。

每个完成窗口统计 `normalized_work = 64 + prompt字节数/4 + 输出 token + 图片像素数/image_pixels_per_token`，目标为 `sum(normalized_work)/窗口秒数`。输出 token 优先使用 vLLM 的 `usage.completion_tokens`，若响应未提供则使用 `expected_output_tokens`。这个量是**工作代理指标**，不是真实视觉 token、NPU FLOPs 或准确率：vLLM 的缩放/patch 规则和缓存命中会改变真实计算量。`usage.total_tokens` 仅校准**准入预算估计**，不回写已完成工作量，避免动态校准让工作量单位漂移。必须按面积与输出长度分桶对比真实负载。

持续需求且预算有阻塞时，慢启动只有增益超过 `slow_start_gain` 才继续翻倍；随后按 `probe_step` 小步探测，增益低于 `probe_gain` 时回到探测前预算并暂缓再探。上探吞吐下降超过 `probe_gain` 时按 `backoff_ratio` 温和退避；到达预算硬上限后也可向下探测。HTTP 429/503 立即按该比率退避，高失败率也退避，不自动重试。时延只作为诊断指标，不会因自身升高而降预算。旧版 `latency_target_ms`、`latency_guard_ratio`、`latency_guard_windows` 配置仍被接受以兼容配置文件，但不参与新的控制决策；建议删除。

`GET /v1/metrics` 的 `models.<id>` 含 `current_token_budget`、`token_budget`、`control_phase`、`window_normalized_work_per_second`、`completed_normalized_work_total`、`throughput_gain`、`throughput_backoff_total`、请求吞吐、平均后端时延、队列深度及排队累计事件。`pressure_total` 会在入队发现阻塞或完成时队列未空累加，**不是当前积压长度**。队首大 BOX 暂时放不进预算时，小 BOX 最多越队四次。多个模型 ID 各自有独立控制器；如果它们共享同一 vLLM endpoint，预算不会跨 ID 合并。vLLM 的 Running/Waiting/KV cache 指标可另行采集对照，当前客户端不抓取远端 Prometheus 指标参与控制，以避免监控端点不可用阻塞推理。

固定并发仍为默认策略；自适应仅用于 vLLM，不能与原生组批或 `instance_overrides` 同时启用。工作量代理和控制参数是否达到实机最高吞吐，必须以固定预算/并发扫描和交替 A/B 测试验证。

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

可选批量端点采用 `POST batch_endpoint`：请求 `{"requests":[{"image":"data:image/png;base64,...","width":1000,"height":1400,"prompt":"..."},...]}`；响应 `{"results":[{"text":"..."},...]}`，结果个数和顺序必须与请求一致。布局返回的每个 `results` 元素仍使用单张布局格式。服务必须真正按 batch 推理；若只在服务端循环调用单张模型，虽然协议可用，却没有原生 batch 加速。

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

ACL 模型默认使用 `"acl_async_stream": true`：每个活跃模型句柄持有独立 stream 和复用的锁页 Host 输入/输出缓冲。一次调用按顺序提交所有 H2D 拷贝、`aclmdlExecuteAsync` 和 D2H 拷贝，最后同步一次，再读取结果；不同句柄可通过各自的 stream 并行执行。设置 `"acl_async_stream": false` 可回退到同步路径。此设置不会让同一模型句柄同时处理多个请求，实际并发仍由 `max_concurrent_requests`、`execution.box_workers` 和设备容量共同决定。锁页缓冲按 OM 输入/输出字节数为**每个活跃句柄**各申请一套，配置并发时应计入 Host 内存预算。失败的 stream 同步不会被重新用于下一次推理。

### 输入

`preprocess` 必须包含固定 `width/height`。当前把 RGB 图像双线性拉伸到该尺寸，布局为 NCHW，默认 `scale=1/255`、`mean=[0,0,0]`、`std=[1,1,1]`，计算 `(pixel * scale - mean) / std`。`color` 可选 `rgb/bgr`。不隐式 letterbox、自动 AIPP 或处理动态多档 shape。

`inputs` 按模型名称匹配：

| source | float32 张量 |
|---|---|
| `image` | `[1,3,H,W]` |
| `original_shape` | `[1,2]`，原图 `[H,W]` |
| `scale_factor` | `[1,2]`，`[目标H/原H,目标W/原W]` |
| `constant` | 显式 `shape` 和 `data` |

启用批量时，每个输入必须有前导 batch 维度 1；框架逐样本完成预处理，然后沿第 0 维拼接。ONNX 可以使用动态 batch 维，也可以导出固定 batch；ACL 当前只支持与 `batch_size` 相同的静态 batch OM，不支持动态 batch OM。固定 batch 尾批复制最后一张输入凑足模型形状，推理后丢弃填充项结果，因此实际执行的输入数可能超过有效请求数；需用真实权重验证该填充不会改变其他样本输出。其他维度、输入名称、dtype 必须一致；不自动实现 AIPP、不同 shape 的分桶或多档 OM。

例如某模型 `im_shape` 期望的是 resize 后的形状，应使用 constant `[H,W]`；不要仅凭输入名字误选 original_shape。

### 输出

`decoder.type: paddle_layout` 选择 float32 `[N,6]` 输出，每行 `[class_id,score,x1,y1,x2,y2]`；`output_index` 默认 0。`labels` 必须逐项对应模型类别；`coordinates` 为 `pixel` 表示原图坐标，为 `input` 表示 resize 后坐标。该适配要求模型已输出检测框，**不包含通用 RT-DETR 原始 logits/bbox 解码、NMS、V2 指针排序网络**。

`decoder.type: ctc` 选择 `[1,T,C]` float32 logits/概率。提供 `vocabulary` 数组或 UTF-8 `dictionary` 文件（每行一个 token），词表必须包括 blank 位置且条目数等于 C；`blank_index` 默认 0。例如首行空行代表 blank，不自动添加空格类。解码执行 argmax、去重复和去 blank。

批量模型的 CTC 输出须为 `[B,T,C]`，布局为 `[B,N,6]`；按有效样本拆成已有的单张解码器格式。输出不是这种形状（例如变长框表 + `bbox_num`、原始 RT-DETR 多头输出）需要专用模型适配，不能靠 `batch_size` 直接启用。

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
