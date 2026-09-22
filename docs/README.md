# OmniOCR 文档

[返回项目首页](../README.md)

从首次运行到真实模型部署，按下面的任务选择文档。所有终端命令默认从仓库根目录执行，各文档中的相对链接以该文档位置为基准。

## 按任务阅读

| 我想做什么 | 阅读路径 |
|---|---|
| 先跑通一个文件 | [快速上手](getting-started.md) → [输入格式](input-formats.md) |
| 配置 Paddle/MinerU 与 BOX 模型 | [配置参考](configuration.md) → [示例配置](../configs/) |
| 部署到昇腾设备 | [本地后端构建](getting-started.md#启用本地模型) → [昇腾部署与记录](ascend.md) |
| 提供持续运行的 OCR 接口 | [REST 演示](getting-started.md#启动-rest-演示) → [REST API](server.md) |
| 一次处理多个文件并设置优先级 | [批处理](batch.md) → [调度原理](architecture.md) |
| 嵌入 C++ 或扩展模型 | [C++ 集成](architecture.md#c-集成) → [模型适配](configuration.md) |
| 验证功能与判断部署条件 | [测试与验证](testing.md) → [目标设备验证](ascend.md) |
| 在 ARM64 环境离线运行 | [离线包说明](../packaging/README.md) → [额外格式依赖](input-formats.md) |

## 文档分工

| 文档 | 维护内容 |
|---|---|
| [快速上手](getting-started.md) | 构建选项、依赖路径、CLI/REST 首次运行 |
| [输入格式](input-formats.md) | 后缀、转换器安装、编码与分页语义 |
| [配置与模型适配](configuration.md) | 字段默认值、模型池、路由、fallback、后端协议、结果结构 |
| [插件与 PP-DocLayoutV3](plugins.md) | v2 执行池绑定、几何、第三方 .so 与接口边界 |
| [REST 服务](server.md) | HTTP 接口、任务状态、部署参数与运行限制 |
| [批处理](batch.md) | jobs 清单、优先级与批次错误处理 |
| [架构与资源生命周期](architecture.md) | 实现边界、线程模型、资源管理、C++ API |
| [昇腾部署与验证](ascend.md) | OM/设备适配、真实硬件记录与待解决问题 |
| [测试与验证](testing.md) | 自动化命令、测试覆盖与验收边界 |
| [ARM64 离线包](../packaging/README.md) | 包内容、运行要求、分发步骤 |

## 维护约定

首页保留项目概览、最短运行路径和文档入口；参数、协议与部署细节放在相应主题文档。修改接口或行为时，同步更新负责该主题的文档和受影响的首页摘要，避免维护两份不同的参数表。

新增或移动文档时，更新本目录及相关文档链接；仓库内部使用相对链接，便于 GitHub 浏览和本地阅读。验证记录区分 Mock、真实转换器、真实模型和目标硬件，不能将接口测试结果写成准确率或性能结论。
