# yuexinmiao：NUC131 + GD25Q64 动画播放器

这是一个经过实物验证的裸机动画播放项目：NUC131SD2AE 从 GD25Q64 SPI NOR Flash
连续读取差分压缩的 RGB565 动画，通过 GPIO 模拟串行接口驱动 128×128 LCD，以
30 fps 循环播放。

## 特性

- NUC131SD2AE，Cortex-M0，48 MHz。
- GD25Q64，SPI0 Mode 0、8 位、10 MHz。
- 128×128 RGB565 LCD，GPIO 模拟串行写屏。
- YXMV 自定义资源包：逐帧变化矩形 + PackBits RLE。
- 完整保留 1210 帧、30 fps 和原始时间轴，不通过丢帧掩盖性能不足。
- 上电校验 Flash JEDEC ID、资源头 CRC32 和索引 CRC32。
- 提供 Python 资源生成器、完整镜像校验器、MCU HEX/BIN 和 GD25Q64 8 MiB 镜像。
- Keil ARMCC 5 实际编译：`0 Error(s), 0 Warning(s)`。
- 已完成目标板实物烧录和播放验证。

## 仓库结构

```text
firmware/                  NUC131 应用、驱动、Keil 工程及资源头文件
tools/                     MP4 资源生成器与 YXMV/GD25Q64 校验器
release/MCU/               可直接烧录的 NUC131 HEX/BIN
release/GD25Q64/           资源包、完整 8 MiB 镜像、manifest 和校验报告
vendor/Nuvoton/NUC131_BSP/ Keil 编译所需的最小 Nuvoton BSP 子集
docs/                      硬件连接、资源格式和烧录说明
assets/                    原始素材放置说明；仓库不包含原始猫咪 MP4
```

## 硬件连接

### LCD

| 信号 | NUC131 引脚 | 说明 |
| --- | --- | --- |
| SCL | PB15 | GPIO 模拟串行时钟 |
| SDA | PC14 | GPIO 模拟串行数据，只写 |
| CS | PC15 | 低有效 |
| DC/RS | PC7 | 0=命令，1=数据 |
| RST | PA7 | 低有效硬复位 |
| LED | PC6 | 高有效背光 |

### GD25Q64

| 信号 | NUC131 引脚 | 说明 |
| --- | --- | --- |
| CS# | PC0 | SPI0_SS0 |
| SCLK | PC1 | SPI0_CLK |
| SO | PC2 | SPI0_MISO0 |
| SI | PC3 | SPI0_MOSI0 |

详细约束见 [硬件说明](docs/HARDWARE.zh-CN.md)。

## 快速开始

### 1. 烧录外部 Flash

将下面的完整镜像从 GD25Q64 地址 `0x000000` 写入全部 8,388,608 字节，并读回校验：

```text
release/GD25Q64/release_yuexinmiao_gd25q64_8MiB.bin
```

### 2. 烧录 MCU

将下面的文件写入 NUC131SD2AE APROM：

```text
release/MCU/release_yuexinmiao.hex
```

两个文件必须配套使用。烧录和状态颜色说明见 [烧录指南](docs/FLASHING.zh-CN.md)。

## Keil 编译

环境：Keil MDK 5、ARM Compiler 5.06 update 6。

打开：

```text
firmware/KEIL/yuexinmiao.uvprojx
```

工程已改为相对引用仓库内 `vendor/Nuvoton/NUC131_BSP`，不依赖原开发电脑上的
绝对路径。Nu-Link 的本机探针配置和序列信息没有进入仓库。

## 重新生成资源

依赖 Python 3.9+、`ffmpeg` 和 `ffprobe`。把自己拥有公开或使用权的绿幕 MP4 放到
`assets/input/`，然后执行：

```powershell
python tools/generate_animation_pack.py `
  --input assets/input/your_animation.mp4 `
  --output-dir build/resources
```

当前生成器锁定 30 fps 和 1210 帧，以防误改已经验证的动画速度。生成新资源后，
将 `build/resources/animation_resource.h` 复制到 `firmware/Resources/generated/`，同步重新
编译 MCU 固件，并使用校验器检查新镜像：

```powershell
python tools/validate_animation_pack.py `
  --pack build/resources/release_yuexinmiao_animation_pack.bin `
  --image build/resources/release_yuexinmiao_gd25q64_8MiB.bin `
  --report build/resources/validation_report.json
```

## 素材范围

原始猫咪 MP4、灰色预览文件以及开发过程中的素材目录没有进入本仓库。
仓库保留用户明确要求发布的 GD25Q64 资源固件和生成清单。资源二进制的授权范围
与源代码许可证分开，详见 [第三方及资源声明](THIRD_PARTY_NOTICES.md)。

## 许可证

项目原创源代码使用 [MIT License](LICENSE)。Nuvoton BSP 和 ARM CMSIS 保留各自原始
许可证及版权声明；资源二进制不自动适用 MIT License。
