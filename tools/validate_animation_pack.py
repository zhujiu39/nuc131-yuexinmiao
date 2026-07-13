#!/usr/bin/env python3
"""离线验证 YXMV 动画包和完整 GD25Q64 镜像。

工具不会相信 manifest，而是直接解析二进制包头、帧索引和 1210 个帧记录，
逐帧执行与 NUC131 固件等价的 PackBits 解码及矩形回放。验证范围包括
包头/索引/数据 CRC32、每帧解码 CRC32、地址边界、完整 8MiB 镜像前缀、
尾部 0xFF 填充以及最终 SHA-256。

任一检查失败都会输出明确原因并返回退出码 1；全部通过才写入 PASS 报告。
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
import zlib
from pathlib import Path


# ======================== 固件同步常量 ========================

FLASH_SIZE = 8 * 1024 * 1024       # GD25Q64 完整镜像必须严格等于 8MiB。
LCD_PIXELS = 128 * 128             # 离线回放使用的完整 RGB565 帧缓冲像素数。


def decode_packbits(payload: bytes, expected_pixels: int) -> list[int]:
    """解码一帧变化矩形的 PackBits RGB565 payload。

    参数：
        payload：不含 16 字节帧头的压缩数据。
        expected_pixels：帧头声明的矩形像素总数。

    返回：
        按 LCD 行扫描顺序排列的 RGB565 整数列表。

    异常：
        令牌越过 payload、解码像素溢出或最终像素数不一致时抛出 ValueError。

    编码规则：
        控制字 bit7=1 时读取一个大端 RGB565 并重复低 7 位加 1 次；
        bit7=0 时读取相同数量的原样大端 RGB565 像素。
    """
    pixels: list[int] = []
    offset = 0
    while offset < len(payload):
        control = payload[offset]
        offset += 1
        length = (control & 0x7F) + 1
        if control & 0x80:
            if offset + 2 > len(payload):
                raise ValueError("重复色令牌越过帧记录末尾")
            color = struct.unpack_from(">H", payload, offset)[0]
            offset += 2
            pixels.extend([color] * length)
        else:
            byte_length = length * 2
            if offset + byte_length > len(payload):
                raise ValueError("原样像素令牌越过帧记录末尾")
            for pixel_offset in range(offset, offset + byte_length, 2):
                pixels.append(struct.unpack_from(">H", payload, pixel_offset)[0])
            offset += byte_length

        if len(pixels) > expected_pixels:
            raise ValueError("RLE 解码像素数超过矩形范围")

    if len(pixels) != expected_pixels:
        raise ValueError(f"RLE 解码像素数 {len(pixels)}，期望 {expected_pixels}")
    return pixels


def pixels_to_bytes(pixels: list[int]) -> bytes:
    """将 RGB565 整数列表序列化为大端字节，用于 CRC32 和最终帧哈希。"""
    output = bytearray(len(pixels) * 2)
    for index, pixel in enumerate(pixels):
        struct.pack_into(">H", output, index * 2, pixel)
    return bytes(output)


def main() -> int:
    """执行资源包、完整镜像和逐帧回放验证。

    返回：
        全部检查通过返回 0；文件、格式、边界或校验错误由入口统一返回 1。

    输出：
        将机器可读 JSON 报告写到 ``--report``，同时把相同内容打印到标准输出。
        报告只会在所有 1210 帧验证完成后生成，不会留下伪 PASS 中间结果。
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pack", required=True, type=Path)
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()

    # ======================== 完整镜像外层检查 ========================

    package = args.pack.read_bytes()
    full_image = args.image.read_bytes()
    if len(full_image) != FLASH_SIZE:
        raise ValueError(f"完整镜像长度 {len(full_image)}，期望 {FLASH_SIZE}")
    if full_image[: len(package)] != package:
        raise ValueError("完整镜像前缀与实际资源包不一致")
    if any(value != 0xFF for value in full_image[len(package) :]):
        raise ValueError("完整镜像保留区不是全 0xFF")

    # ======================== YXMV 固定包头解析 ========================

    # 所有多字节包头字段为小端；各偏移必须与生成器和 MCU animation_player.c 一致。
    magic, version, header_size = struct.unpack_from("<4sHH", package, 0x00)
    width, height, fps_num, fps_den = struct.unpack_from("<HHHH", package, 0x08)
    frame_count = struct.unpack_from("<I", package, 0x10)[0]
    index_offset, index_length, data_offset, data_length, package_length, flash_size = struct.unpack_from(
        "<IIIIII", package, 0x14
    )
    pixel_format, compression = struct.unpack_from("<II", package, 0x2C)
    index_crc32, data_crc32 = struct.unpack_from("<II", package, 0x68)
    header_crc32 = struct.unpack_from("<I", package, 0x70)[0]

    # 先锁定格式语义和本项目硬约束，避免后续按错误尺寸或压缩方式解释 payload。
    if magic != b"YXMV" or version != 1 or header_size != 256:
        raise ValueError("资源包 magic、版本或包头长度错误")
    if (width, height, fps_num, fps_den, frame_count) != (128, 128, 30, 1, 1210):
        raise ValueError("资源包尺寸、帧率或帧数不符合当前工程")
    if flash_size != FLASH_SIZE or package_length != len(package):
        raise ValueError("资源包容量或实际长度字段错误")
    if pixel_format != 1 or compression != 1:
        raise ValueError("像素格式或压缩格式不受支持")
    if zlib.crc32(package[:0x70]) & 0xFFFFFFFF != header_crc32:
        raise ValueError("包头 CRC32 错误")

    # 切片后分别核对索引和帧数据 CRC，能定位是地址表损坏还是 payload 损坏。
    index_blob = package[index_offset : index_offset + index_length]
    data_blob = package[data_offset : data_offset + data_length]
    if zlib.crc32(index_blob) & 0xFFFFFFFF != index_crc32:
        raise ValueError("索引 CRC32 错误")
    if zlib.crc32(data_blob) & 0xFFFFFFFF != data_crc32:
        raise ValueError("动画数据 CRC32 错误")

    # ======================== 逐帧解码与离线显存回放 ========================

    # 索引必须包含 frame_count+1 个绝对地址，相邻项定义每帧记录的 [start, end)。
    offsets = list(struct.unpack(f"<{frame_count + 1}I", index_blob))
    # 第 0 帧以前的屏幕为全黑；每帧只覆盖自身变化矩形，与 MCU 实际写屏行为一致。
    framebuffer = [0] * LCD_PIXELS
    decoded_pixel_total = 0
    empty_frames = 0
    max_record_length = 0

    for frame_index in range(frame_count):
        # 先检查索引单调性和数据区边界，再允许 struct 从包内读取帧头。
        start = offsets[frame_index]
        end = offsets[frame_index + 1]
        if start < data_offset or end < start or end > data_offset + data_length:
            raise ValueError(f"第 {frame_index} 帧索引越界")
        if end - start < 16:
            raise ValueError(f"第 {frame_index} 帧记录长度不足")

        x, y, rect_width, rect_height, encoded_length, pixel_count, decoded_crc32 = struct.unpack_from(
            "<BBBBIII", package, start
        )
        # 帧头、索引跨度、矩形面积和屏幕边界必须四者一致。
        if encoded_length + 16 != end - start:
            raise ValueError(f"第 {frame_index} 帧编码长度与索引不一致")
        if pixel_count != rect_width * rect_height:
            raise ValueError(f"第 {frame_index} 帧像素数与矩形不一致")
        if x + rect_width > width or y + rect_height > height:
            raise ValueError(f"第 {frame_index} 帧矩形越界")

        payload = package[start + 16 : end]
        decoded = decode_packbits(payload, pixel_count)
        # 帧 CRC 针对解码后的大端 RGB565 数据，可发现压缩流中任意单帧颜色损坏。
        if zlib.crc32(pixels_to_bytes(decoded)) & 0xFFFFFFFF != decoded_crc32:
            raise ValueError(f"第 {frame_index} 帧解码 CRC32 错误")

        # 将变化矩形回放到完整帧缓冲，用于验证跨帧叠加和最终画面状态。
        pixel_index = 0
        for row in range(y, y + rect_height):
            destination = row * width + x
            framebuffer[destination : destination + rect_width] = decoded[pixel_index : pixel_index + rect_width]
            pixel_index += rect_width

        decoded_pixel_total += pixel_count
        max_record_length = max(max_record_length, end - start)
        if pixel_count == 0:
            empty_frames += 1

    # ======================== PASS 报告 ========================

    # 只有所有结构、CRC 和逐帧回放检查通过后才构造 PASS 报告。
    report = {
        "result": "PASS",
        "magic": magic.decode("ascii"),
        "version": version,
        "width": width,
        "height": height,
        "fps": f"{fps_num}/{fps_den}",
        "frame_count": frame_count,
        "package_length": package_length,
        "flash_size": flash_size,
        "flash_utilization_percent": round(package_length * 100.0 / flash_size, 3),
        "data_crc32": f"{data_crc32:08X}",
        "index_crc32": f"{index_crc32:08X}",
        "decoded_pixel_total": decoded_pixel_total,
        "empty_frames": empty_frames,
        "max_frame_record": max_record_length,
        "final_framebuffer_crc32": f"{zlib.crc32(pixels_to_bytes(framebuffer)) & 0xFFFFFFFF:08X}",
        "package_sha256": hashlib.sha256(package).hexdigest().upper(),
        "full_image_sha256": hashlib.sha256(full_image).hexdigest().upper(),
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8", newline="\n")
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, struct.error) as error:
        # 非零退出码用于阻止 CI、批处理或生产脚本继续交付损坏镜像。
        print(f"验证失败：{error}", file=sys.stderr)
        raise SystemExit(1)
