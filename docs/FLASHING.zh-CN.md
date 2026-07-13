# 烧录指南

## 1. GD25Q64

推荐文件：

```text
release/GD25Q64/release_yuexinmiao_gd25q64_8MiB.bin
```

- 起始地址：`0x000000`
- 长度：8,388,608 字节
- 操作：整片擦除、写入、读回校验
- SHA-256：`16D314AFA7C8ABD02B2C1785508F3B3C23EEA64178A7AE9C3B0323A44E12105A`

## 2. NUC131

推荐文件：

```text
release/MCU/release_yuexinmiao.hex
```

- 目标区域：APROM
- SHA-256：`DA5D2E0665D18C07A70C146DF9B20E2C8D0562842AA9D5E01327378DA5DA263B`

不要把 GD25Q64 BIN 烧入 NUC131，也不要把 MCU HEX 写入外部 Flash。

## 3. 上电状态

| 画面 | 含义 |
| --- | --- |
| 正常动画 | Flash、资源和播放链路正常 |
| 红屏 | Flash 通信失败或 JEDEC ID 不匹配 |
| 洋红屏 | 资源头、尺寸、帧率或索引 CRC 错误 |
| 黄屏 | 播放过程中 Flash、解码或 LCD 写入错误 |
| 青屏 | 帧刷新连续积压，无法保持 30 fps |

