# 架构与资源生命周期

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

`workers=8`、`text.instances=2` 意味着最多 8 个 BOX 在处理，其中至多 2 个使用 text 模型推理；其他线程等待租约。表格可以单独使用另一个池，但固定线程可能被文本池等待占用，初版不是按模型分队列的公平调度器。单页大量同类 BOX 时，可调整线程/实例比例；后续可扩展为按池任务队列。

任意 BOX 抛出异常后租约都会归还。`fail` 模式停止领取后续任务，等待已经运行的任务结束，再向调用方抛出；不会在线程还使用图像/模型时释放它们。`record` 模式保留失败块，继续其他块，CLI 返回 2。远端 HTTP 设置连接/总请求超时和响应大小限制，不进行可能重复执行推理的自动重试。

进程总并发取决于外层 `run()` 调用数；单个 Pipeline 的模型池在这些调用之间共享，但每个调用有独立 BOX 线程组。因此业务服务应额外限制文档请求数。

## ACL 资源

进程级 runtime 管理 `aclInit` 与最终 `aclFinalize`，实例拥有模型、context 和输入/输出设备缓冲区。每次推理恢复当前线程的 context，避免模型被不同线程租用后使用错误的设备上下文。释放模型实例时不立即 reset 设备；进程 runtime 退出时统一 reset，防止同卡其他池实例被提前破坏。

这个 runtime 所有权约定适用于由 OmniOCR 管理 ACL 的进程。若嵌入已经自行调用 `aclInit/aclFinalize` 的宿主，应先扩展为可注入、共享的 runtime 管理器。ACL 内部设备故障或永久阻塞不使用 C++ 强行终止线程；生产服务可在独立工作进程外增加超时/重建策略。

## 文档与结果

Office 转换使用唯一临时目录与唯一 LibreOffice 用户配置；通过 argv 启动，不经过 shell。超时杀掉子进程组并回收直接子进程。除了检查退出码，还检查 PDF 实际存在，再用 pdfinfo 验证。受密码、IRM 或损坏影响的文件返回错误，不尝试解除权限。

JSON 坐标固定为渲染页像素 `[x1,y1,x2,y2]`，page 从 1 开始，rotation 为逆时针角度。BOX 包含规范化类型、原始类型、顺序、置信度、使用的模型 ID、文本、裁剪路径、错误；表格同时保留 `raw_text`。阅读顺序来自模型返回的 order，否则使用模型返回顺序，不用简单坐标排序冒充多栏阅读顺序。

Markdown 可保留模型输出的 Markdown/HTML；OTSL 被转成支持 rowspan/colspan 的 HTML 表格。原样 HTML 属于模型内容，下游网页渲染时应按自己的显示策略处理。格式输出不包含 token 使用量、合并跨页表格、OCR 精度评价或复杂公式纠错。
