# 发票 JPEG 渲染的 go-fitz 分支

基线：github.com/gen2brain/go-fitz v1.28.2（提交 1c84212）。本仓库 fork 自上游，改动在 jpeg 分支，打常规版本号标签（当前 v1.28.6，基于上游 v1.28.2）。模块路径仍为 github.com/gen2brain/go-fitz，使用方在 go.mod 中用 `replace github.com/gen2brain/go-fitz => github.com/973212983/go-fitz <标签>` 引用，代码中的 import 不变。

新增的 ImageJPEG(page, dpi, quality) 在 CGO 构建中由 C 完成绘制和 JPEG 编码，只把压缩后的字节复制到 Go，没有整页 Go 像素缓冲。原 ImageDPI/PNG 等 API 保留。nocgo 版本使用原 ImageDPI + Go JPEG 编码作为兼容回退。

修改集中在 fitz_jpeg_cgo.go、fitz_jpeg_nocgo.go、fitz_jpeg_fast.c、fitz_jpeg_turbo.c、fitz_jpeg_render.c、fitz_jpeg_encode.c/.h 和 fitz_jpeg_*test.go；include/jpeg 增加匹配 MuPDF 1.28.0 静态库的 IJG 头文件，include/turbojpeg、libs/libturbojpeg_*.a 与 scripts/build-turbojpeg.sh 为 libjpeg-turbo 编码器。上游源码、include/mupdf、libs、testdata 与 COPYING 保留。libs 约 258 MiB，包含上游全部平台的预编译静态库，版本与头文件匹配，干净检出即可构建，无运行期下载或系统 MuPDF 依赖。来源、静态库校验值见 UPSTREAM.json。

## 绘制（fitz_jpeg_render.c）

页面画在不透明白底、无 alpha 的 RGB 像素图上，与 RGBA 绘制的像素完全相同，但更快、少 1/4 内存。解释器与 draw device 之间加了一层过滤设备：

- 跨文档复用图片解码结果。发票每页都有相同的监制章（含 SMask）、标题图等，Flate + PNG 预测器解码原先占绘制时间的大半。8 位 Flate、DeviceGray/DeviceRGB 或纯 alpha 蒙版、无 Decode/色键/Matte 的图片包一层代理 fz_image；其 get_pixmap 对完全相同的压缩数据和请求（子区域、目标尺寸、l2factor）返回缓存 tile 的副本，否则调用原图片解码。之后 fz_get_pixmap_from_image 与 draw device 的流程不变，像素与不用缓存时完全一致。缓存是进程内 LRU（最多 64 项、16 MiB），只存 malloc 的纯数据，不跨 fz_context 共享 MuPDF 对象；同一请求第二次出现才入缓存，二维码这类一次性图片不占位置。命中时还会逐字节比较压缩数据，不依赖哈希。
- 跳过整页透明组。页面上只要有带 SMask 的图片，MuPDF 就把整页包进隔离透明组（整页缓冲 + 一次合成）。在不透明白底上，普通混合、alpha 为 1 的隔离组与直接绘制只差 8 位舍入：像素最大差 2/255。页面里若还出现其它透明组、软蒙版或平铺图案，就保留透明组重新绘制该页，结果与 MuPDF 原路径相同。

- 复用页面像素缓冲。150 DPI 的 A4 页面约 6.5 MiB，每页重新申请会映射新内存并逐页缺页，开销与清白底相当。invoice_render_page 从进程内池取 malloc 缓冲（fz_new_pixmap_with_bbox_and_data），结果必须用 invoice_drop_page_pixmap 释放并归还；池最多保留 4 块空闲缓冲（单块不超过 64 MiB，满了保留较大的），常驻内存上限约为 4 页。
- Document.DropToUnicode()：在内存中删除各页资源里嵌入 Type0 字体（FontFile/FontFile2/FontFile3）的 /ToUnicode。MuPDF 加载此类字体时会把 Identity 编码的 65536 个码位逐个经 ToUnicode 重映射，占发票字体加载的大半。嵌入 CID 字体的字形只由 CID 决定，像素不变；但之后同一 Document 的文本提取失去 Unicode 映射，所以只在仅用于渲染的文档上、渲染前调用。表单 XObject 内的字体不处理。nocgo 版本为空操作。

