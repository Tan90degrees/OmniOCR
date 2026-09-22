# 文件提取格式

[项目首页](../README.md) · [文档目录](README.md)

所有输入统一为页面 RGB 图像，再执行版面识别、BOX 路由和 Markdown/JSON 输出。单文件 CLI、批处理和 REST 复用同一个 reader 与扩展名注册表，大小写不敏感。这里的“提取”是视觉页面 OCR，不是直接导出 Office 对象模型、Excel 公式或电子书 DOM。

| 输入 | 处理方式 | 依赖 |
|---|---|---|
| PDF（含扫描 PDF） | Poppler 逐页渲染 | pdfinfo、pdftoppm |
| 扫描/拍摄图片 | PNG/JPG/JPEG/BMP/PPM/PGM/TGA 直接解码 | 内置 stb |
| TIF/TIFF | 逐个主 IFD 解码为页面，保留页序 | 原生 libtiff |
| DOC/DOCX、PPT/PPTX、XLS/XLSX | LibreOffice → PDF | LibreOffice、Poppler |
| RTF、ODT/ODS/ODP | LibreOffice → PDF | LibreOffice、Poppler |
| HTML/HTM | 静态页面 → PDF | LibreOffice Writer、Poppler |
| CSV | C++ 解析字面单元格 → HTML 表格 → PDF | LibreOffice Writer、Poppler |
| EPUB | ebook-convert → PDF | Calibre、Poppler |
| OFD | OFDRW/PDFBox → PDF | JRE、转换器 JAR、Poppler |

## 安装与配置

Ubuntu 示例；EulerOS/其他昇腾主机请安装对应架构的软件包并确认字体：

```bash
sudo apt-get install libtiff-dev poppler-utils libreoffice calibre default-jdk maven
mvn -B -f tools/ofd-converter/pom.xml package
export PATH="$PWD/tools:$PATH"
./build/omniocr --list-formats
```

OFD 工具源代码在 `tools/ofd-converter/`，固定 OFDRW 2.3.8，使用 PDFBox 导出器。默认 wrapper 寻找旁边 `ofd-converter/target/omniocr-ofd-converter.jar`；安装时可以把 JAR 放到固定目录，再设置 `OMNIOCR_OFD_JAR`。运行不需要 Maven，只需 Java 8+；构建使用 JDK 11+。JVM 堆上限为 512 MiB，可复制 wrapper 调整，但进程总内存还包括堆外内存。第三方库许可证随依赖保留，分发时应保留对应声明。

配置示例（合并到现有模型配置）：

```json
{
  "document": {
    "soffice": "soffice",
    "pdfinfo": "pdfinfo",
    "pdftoppm": "pdftoppm",
    "ebook_convert": "ebook-convert",
    "ofd_converter": "/opt/OmniOCR/tools/omniocr-ofd-to-pdf",
    "csv_delimiter": ",",
    "max_csv_bytes": 16777216,
    "dpi": 150,
    "max_pages": 1000,
    "max_pixels": 40000000,
    "timeout_seconds": 120
  }
}
```

两个外部转换器的调用契约都是 `EXECUTABLE INPUT_FILE OUTPUT_PDF`，不经 shell；路径可以包含空格，不能把 `java -jar ...` 填入可执行文件字段。可使用随仓库提供的 wrapper 或符合相同契约的自有转换器。转换器字段是管理员配置，不接收 REST 请求中的任意命令。缺失程序、超时、非零退出、空产物或无效 PDF 都作为文档错误返回。

无桌面环境的 Calibre 建议设置 `QT_QPA_PLATFORM=offscreen` 并以非 root 用户运行。DRM/IRM、密码保护或损坏文件不绕过保护，无法读取时明确失败。转换器的字体及渲染兼容性会影响 OCR 结果。

## 分页与内容语义

- DOC/PPT/Excel/ODF 使用转换器打印视图；隐藏内容、备注、公式表达式和宏不属于页面 OCR 输出。电子书、HTML、CSV 是流式内容，页码对应渲染后的页码，不是源文件固有页码。
- CSV 必须为 UTF-8（可含 BOM），支持带引号分隔符、双引号转义、CRLF/LF、单元格内换行和尾部空列；可配置 `,`、`;`、tab、`|`。数字和 `=...` 都作为文本渲染，不做数值推断或公式求值。格式不合法或编码不支持时拒绝；GBK 等编码应先转 UTF-8。JSON 仍是 OCR BOX schema，不额外保证恢复原 CSV 的每个单元格值。
- HTML 使用 LibreOffice 的静态布局，不执行浏览器 JavaScript，不保证现代 CSS 与 Chromium 一致。路径输入直接读取原 HTML，以保留相对图片资源；REST 上传只有单文件，需自包含资源。不递归抓取网站。
- OFD 验证覆盖真实两页向量文档的转换与页序；字体、印章、嵌入图片、复杂票据仍需业务样本验收。转换不等于验证数字签名。
- TIFF 按主 IFD 页序处理，页数与像素上限在解码前检查；不把缩略图 SubIFD 当成额外页面。JPEG/PNG 等不会自动做拍摄透视矫正，WebP/HEIC 当前不在输入列表。
- 页数/像素限制约束 OCR reader；Office/Calibre/OFD 在产出 PDF 前仍可能占用较多资源，超时只限制运行时间。部署服务时应给转换进程设置容器内存/磁盘限额，并隔离不可信文件的网络和宿主文件访问；白名单输入路径本身不能限制文档中的外部资源引用。

## 回归验证

```bash
python3 -m pip install pillow python-docx python-pptx openpyxl reportlab
ctest --test-dir build --output-on-failure
python3 tests/document_integration.py build/omniocr
python3 tests/input_formats_integration.py build/omniocr \
  --external --ofd-converter tools/omniocr-ofd-to-pdf
python3 tests/server_integration.py build/omniocr-server --formats
```

`--external` 强制要求真实 Calibre/OFD 工具，不会跳过缺失依赖。格式测试实际渲染文件并检查非空白页面、TIFF/OFD 页序、HTML 相对图片、资源限制、转换假成功和多格式批次；版面使用 mock，因此不能据此宣称真实 OCR 识别精度通过。Linux CI 包含这些步骤；ARM64 包的基础镜像仍需单独配置 Calibre/JRE/JAR 并做目标机验收。

参考：[Calibre CLI](https://manual.calibre-ebook.com/generated/en/ebook-convert.html)、[OFDRW 导出器](https://github.com/ofdrw/ofdrw/blob/master/ofdrw-converter/doc/EXPORTER.md)。

## 相关文档

[快速上手](getting-started.md) · [REST 上传](server.md) · [测试与验证](testing.md)
