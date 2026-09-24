# 架构与资源生命周期

[项目首页](../README.md) · [文档目录](README.md)

## 模块边界

| 模块 | 实现 | 责任 |
|---|---|---|
| 文档入口 | `src/document.cpp` | 图片解码、独立 Office 工作目录与用户配置、PDF 按页渲染、子进程超时 |
| 统一数据 | `include/omniocr/core.hpp` | Image、Box、Region、Page、Document |
| 配置 | `src/config.cpp` | 引用关系、并发与资源上限、模型文件相对路径 |
| 布局适配 | `src/layout.cpp` | Paddle/归一化 JSON、MinerU token、类型映射、坐标裁剪、阅读顺序 |
| 模型注册表 | `src/model_pool.cpp` | 每个 ID 创建一个池、独占租用、异常归还、获取超时 |
| 远程推理 | `src/backends/http.cpp`、`models.cpp` | JSON HTTP、PNG data URL、vLLM 请求与响应 |
| 本地推理 | `src/backends/onnx.cpp`、`acl.cpp` | TensorEngine；Session/context、模型、内存的生命周期 |
| 模型语义 | `src/backends/tensor.cpp` | NCHW/RGB/BGR/归一化、辅助输入、CTC 与 Paddle 检测解码 |
| 编排 | `src/pipeline.cpp` | 逐页布局、BOX 路由、有界并发、按原始顺序写结果 |
| 输出 | `src/output.cpp`、`table.cpp` | JSON、Markdown、OTSL 合并表格 |

核心设计是 `模型语义适配 → 张量执行后端` 的分离。ONNX 和 OM 只描述执行图，不自动提供 OCR 解码器；相同模型导出到不同执行后端时可以复用语义适配，不同 OCR 模型则可能需要新的适配器。

## 调度

一次 Pipeline 构造完成所有模型池初始化。每个 `run()` 按页读取：先布局，释放布局模型租约，再发起 BOX 识别。识别线程按索引领取任务，每个线程只持有当前 BOX 的裁剪。没有每个 BOX 启动一个线程、也没有整本文档的图片预加载。

