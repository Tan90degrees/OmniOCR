# OmniOCR REST 服务

[项目首页](../README.md) · [文档目录](README.md)

C++ 常驻服务通过 libmicrohttpd 提供异步任务接口，所有文件共享同一模型注册表和全局优先级页面队列。客户端提交后收到任务 ID，不需要保持 HTTP 连接等待 OCR。

## 构建及启动

```bash
# Ubuntu: sudo apt-get install libmicrohttpd-dev
# EulerOS/openEuler: 使用目标系统仓库中匹配的 libmicrohttpd 开发包。
cmake -S . -B build -DOMNIOCR_WITH_SERVER=ON \
  -DOMNIOCR_WITH_ACL=ON -DASCEND_HOME=/usr/local/Ascend/ascend-toolkit/latest
cmake --build build -j4
export OCR_API_KEY='replace-with-a-strong-secret'
./build/omniocr-server \
  --config /etc/omniocr/acl_vllm_ocr.json \
  --data-dir /var/lib/omniocr \
  --allowed-input-root /data/documents \
  --host 127.0.0.1 --port 8080 \
  --page-workers 4 --box-workers 8 --document-workers 4 --max-queued-pages 8 \
  --max-upload-bytes 67108864 --max-jobs 1000 \
  --max-active-jobs 64 --max-inflight-upload-bytes 268435456 \
  --max-queued-page-bytes 268435456 --http-connections 128 \
  --api-key-env OCR_API_KEY
```

上述配置路径应替换为实际已验证的模型配置文件。文件路径提交接口仅在显式设置 `--allowed-input-root` 时启用；未设置时只能上传二进制。默认只监听 127.0.0.1；监听 0.0.0.0 时必须设置 API key。HTTP 本身没有 TLS 或租户权限隔离：跨机器访问请通过可信 HTTPS 反向代理增加独立用户鉴权、限流与审计，不要把高权限的服务器目录开放给不可信客户端。

## 提交文件

**通过服务器/容器内文件绝对路径（POST /v1/jobs，application/json）：**

```bash
curl -X POST http://127.0.0.1:8080/v1/jobs \
  -H "Authorization: Bearer $OCR_API_KEY" \
  -H 'Content-Type: application/json' \
  -d '{"path":"/data/documents/book.pdf","priority":100}'
```

服务器对路径做真实路径解析，拒绝越出 allowed-input-root 的文件及符号链接跳转。这个路径不是客户端电脑上的路径。

**上传原始文件字节（POST /v1/jobs/upload）：**

```bash
curl -X POST 'http://127.0.0.1:8080/v1/jobs/upload?extension=.pdf&priority=100' \
  -H "Authorization: Bearer $OCR_API_KEY" \
  -H 'Content-Type: application/octet-stream' \
  --data-binary '@/home/user/book.pdf'
```

请求体直接是**原始文件字节**，不是 JSON、base64 或 multipart 表单。查询参数 `extension` 必填，限定为[输入格式表](input-formats.md)中的受支持后缀；不使用客户端文件名构造任意路径。服务在接收 HTTP 数据块时将内容写入任务独立的暂存文件，而不是把整个文件放在请求内存。默认大小上限 64 MiB，可由 `--max-upload-bytes` 修改；超限返回 413，空内容返回 400。PDF 页数、像素限制沿用现有文档配置。

并发准入：`--max-active-jobs` 默认 64，包括尚在上传/提交的预留请求、排队、转换、推理及写出中的任务；达到上限立即返回 **429**，带 `Retry-After: 1`。`--max-inflight-upload-bytes` 默认 256 MiB，按收到的原始字节计量，覆盖上传中及已提交但未结束的任务；超过配额返回 **429**。提交失败、连接断开和任务结束都会归还相应配额。`--max-jobs` 默认 1000，是进程生命周期累计接收上限，达到后返回 **503**；修改活跃任务上限不会延长累计上限。429 的客户端可按 `Retry-After` 延迟并加抖动重试；503 累计上限需运维调整容量/重启，客户端不要无限重试。

两种提交成功均返回 **HTTP 202** 和：

```json
{"id":"<job-id>","status":"queued","status_url":"/v1/jobs/<job-id>"}
```

`priority` 为可选整数，范围 -1000000 至 1000000，默认 0，数值越大越优先。

## 查询状态、结果与图片

```bash
curl -H "Authorization: Bearer $OCR_API_KEY" http://127.0.0.1:8080/v1/jobs/<job-id>
curl -H "Authorization: Bearer $OCR_API_KEY" http://127.0.0.1:8080/v1/jobs/<job-id>/result
curl -H "Authorization: Bearer $OCR_API_KEY" \
  'http://127.0.0.1:8080/v1/jobs/<job-id>/result?format=markdown'
```

状态可能为 `queued`、`reading`、`processing`、`finalizing`、`succeeded` 或 `failed`。还返回 `priority`、`pages_queued`、`pages_completed`、`error` 和结果链接。结果只在 succeeded 后可用，未就绪返回 409、未知任务返回 404。结果 JSON/Markdown 与现有 CLI 保持同一格式。Markdown 的相对图片路径可映射到 `GET /v1/jobs/<job-id>/assets/<filename.png>`。 `GET /healthz` 仅检测 HTTP 服务存活，不代表 NPU 或远端 vLLM 健康。

