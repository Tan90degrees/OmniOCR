# Linux ARM64 离线包

由 GitHub Actions 的 `ARM64 offline package` 工作流生成。包包含 AArch64 `omniocr`、
可选 REST 服务可执行程序 `omniocr-server`、其启动脚本 `server.sh`、
程序依赖的非 glibc 共享库、Poppler 的 `pdfinfo`/`pdftoppm`、LibreOffice、
配置与文档、`run.sh`、`verify.sh`。不包含模型权重、NPU 驱动和 CANN。

运行环境：Linux AArch64、与出包环境兼容的 glibc（构建基线 Ubuntu 24.04 ARM64）、
可运行 ARM64 ELF 的内核；执行模型需另行提供服务或模型文件。LibreOffice 的字体、
系统图形/Java 扩展和格式兼容不保证对所有文档完备。

下载 Actions artifact、解压后：

```bash
tar -xzf omniocr-*-linux-arm64-offline.tar.gz
cd omniocr-*-linux-arm64-offline
./verify.sh
./run.sh --config configs/demo.json --input /data/example.pdf --output /data/ocr-output
```

`run.sh` 和 `server.sh` 通过仅修改子进程的 `PATH`、`LD_LIBRARY_PATH` 优先使用包内工具与非 glibc 共享库，仍依赖兼容的宿主动态链接器与 glibc，不修改父 shell 环境。可用 `--config` 指定模型配置；REST API 参见 `docs/server.md`。
当前发布工作流的 CLI 关闭 ACL/ONNX，仅支持 HTTP/vLLM 和 mock。
ONNX/ACL 离线分发须另外提供匹配的 ARM64 SDK/运行时及构建环境；
NPU 实机验证与普通 ARM64 构建分开。

`verify.sh` 检查本机架构、CLI/REST 二进制 ELF 类型、SHA256 清单和各自的 `--help`；
Actions 在打包后运行同样的检查和 demo 端到端冒烟测试。

REST 服务的部署示例：

```bash
export OCR_API_KEY='replace-with-a-strong-secret'
./server.sh --config configs/demo.json --data-dir /data/omniocr-state \
  --allowed-input-root /data/documents --host 127.0.0.1 --port 8080 --api-key-env OCR_API_KEY
```

当前 ARM64 发布工作流的 REST 二进制与 CLI 一样关闭 ACL/ONNX，适用于 HTTP/vLLM 模型；启用 ACL 的 310P3 实机服务仍需使用与目标 CANN 匹配的 ARM64 SDK 在目标环境编译和验证，不能把普通 ARM64 出包作为 NPU 实测证明。

## 新增输入格式依赖

包内原生 libtiff 支持多页 TIF/TIFF；RTF、ODT/ODS/ODP、HTML/HTM、CSV 复用 LibreOffice。EPUB 需要匹配目标 ARM64 系统的 Calibre，OFD 需要 JRE 和 `tools/ofd-converter` 构建的 JAR；当前离线包不自动包含 Calibre/JRE/JAR，部署前需单独准备并通过 document.ebook_convert/ofd_converter 指定。不能把格式列表视为外部依赖已经安装。完整安装和验证步骤见 docs/input-formats.md。
