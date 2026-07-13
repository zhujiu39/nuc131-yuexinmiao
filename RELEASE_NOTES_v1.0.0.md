# v1.0.0

首次公开版本。

## 内容

- NUC131SD2AE 48MHz 裸机播放固件。
- GD25Q64 SPI0 只读驱动和 JEDEC ID 检查。
- 128×128 RGB565 LCD GPIO 流式写入驱动。
- 1210 帧、30 fps YXMV 差分动画播放器。
- Python 绿幕资源生成器和逐帧完整镜像校验器。
- Keil 工程、最小 Nuvoton BSP 子集和可直接烧录的 MCU/Flash 固件。

## 验证

- 目标板实物播放通过。
- Keil ARMCC 5：0 Error(s), 0 Warning(s)。
- GD25Q64 资源验证：PASS。
- 完整镜像长度：8,388,608 字节。

## 素材说明

原始猫咪 MP4 和预览 MP4 不包含在仓库中；生成后的 GD25Q64 Flash 固件按仓库
所有者要求保留。再分发前请阅读 `THIRD_PARTY_NOTICES.md`。