## JPEG 编码

### 独立 RGB 编码子包

`turbojpeg.EncodeRGB(pix, width, height, stride, Options{Quality: 90, DPI: 150})`
直接编码调用方持有的 RGB 像素，支持行填充，不导入根包、不链接 MuPDF。
Windows/amd64 和 Linux/amd64 的 cgo 构建只链接现有的 libturbojpeg 静态库；
`musl` 标签选择对应的 Linux 库。其余平台、CGO_ENABLED=0 或 `nocgo` 标签
使用 image/jpeg 回退。两条路径均输出基线 4:2:0 JPEG，并写入 JFIF DPI。
`turbojpeg.Available()` 可检查是否使用原生编码器。编码参数与 ImageJPEG 共用
`include/turbojpeg/fitz_turbojpeg.h` 里的 `fitz_tj_compress`，改参数只改这一处。
压缩器句柄和输出缓冲在调用之间复用（最多缓存 16 个）。DPI 先四舍五入，
结果须在 1–65535 之间。`include/turbojpeg` 和 `libs` 由 `required` 标签的
空导入带进 `go mod vendor`。

测试：`go test ./turbojpeg`、`go test -tags nocgo ./turbojpeg`，涵盖尺寸、
行填充、参数错误、DPI 取整、色度采样、并发一致性，以及 windows/amd64、
linux/amd64 的 cgo 构建确实链接了 libjpeg-turbo。改了 include/ 下的头文件后
要用新的 GOCACHE 测试。

ImageJPEG 默认用 libjpeg-turbo（fitz_jpeg_turbo.c），仅 windows/amd64、linux/amd64（glibc 与 musl）带预编译库；其余平台用下面的快速编码器，输出与 libjpeg 逐字节相同。

### libjpeg-turbo（fitz_jpeg_turbo.c）

libjpeg-turbo 3.2.0 官方源码包（SHA-256 见 scripts/build-turbojpeg.sh）编译的静态库，启用 SIMD（x86-64 SSE2/AVX2），只编 TurboJPEG 所需部分。参数与另两个编码器一致：基线、YCbCr 4:2:0、精确整数 DCT（JDCT_ISLOW）、标准霍夫曼表、JFIF 密度为像素图分辨率；RGBA 的 alpha 当填充跳过。区别是色度降采样：libjpeg 9 用 16x16 缩放 DCT，libjpeg-turbo 用 2x2 平均，所以字节不同，解码像素相差几个灰阶。发票样本（10 种票面、14 页，150 DPI、质量 90）上编码快 1.6–1.7 倍，渲染加编码整页快 1.3–1.4 倍，文件小 0.2%，PSNR 高 0.02–0.3 dB。

静态库里所有全局符号都改名加了 fitz_ljt_ 前缀（objcopy --redefine-syms，MinGW 的 .refptr 存根同步改名），与 libmupdfthird 里的 libjpeg 9 不冲突；include/turbojpeg/fitz_turbojpeg.h 把用到的 TurboJPEG 函数映射到前缀名，C 代码包含它而不是 turbojpeg.h。三个库只依赖 libc。Windows 用 MinGW gcc 编译，Linux 用 zig cc 交叉编译（musl 与 glibc 2.17），去掉调试信息。重新生成：在 Windows 的 Git Bash 中执行 scripts/build-turbojpeg.sh（需 MinGW-w64、cmake、nasm、zig、curl；RUN_TESTS=1 时跑 libjpeg-turbo 自带的回归测试，3.2.0 的 314 项全部通过），然后更新 UPSTREAM.json 中的校验值。

### 快速编码器（fitz_jpeg_fast.c）

