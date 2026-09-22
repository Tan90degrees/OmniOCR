# 模型、执行后端与几何插件（开发中的 v2 接口）

[项目首页](../README.md) · [配置参考](configuration.md) · [Ascend 验证](ascend.md)

## 已实现的插件边界

`include/omniocr/plugins.hpp` 提供内置 C++ 注册表：布局解析 `LayoutAdapter`、识别结果 `RecognitionAdapter`、裁剪 `CropAdapter`、执行实现 `BackendFactory`。部署启动时注册，运行期不会对单个文件动态加载任意代码。主流水线只请求注册 ID；BOX 类型映射、实例共享、候选回退及全局页面调度沿用旧机制。

内置布局适配：`paddle`、`mineru`、`normalized`（兼容旧版）及 **`paddle.doclayout_v3.http`**。后者消费已经由 Paddle 推理服务后处理的 `boxes` JSON（或 `res.boxes`），支持 `label`、`coordinate`、`score`、`polygon_points`、`order: null`。这是 **HTTP 后处理结果适配**，并非 Paddle V3 ONNX/OM 原始 logits/masks/排序头解码器，也不是声明官方权重已在目标 NPU 完成精度/性能验收。

默认识别适配：`text`、`table`（OTSL 转 HTML）、`vlm.ovisocr2`、`vlm.generic`、`ctc` 与 `table.otsl`。它们仅规范已有推理结果；例如 `vlm.ovisocr2` 处理 `{"text":"..."}`，不自动在进程内加载权重，也不启动 vLLM。可调用 `register_recognition_adapter` 增加自定义协议。

内置几何适配：`bbox_crop`、`polygon_mask_crop`。掩膜以原始页坐标判断像素中心，轮廓外默认填白（`mask_background: 0..255`）；输入未提供轮廓时默认报错，只有显式设置 `crop_fallback: "bbox_crop"` 才降级，并在输出 `extensions["omniocr.crop_fallback"]` 记录。多边形超出检测矩形时采用独立的 `crop_bbox` 以防裁掉轮廓；原始 `bbox` 不变。多边形掩膜不是倾斜文字矫正、四点透视变换或曲面展开。

请求级 `TransformContext` 把像素、归一化或 `model_input` 坐标映射回原始页面；后者从 `layout.image_size` 建立默认缩放矩阵，也可显式配置 `layout.transform.matrix` 的 3×3 **模型输入 → 页面**齐次矩阵，用于指定 padding/透视。非平面曲面需专门网格映射实现，当前不支持。

## v2 配置和结果 schema 独立

`version: 1` 旧配置继续受支持；`version: 2` 把 `executors`、`models` 绑定和 `pipeline` 拆分。读取配置时归一化为现有执行池：同一 `executor` ID 的不同模型绑定共用一个池，`max_inflight` 转为实例/请求槽位，`backend: openai_chat` 映射已有 vLLM HTTP 实现，结果 `model` 保留逻辑绑定 ID。使用 [PP-DocLayoutV3 + OvisOCR2 v2 配置示例](../configs/doclayout-v3-http-ovisocr2.v2.json)，修改 HTTP 端点和真实 served model name 后运行：

```bash
python tools/paddle_layout_server.py --model PP-DocLayoutV3 --device cpu --port 8001
./build/omniocr --config configs/doclayout-v3-http-ovisocr2.v2.json --validate
./build/omniocr --config configs/doclayout-v3-http-ovisocr2.v2.json --input example.png --output out/v3
```

Paddle 环境必须安装经验证的匹配版本并固定权重 revision、预测选项；`--validate` 不会启动服务或检测权重。当前 CI 只测试自建协议夹具，不包含真实 V3 权重。

`output.schema_version` 可独立设为 1 或 2；不指定时，旧矩形流水线维持 schema v1，出现 V3 几何或空阅读顺序时自动写 v2。v2 每个 block 增加 `polygon`、`crop_bbox`、`reading_order`（可为 null）、`source_index`、`provenance`、`extensions`，其 ID 使用稳定的原始返回索引 `p{page}-s{source_index}`；v1 故意只导出 bbox 和旧字段，会丢掉这些信息。显式 null 阅读顺序的区域保留在 JSON 中，但默认不进入 Markdown 正文。

## 外部插件：稳定 C ABI v1

`include/omniocr/plugin_abi.h` 是 C 语言头。编译产物只需在可信部署配置中登记（不允许 OCR 请求覆盖库路径）：

```json
{
  "version": 1,
  "plugins": [{"id":"example.external","library":"./plugins/libexample_backend.so"}],
  "models": {
    "external_ocr": {"backend":"example.external","instances":2}
  }
}
```

这段 JSON 只展示相关字段，还需填入有效的 `layout`、`routes` 等配置。可参照 `tests/external_plugin_integration.py` 在**仓库之外**编译 `tests/plugins/sample_backend.c`，生成独立 `.so`，无需重新构建 OmniOCR 主程序即可通过单文件 CLI、batch、REST 使用外部 OCR 结果。

导出 `omniocr_plugin_entry_v1`，返回 `omniocr_plugin_api_v1` 函数表。支持 `kind: backend/layout/recognition`；`create/execute/release/destroy` 采用 opaque handle、显式 buffer 长度与状态码。**插件侧分配的所有输出/错误 buffer 必须由插件侧 release，错误不得跨 C ABI 抛出 C++ 异常**。调用方在模型实例和请求全部结束后释放句柄；插件共享库在进程退出时卸载，暂不支持热替换。layout 外部插件返回 Paddle 风格的统一 `boxes` JSON；recognition 返回 `text/raw_text`；backend 收到图像 data URL + prompt 的 JSON 请求并返回 JSON。32 MiB 的响应上限不是通用大张量传输协议。

**安全边界：加载 .so 等于执行该库代码**；只从管理员信任的部署配置加载，并核验文件来源与签名，不能交给不可信 REST 调用方。当前 ABI 提供的是 JSON/图像级插件扩展：大规模 host/device Typed Tensor 零拷贝 ABI、独立外部服务 RPC 规范、动态 tensor dtype/shape、多阶段任务 DAG 尚未交付。内置 ACL/ONNX 仍局限现有 float32 静态适配；V3 ONNX/OM 必须在权重导出、输出解码和目标 Ascend 芯片实际验收后另行实现。

## 验收与当前范围

CI 应分别证明：旧 v1 配置/输出不退化、V3 的 null/缺失 order 与原始索引、轮廓和裁剪一致、explicit transform、无效/退化 polygon 被拒绝、JSON v2 不丢信息、外部 C ABI 在 CLI/batch/REST 都可用。本地测试并不替代真实 V3 权重、MinerU、动态 OM、310P3/910B 的兼容性与长稳测试；已有 ACL 退出 SIGSEGV 继续由 [Issue #2](https://github.com/Tan90degrees/OmniOCR/issues/2) 单独跟踪。
