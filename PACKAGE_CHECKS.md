# 开源包检查记录

检查日期：2026-07-13

## 内容边界

- 未包含原始猫咪 MP4。
- 未包含灰色预览 MP4 或用于生成固件的原始视频。
- `docs/media/` 仅包含目标板验证实拍；公开视频已转为 H.264、移除录音轨和拍摄元数据。
- 未包含 Keil 临时输出目录、Nu-Link 探针配置和本机绝对路径。
- 包含 NUC131 MCU 烧录文件、GD25Q64 完整镜像与动画数据包。
- 包含资源生成工具、资源校验工具、格式说明和烧录说明。

## 固件构建

已从本开源包内的 `firmware/KEIL/yuexinmiao.uvprojx` 独立执行 Keil 构建：

```text
Program Size: Code=5056 RO-data=304 RW-data=60 ZI-data=1028
0 Error(s), 0 Warning(s)
```

重新构建得到的 HEX、BIN 与 `release/MCU/` 中的发布文件逐字节一致。

## 资源校验

`tools/validate_animation_pack.py` 对发布资源的校验结果：

- 画面尺寸：128 x 128
- 帧率：30/1 fps
- 总帧数：1210
- 动画数据包：2,289,920 字节
- GD25Q64 完整镜像：8,388,608 字节
- Flash 使用率：27.298%
- 数据包 SHA-256：`D61019E232176E31799FC003F52723B97DBAF0819E9ECD24B95BD4217E38E0E0`
- 完整镜像 SHA-256：`16D314AFA7C8ABD02B2C1785508F3B3C23EEA64178A7AE9C3B0323A44E12105A`

原始素材没有进入开源包；manifest 中的源文件名已匿名化，但源尺寸、帧数、时长和哈希仍被保留，用于确认资源来源版本。