快速编码器与 MuPDF 1.28.0 内置 libjpeg 10 在 jpeg_set_defaults() + jpeg_set_quality(q, TRUE) 下的输出逐字节相同：JFIF 1.01、YCbCr 4:2:0（色度用 16x16 缩放 DCT 降采样，即 do_fancy_downsampling）、JDCT_ISLOW、顺序模式、标准霍夫曼表。颜色转换、DCT（移植自 jfdctint.c）、量化舍入、边缘扩展、虚拟块、霍夫曼编码与标记段均按 libjpeg 实现。票面大多是白纸和黑字：样本全部相等的块只有 DC 系数，直接按公式计算并跳过 DCT；R = G = B 的像素在 libjpeg 定点转换下 Cb = Cr = 128；其余块走完整流程。两种 DCT 的中间量经仿射区间分析均小于 2^31 的 0.7 倍，32 位与 64 位 long 的 libjpeg 结果一致。

fitz_jpeg_encode.c 保留原 libjpeg 编码路径（派生自 MuPDF 1.28.0 output-jpeg.c，普通 JPEG、4:2:0），作为测试中的参考实现，生产路径不再调用。

include/turbojpeg 保留 libjpeg-turbo 的 LICENSE.md（IJG 与 BSD-3-Clause）和 README.ijg；This software is based in part on the work of the Independent JPEG Group。

新增头文件来源：ArtifexSoftware/thirdparty-libjpeg 的 627faae398468d5d11e3d694dc325432f734b5c4（MuPDF 1.28.0 的 submodule commit），以及 MuPDF 1.28.0 的 scripts/libjpeg/jconfig.h、source/fitz/jmemcust.h。保留 IJG README 与 MuPDF COPYING；This software is based in part on the work of the Independent JPEG Group。

## 测试与更新

fitz_jpeg_turbo_test.go 覆盖：libjpeg-turbo 输出为基线 4:2:0、JFIF 密度正确，16x16 以上的合成图像解码 PSNR 不低于 libjpeg 0.5 dB；ImageJPEG 在带库的平台用 libjpeg-turbo，其余平台用快速编码器。fitz_jpeg_internal_test.go 覆盖：DropToUnicode 前后像素一致且文本映射确已删除（测试内合成嵌入 TrueType CID 字体）；页面缓冲在不同页面尺寸间复用、并发渲染结果一致且空闲缓冲不超过上限；合成图像（多种尺寸、内容、质量，RGB 与 RGBA）快速编码器与 libjpeg 逐字节一致；渲染页面两种编码器一致；图片缓存命中前后与 MuPDF 直接绘制逐像素一致；跳过透明组的差异上限与含混合模式页面的回退；多 goroutine 并发渲染结果一致。

更新上游时：把 jpeg 分支变基到新的上游标签，保留 COPYING/AUTHORS，按新版本同步整套头文件与静态库，执行本模块全部测试和使用方的渲染测试，并检查 Windows amd64 与 Linux musl 静态构建，以及真实票面质量/性能；然后打新标签（上游尚未使用的下一个补丁号，如同步上游 v1.29.0 后用 v1.29.1），再更新使用方 go.mod 中的 replace 版本。修改本分支后同样需要打新标签。标签一经发布不要移动或复用（Go 模块代理与 sum.golang.org 会永久记录），v1.28.2 加后缀的预发布标签已被记录，不再使用。libjpeg 版本或默认参数变化时，以 TestEncodeJPEGMatchesLibjpeg 为准调整 fitz_jpeg_fast.c；更新 libjpeg-turbo 时改 scripts/build-turbojpeg.sh 的版本与校验值后重新生成，再跑 TestTurboJPEG；MuPDF 的图片解码或透明组逻辑变化时，以 TestImageCacheKeepsPixels、TestPageGroupSkip 为准复核 fitz_jpeg_render.c。

验证命令（在本仓库根目录执行；Windows 需 MinGW）：

```
go test ./...
```

Linux 用 CGO_ENABLED=1 和 -tags musl；Windows 上可用 Zig（CC="zig cc -target x86_64-linux-musl"）交叉编译。
