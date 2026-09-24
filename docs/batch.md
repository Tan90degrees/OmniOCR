# 批处理与优先级调度

[项目首页](../README.md) · [文档目录](README.md)

在仓库根目录运行以下命令。需要运行中继续提交文件时，请使用 [REST 服务](server.md)。

批处理使用**一个** Pipeline 和共享模型池，不需要对每个文件单独启动 OmniOCR 进程。
例如创建 `jobs.json`（输入和输出路径相对于此清单所在目录）：

```json
{
  "options": {
    "page_workers": 4,
    "box_workers": 8,
    "max_active_documents": 2,
    "max_queued_pages": 2
  },
  "jobs": [
    {"input": "in/normal.pdf", "output": "out/normal", "priority": 0},
    {"input": "in/urgent.pdf", "output": "out/urgent", "priority": 100},
    {"input": "in/notes.docx", "output": "out/notes", "priority": 20}
  ]
}
```

```bash
./build/omniocr --config configs/demo.json --batch /path/to/jobs.json --format both
```

上述 demo 配置用于验证调度，真实 OCR 请换成已适配的模型配置。单文件 `--input/--output` 命令保持兼容；`--batch` 与这两个参数不可并用。清单可直接写成任务数组，也可使用示例中的 `jobs/options` 对象。优先级越大，**已就绪**的页面越先调度；不抢占正在执行的页面。等优先级按入队顺序执行，各文件返回和写出的页序仍按原始页码排列。

`page_workers` 默认取 `execution.workers`；`box_workers` 默认 1，设置为大于 1 时创建**全批次共享**的固定 BOX 工作池，单页内不同 BOX 可同时调用模型；`max_active_documents` 默认 2，`max_queued_pages` 默认 2。全局 BOX 并发最多为 `box_workers`，不会按 `page_workers × box_workers` 新建线程；NPU/vLLM 模型还受到各自 `max_concurrent_requests`（默认 `instances`）池大小约束。PDF/Office 转换子进程与页面推理可并行，总进程 CPU/内存仍需按文档尺寸配置。一个文件失败时，其他文件继续写结果；整个批次若有文件级失败，CLI 返回 1；仅有 `record` 模式 BOX 失败返回 2；全部成功返回 0。各任务输出目录必须互不重叠。

此接口处理**一次性提交的批次**，暂不支持任务运行期间动态插入或调整优先级、跨进程持久化队列和页面级强制抢占。详见[调度说明](architecture.md)。


## 相关文档

[配置与实例池](configuration.md) · [调度与资源生命周期](architecture.md) · [测试与验证](testing.md)
