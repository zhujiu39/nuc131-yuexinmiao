# 第三方及资源声明

## Nuvoton NUC131 BSP

`vendor/Nuvoton/NUC131_BSP` 是为保证 Keil 工程可独立编译而保留的最小 BSP 子集。
原 BSP README 标注：

- SPDX-License-Identifier: Apache-2.0
- Copyright (C) Nuvoton Technology Corp.

各文件保留原始版权和 SPDX 声明。BSP 中包含的 ARM CMSIS 文件可能适用不同许可，
对应许可材料保存在 `vendor/Nuvoton/NUC131_BSP/licenses/`。

## GD25Q64 动画资源

`release/GD25Q64/` 中的 BIN、manifest 和逐帧清单来自项目动画素材的转换结果。
按照仓库所有者要求，原始猫咪 MP4 不随仓库发布，但生成后的 Flash 固件保留。

这些资源二进制不自动适用顶层 MIT License。仓库发布者和再分发者应自行确认拥有
发布、展示及再分发对应动画内容的权利。若只需要研究固件和资源格式，可使用
`tools/` 将自有合法素材重新生成替代镜像。