`workers=8`、`text.instances=1`、`text.max_concurrent_requests=4` 意味着最多 8 个 BOX 在处理，其中至多 4 个 text 后端调用同时在途；其他线程等待。额外槽位对 HTTP 是独立连接客户端，对本地 ACL/ONNX 是额外模型句柄。设置 `text.batch_size>1` 后，每个并发槽位一次处理最多 batch_size 个请求，模型池按 ID 汇集所有文件的 BOX，并在最大等待窗口结束或收齐一批时调用一次原生批量接口。这个队列不会额外创建页面处理线程；正在等待模型的页面线程仍被占用，跨模型公平性和独立的全局 BOX 工作池尚未实现。配置和后端支持范围见 [模型池](configuration.md#模型池)。

任意 BOX 抛出异常后租约都会归还。`fail` 模式停止领取后续任务，等待已经运行的任务结束，再向调用方抛出；不会在线程还使用图像/模型时释放它们。`record` 模式保留失败块，继续其他块，CLI 返回 2。远端 HTTP 设置连接/总请求超时和响应大小限制，不进行可能重复执行推理的自动重试。

单文件 `run()` 的进程总并发仍取决于外层调用数；单个 Pipeline 的模型池在这些调用之间共享，但每个调用有独立 BOX 线程组。多文件场景优先使用下述 `run_batch()` 全局页工作线程池。

## 多文件与按页优先级调度

`Pipeline::run_batch(jobs, options)` 复用同一个 `ModelRegistry`，一次接收多个 `BatchJob{input, output_dir, priority}`。单个文档的读取/转换由最多 `max_active_documents` 个 reader 线程处理；每个 reader 依次把 PDF 页渲染成图片并放入全局 `max_queued_pages` 限长队列。最多 `page_workers` 个页面线程从就绪页中选择 **priority 数值更大** 的文件页面处理，同优先级按入队顺序处理。未就绪的高优先级页不会阻塞已经就绪的低优先级页；已开始的页不会抢占或中断。若高优先级文件持续产出页面，低优先级文件可能等待较久（本版未实现老化或时限公平调度）。

每个页面线程从版面到 BOX 识别串行执行该页的 BOX，因此 `page_workers` 是整批处理的推理并发上限，而非乘以现有 `execution.workers`；每个模型 ID 的 `instances` 仍对全批任务生效。Reader 的 PDF 渲染、Office 转换可以与推理并行，它们受 `max_active_documents` 单独限制，不计入页面推理工作线程数。内存上限来自 `max_queued_pages` 个已入队图像 + 正在渲染的 reader 图像 + 正在处理的页面及模型中间缓存，不是全程固定的精确字节数。

一个文件出现文档/页级致命错误时，取消该文件尚未执行的页面；已启动的推理会完成并归还模型租约，其他文件继续处理。调用方收到按 **输入任务原顺序** 排列的 `BatchResult`，成功文档内部的页按原始页码升序输出。不同任务的输出目录必须互不重叠；一个文件的失败不应覆盖另一个文件的结果。BOX 级 `on_error: record` 仍保留部分结果，页渲染或版面失败仍属于文件级失败。

批处理 API 接收一次性提交的任务清单，使用方式见 [批处理指南](batch.md)。运行期间动态提交新文件请使用下节的常驻 REST 服务；两种方式都不提供任务取消、运行中优先级调整、持久化队列、跨进程调度或抢占式调度。

## 常驻 REST 服务调度

`src/server.cpp`（可选 `OMNIOCR_WITH_SERVER=ON`，依赖 libmicrohttpd）提供 `omniocr-server`：HTTP 线程只接收请求、校验输入并落盘，**不在请求线程运行 OCR**。路径或原始二进制提交后生成不可预测的任务 ID、独立输入/输出目录，随即返回 HTTP 202。后台常驻 `document-workers` 线程从等待文件队列中选择较高优先级任务，执行文档转换和逐页渲染；`page-workers` 全局页面线程从已就绪页队列选择较高优先级页。模型实例池只初始化一次并被整个进程复用，所有文件共享实例上限；页面内采用单 BOX 工作线程，避免双层并发相乘。

HTTP 可在旧任务处理中继续提交新文件；新任务在 reader 空闲后进入页队列，并在**下一次页面工作线程领取时**按优先级参与调度。已开始的页面不会被强制抢占，已占用的文档读取线程也不会因新任务立刻中断；这不是实时 SLA 调度。每个文件的页面结果按页号排序后写到该任务独立输出目录，一个文件错误只影响它自身。上游请求断开不会取消后台任务。页图像内存由等待队列上限、reader 和页面工作线程数量共同约束；任务文本结果在写出后从内存释放。

任务状态是进程内的：重启后已写盘的文件/结果保留，但原有任务 ID 状态不恢复；`max-jobs` 限制服务进程生命周期累计任务数。服务端路径访问需要配置 allow root；服务 API 不支持多租户隔离或 TLS，必须经 HTTPS 反向代理增加权限治理。详细接口及限制参见 [REST 服务说明](server.md)。

## ACL 资源

进程级 runtime 管理 `aclInit` 与最终 `aclFinalize`，实例拥有模型、context 和输入/输出设备缓冲区。每次推理恢复当前线程的 context，避免模型被不同线程租用后使用错误的设备上下文。释放模型实例时不立即 reset 设备；进程 runtime 退出时统一 reset，防止同卡其他池实例被提前破坏。

这个 runtime 所有权约定适用于由 OmniOCR 管理 ACL 的进程。若嵌入已经自行调用 `aclInit/aclFinalize` 的宿主，应先扩展为可注入、共享的 runtime 管理器。ACL 内部设备故障或永久阻塞不使用 C++ 强行终止线程；生产服务可在独立工作进程外增加超时/重建策略。

## 文档与结果

Office 转换使用唯一临时目录与唯一 LibreOffice 用户配置；通过 argv 启动，不经过 shell。超时杀掉子进程组并回收直接子进程。除了检查退出码，还检查 PDF 实际存在，再用 pdfinfo 验证。受密码、IRM 或损坏影响的文件返回错误，不尝试解除权限。

JSON 坐标固定为渲染页像素 `[x1,y1,x2,y2]`，page 从 1 开始，rotation 为逆时针角度。BOX 包含规范化类型、原始类型、顺序、置信度、使用的模型 ID、文本、裁剪路径、错误；表格同时保留 `raw_text`。阅读顺序来自模型返回的 order，否则使用模型返回顺序，不用简单坐标排序冒充多栏阅读顺序。

Markdown 可保留模型输出的 Markdown/HTML；OTSL 被转成支持 rowspan/colspan 的 HTML 表格。原样 HTML 属于模型内容，下游网页渲染时应按自己的显示策略处理。格式输出不包含 token 使用量、合并跨页表格、OCR 精度评价或复杂公式纠错。

## C++ 集成

在仓库根目录运行的应用中，可按以下方式复用 Pipeline：

```cpp
#include <omniocr/core.hpp>

auto config = omniocr::load_config("configs/mineru-vllm.json");
omniocr::Pipeline pipeline(config); // 加载模型一次，可顺序复用处理多个文档
auto document = pipeline.run("document.pdf", "out/document");
omniocr::write_outputs(document, "out/document", "both");
```

可通过 `ModelFactory` 注入自定义 `Model`，或实现 `TensorEngine` 并配套模型预处理/解码器。布局与识别共用模型注册表；不要在 BOX 回调中重新创建模型。外层如需并发调用 `run()`，应自行限制文档并发，因为工作线程上限是每次调用的上限。

## 相关文档

[批处理用法](batch.md) · [REST API](server.md) · [配置参考](configuration.md)
