# Linux x64 / ARM64 离线包

[项目首页](../README.md) · [文档目录](../docs/README.md)

由 GitHub Actions 的 **Linux x64 and ARM64 offline packages** 工作流使用两种原生 Runner 分别构建，产生两个独立可下载的 Artifact：`omniocr-linux-x64-offline`（`x86_64`）和 `omniocr-linux-arm64-offline`（`aarch64`）。两个包均包含各自架构的 `omniocr`、
可选 REST 服务可执行程序 `omniocr-server`、其启动脚本 `server.sh`、
程序依赖的非 glibc 共享库、Poppler 的 `pdfinfo`/`pdftoppm`、LibreOffice、
配置与文档、`run.sh`、`verify.sh`。不包含模型权重、NPU 驱动和 CANN。

运行环境：与下载产物一致的 Linux x86_64 或 AArch64、与出包环境兼容的 glibc（两种架构的构建基线均为 Ubuntu 24.04）；执行模型需另行提供服务或模型文件。LibreOffice 的字体、
系统图形/Java 扩展和格式兼容不保证对所有文档完备。

下载 Actions artifact、解压后：

```bash
tar -xzf omniocr-*-linux-<x64或arm64>-offline.tar.gz
cd omniocr-*-linux-<x64或arm64>-offline
./verify.sh
./run.sh --config configs/demo.json --input /data/example.pdf --output /data/ocr-output
```

`run.sh` 和 `server.sh` 通过仅修改子进程的 `PATH`、`LD_LIBRARY_PATH` 优先使用包内工具与非 glibc 共享库，仍依赖兼容的宿主动态链接器与 glibc，不修改父 shell 环境。可用 `--config` 指定模型配置；REST API 参见 [REST 服务](../docs/server.md)。
当前发布工作流在两个架构下均关闭 ACL/ONNX，仅支持 HTTP/vLLM 和 Mock，完整运行依赖随对应架构产物打包。
ONNX/ACL 离线分发须另外提供匹配目标架构的 SDK/运行时及构建环境；
NPU 实机验证与普通 ARM64 构建分开。

`verify.sh` 根据 `PACKAGE-INFO` 校验宿主架构（x64/arm64）、CLI/REST ELF 机器类型、SHA256 清单和各自的 `--help`；
Actions 在打包后运行同样的检查和 demo 端到端冒烟测试。

REST 服务的部署示例：

```bash
export OCR_API_KEY='replace-with-a-strong-secret'
./server.sh --config configs/demo.json --data-dir /data/omniocr-state \
  --allowed-input-root /data/documents --host 127.0.0.1 --port 8080 --api-key-env OCR_API_KEY
```

当前两种架构的离线出包都关闭 ACL/ONNX，REST 二进制与 CLI 一样仅支持 HTTP/vLLM 或 Mock，适用于 HTTP/vLLM 模型；启用 ACL 的 310P3 实机服务仍需使用与目标 CANN 和处理器架构匹配的 SDK 在目标环境编译和验证，不能把普通 ARM64 出包作为 NPU 实测证明。

## 新增输入格式依赖

包内原生 libtiff 支持多页 TIF/TIFF；RTF、ODT/ODS/ODP、HTML/HTM、CSV 复用 LibreOffice。EPUB 需要匹配目标 ARM64 系统的 Calibre，OFD 需要 JRE 和 `tools/ofd-converter` 构建的 JAR；当前离线包不自动包含 Calibre/JRE/JAR，部署前需单独准备并通过 document.ebook_convert/ofd_converter 指定。不能把格式列表视为外部依赖已经安装。完整安装和验证步骤见 [输入格式](../docs/input-formats.md)。

## 双架构构建与发布

工作流文件为 [`package-arm64.yml`](../.github/workflows/package-arm64.yml)（保留历史文件名，**工作流实际同时构建 x64、arm64**）。可在 GitHub Actions 手动运行，或随 `main`、开发分支及 `v*` 标签推送自动执行。两个 matrix job 分别使用原生 `ubuntu-24.04`（x64）和 `ubuntu-24.04-arm`（ARM64），每个 job 自行执行 CMake/CTest、REST/V3 HTTP/外部 ABI 插件集成测试、离线包校验和 Demo 冒烟，最后独立上传 `.tar.gz` 与对应 `.sha256`。

在目标原生 Linux 主机本地构建可使用统一脚本（`x64` 对应 `x86_64`，`arm64` 对应 `aarch64`）：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \\
  -DOMNIOCR_WITH_SERVER=ON -DOMNIOCR_WITH_ACL=OFF -DOMNIOCR_WITH_ONNX=OFF
cmake --build build -j2
ctest --test-dir build --output-on-failure
bash packaging/make-linux.sh build /tmp/omniocr-linux-<x64或arm64> <x64或arm64>
(cd /tmp/omniocr-linux-<x64或arm64> && ./verify.sh)
```

通用打包程序会检查宿主机器和编译器架构是否匹配请求的包类型，不允许直接把 x64 编译产物标记成 arm64（反之亦然）。编译输出与动态插件均由目标原生架构构建，不能混用 x64 和 ARM64 的 `.so`。包内不携带 glibc、NPU 驱动、CANN、ONNX SDK；**Ubuntu 24.04 上打包的通用离线包不保证在较老的 EulerOS/openEuler 上启动**。如果部署目标是 EulerOS 2.0 SP13，请在兼容该环境 glibc 的原生构建环境重新打包并进行依赖、PDF/Office 和 ACL 实机验收。
