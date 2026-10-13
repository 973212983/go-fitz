# invoicefile 维护的 go-fitz 分支

基线：github.com/gen2brain/go-fitz v1.28.2（提交 1c84212）。本仓库 fork 自上游，改动在 invoicefile 分支，按 v1.28.2-invoicefile.N 打标签。模块路径仍为 github.com/gen2brain/go-fitz，invoicefile 项目在 go.mod 中用 `replace github.com/gen2brain/go-fitz => github.com/973212983/go-fitz <标签>` 引用，代码中的 import 不变。

新增的 ImageJPEG(page, dpi, quality) 在 CGO 构建中由 C 完成绘制和 JPEG 编码，只把压缩后的字节复制到 Go，没有整页 Go 像素缓冲。原 ImageDPI/PNG 等 API 保留。nocgo 版本使用原 ImageDPI + Go JPEG 编码作为兼容回退。

修改集中在 fitz_jpeg_cgo.go、fitz_jpeg_nocgo.go、fitz_jpeg_fast.c、fitz_jpeg_render.c、fitz_jpeg_encode.c/.h 和 fitz_jpeg_*test.go；include/jpeg 增加匹配 MuPDF 1.28.0 静态库的 IJG 头文件。上游源码、include/mupdf、libs、testdata 与 COPYING 保留。libs 约 258 MiB，包含上游全部平台的预编译静态库，版本与头文件匹配，干净检出即可构建，无运行期下载或系统 MuPDF 依赖。来源、静态库校验值见 UPSTREAM.json。

## 绘制（fitz_jpeg_render.c）

页面画在不透明白底、无 alpha 的 RGB 像素图上，与 RGBA 绘制的像素完全相同，但更快、少 1/4 内存。解释器与 draw device 之间加了一层过滤设备：

- 跨文档复用图片解码结果。发票每页都有相同的监制章（含 SMask）、标题图等，Flate + PNG 预测器解码原先占绘制时间的大半。8 位 Flate、DeviceGray/DeviceRGB 或纯 alpha 蒙版、无 Decode/色键/Matte 的图片包一层代理 fz_image；其 get_pixmap 对完全相同的压缩数据和请求（子区域、目标尺寸、l2factor）返回缓存 tile 的副本，否则调用原图片解码。之后 fz_get_pixmap_from_image 与 draw device 的流程不变，像素与不用缓存时完全一致。缓存是进程内 LRU（最多 64 项、16 MiB），只存 malloc 的纯数据，不跨 fz_context 共享 MuPDF 对象；同一请求第二次出现才入缓存，二维码这类一次性图片不占位置。命中时还会逐字节比较压缩数据，不依赖哈希。
- 跳过整页透明组。页面上只要有带 SMask 的图片，MuPDF 就把整页包进隔离透明组（整页缓冲 + 一次合成）。在不透明白底上，普通混合、alpha 为 1 的隔离组与直接绘制只差 8 位舍入：像素最大差 2/255。页面里若还出现其它透明组、软蒙版或平铺图案，就保留透明组重新绘制该页，结果与 MuPDF 原路径相同。

## JPEG 编码（fitz_jpeg_fast.c）

快速编码器与 MuPDF 1.28.0 内置 libjpeg 10 在 jpeg_set_defaults() + jpeg_set_quality(q, TRUE) 下的输出逐字节相同：JFIF 1.01、YCbCr 4:2:0（色度用 16x16 缩放 DCT 降采样，即 do_fancy_downsampling）、JDCT_ISLOW、顺序模式、标准霍夫曼表。颜色转换、DCT（移植自 jfdctint.c）、量化舍入、边缘扩展、虚拟块、霍夫曼编码与标记段均按 libjpeg 实现。票面大多是白纸和黑字：样本全部相等的块只有 DC 系数，直接按公式计算并跳过 DCT；R = G = B 的像素在 libjpeg 定点转换下 Cb = Cr = 128；其余块走完整流程。两种 DCT 的中间量经仿射区间分析均小于 2^31 的 0.7 倍，32 位与 64 位 long 的 libjpeg 结果一致。

fitz_jpeg_encode.c 保留原 libjpeg 编码路径（派生自 MuPDF 1.28.0 output-jpeg.c，普通 JPEG、4:2:0），作为测试中的参考实现，生产路径不再调用。

新增头文件来源：ArtifexSoftware/thirdparty-libjpeg 的 627faae398468d5d11e3d694dc325432f734b5c4（MuPDF 1.28.0 的 submodule commit），以及 MuPDF 1.28.0 的 scripts/libjpeg/jconfig.h、source/fitz/jmemcust.h。保留 IJG README 与 MuPDF COPYING；This software is based in part on the work of the Independent JPEG Group。

## 测试与更新

fitz_jpeg_internal_test.go 覆盖：合成图像（多种尺寸、内容、质量，RGB 与 RGBA）快速编码器与 libjpeg 逐字节一致；渲染页面两种编码器一致；图片缓存命中前后与 MuPDF 直接绘制逐像素一致；跳过透明组的差异上限与含混合模式页面的回退；多 goroutine 并发渲染结果一致。

更新上游时：把 invoicefile 分支变基到新的上游标签，保留 COPYING/AUTHORS，按新版本同步整套头文件与静态库，执行本模块全部测试和 invoicefile 的 gen 测试，并检查 Windows amd64 与 Linux musl 静态构建，以及真实票面质量/性能；然后打新标签（如 v1.29.0-invoicefile.1），再更新 invoicefile 项目 go.mod 中的 replace 版本。修改本分支后同样需要打新标签，标签一经发布不要移动（Go 模块代理与校验和数据库会缓存）。libjpeg 版本或默认参数变化时，以 TestEncodeJPEGMatchesLibjpeg 为准调整 fitz_jpeg_fast.c；MuPDF 的图片解码或透明组逻辑变化时，以 TestImageCacheKeepsPixels、TestPageGroupSkip 为准复核 fitz_jpeg_render.c。

验证命令（在本仓库根目录执行；Windows 需 MinGW）：

```
go test ./...
```

Linux 用 CGO_ENABLED=1 和 -tags musl；invoicefile 项目的 build.cmd/buildver.bat 配置了 Zig musl 交叉编译。
