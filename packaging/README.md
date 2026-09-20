# Linux ARM64 离线包

由 GitHub Actions 的 `ARM64 offline package` 工作流生成。包包含 AArch64 `omniocr`、
程序依赖的非 glibc 共享库、Poppler 的 `pdfinfo`/`pdftoppm`、LibreOffice、
配置与文档、`run.sh`、`verify.sh`。不包含模型权重、NPU 驱动和 CANN。

运行环境：Linux AArch64、与出包环境兼容的 glibc（构建基线 Ubuntu 22.04 ARM64）、
可运行 ARM64 ELF 的内核；执行模型需另行提供服务或模型文件。LibreOffice 的字体、
系统图形/Java 扩展和格式兼容不保证对所有文档完备。

下载 Actions artifact、解压后：

```bash
tar -xzf omniocr-*-linux-arm64-offline.tar.gz
cd omniocr-*-linux-arm64-offline
./verify.sh
./run.sh --config configs/demo.json --input /data/example.pdf --output /data/ocr-output
```

`run.sh` 通过打包的动态链接器及 `lib/` 优先加载包内库，
不改变主机的 LD_LIBRARY_PATH。可通过 `--config` 指定任意配置。
打包的 CLI 默认关闭 ACL/ONNX，仅支持 HTTP/vLLM 和 mock。
ONNX/ACL 离线分发须另外提供匹配的 ARM64 SDK/运行时及构建环境；
NPU 实机验证与普通 ARM64 构建分开。

`verify.sh` 检查本机架构、二进制 ELF 类型、SHA256 清单和 `--help`；
Actions 在打包后运行同样的检查和 demo 端到端冒烟测试。