`GET /v1/metrics` 返回受相同 API key 保护的 JSON 快照：`active_jobs`、`reserved_jobs`、`queued_documents`、`queued_pages`、`queued_page_bytes`、`reserved_page_bytes`、`inflight_upload_bytes` 以及 `admitted_total`、`succeeded_total`、`failed_total`、`rejected_total`。已接受任务满足 `admitted_total = succeeded_total + failed_total + active_jobs`；预留请求另见 `reserved_jobs`。计数器只覆盖当前进程，重启后清零，不是 Prometheus 文本格式。429 和累计容量 503 均计入 `rejected_total`。

## 调度和部署边界

`document-workers`（默认 2）控制同时进行文件读取、PDF 渲染、Office 转换的文档数，按待处理文件优先级领取任务；`max-queued-pages`（默认 2）约束就绪页面队列；`--max-queued-page-bytes`（默认 256 MiB）同时约束队列中 RGB 图像与拷贝预留字节，超过单页限制的渲染页使所属任务失败。读取线程完成页面拷贝前会预留名额与字节；其他线程可继续调度。`page-workers`（默认 4）限制同时在处理的页面数；`--box-workers`（默认 1，范围 1–128）是**所有页面共享**的 BOX 工作线程数，设置为大于 1 时单页多个 BOX 能同时识别，不会为每页再创建一套 BOX 线程。每页最多向全局池提交 `min(BOX 数, box-workers)` 个任务；布局仍先在页面线程完成。各模型还受自己的 `max_concurrent_requests`（默认 `instances`）限制。高优先级文件的**已就绪页面**优先取得空闲页面线程；BOX 池的排队不继承文件优先级。每个文件按页号和 BOX 原顺序汇总结果，文件失败不会中断其他任务；断开 HTTP 连接不取消已提交任务。

页面字节限制针对等待队列及其预留拷贝，不包含正在转换的子进程、读取线程当前页、正在推理的页面、BOX 裁剪图像和结果缓冲。需结合 `document-workers`、`page-workers`、`box-workers`、`document.max_pixels` 和容器内存预算设置，不能将该值直接当作进程 RSS 上限。HTTP 当前使用每连接一个服务线程，`--http-connections` 默认 128、可配 16–1024；提高该值会增加线程/FD 占用。应在活跃任务上限之外留出状态轮询和结果下载连接。

可在[模型池配置](configuration.md#模型池)里为每个模型设置 `batch_size` 和 `max_batch_wait_ms`。相同模型 ID 的跨文件、跨页 BOX 可共同组成一次原生批量推理；`max_concurrent_requests` 是同时运行的批次数上限。单页多 BOX + 单套 vLLM 服务可用 `instances: 1, max_concurrent_requests: 8`，并启动 `--page-workers 4 --box-workers 8 --document-workers 4`；即使仅有 1 页在 OCR 阶段，也能向后端发出最多 8 个 BOX 请求。vLLM 的客户端 `batch_size` 保持 1，具体并行度取决于 BOX 数和服务端容量。等待组批的页面仍计入 `page-workers` 并持有裁剪图像，不能只按已就绪页队列的字节上限估算进程内存。

本版单进程的任务状态仅在内存中，原始文件与成功输出保留在 data-dir 中；服务重启不会自动恢复旧任务，`max-jobs` 是进程生命周期内累计接受的任务上限。尚不支持删除/取消、运行中更改优先级、持久化队列、跨节点/多租户调度或幂等键。模型初始化在服务启动时进行。

ARM64 离线包在启用服务端构建的工作流通过后包含 `server.sh`，但不包含 CANN、NPU 驱动、模型权重或 vLLM 服务。310P3 + DocLayout + OvisOCR2 的服务器实机并发、长稳尚未验收，原先的 [ACL 退出 SIGSEGV Issue #2](https://github.com/Tan90degrees/OmniOCR/issues/2) 也仍未关闭。

## 输入格式

路径提交与二进制上传共用 [输入格式注册表](input-formats.md)，支持 PDF、扫描/拍摄图片（含多页 TIFF）、DOC/DOCX、PPT/PPTX、XLS/XLSX、RTF、ODT/ODS/ODP、EPUB、OFD、HTML/HTM、CSV。扩展名大小写不敏感。

`POST /v1/jobs/upload?extension=.csv` 仍接收原始文件二进制；不改变现有任务、优先级与结果 API。HTTP 202 表示入队，不代表转换成功；缺失 Calibre/OFD 工具或无效文件会通过任务的 failed/error 返回。上传 HTML 不能携带邻接资源目录，应使用自包含 HTML；路径提交可解析文件旁的相对图片。服务进程需有对应转换器和字体。

## 性能与容量

等待读取的文档使用独立优先堆，已经完成的任务仅保留查询状态，不参与调度扫描。页面内串行 BOX 直接运行在页面线程；模型池限制仍然生效。结果和图片通过文件响应传输，避免每次下载将整个文件读入并复制到内存。

累计任务上限、进程内状态、64 个 HTTP 连接上限及文档转换资源约束仍然存在；本轮未加入任务回收或持久队列。压测命令与优化路线见 [并发与性能](performance.md)。

## 相关文档

[REST 演示](getting-started.md#启动-rest-演示) · [输入格式](input-formats.md) · [调度原理](architecture.md)
