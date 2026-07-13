#!/usr/bin/env python3
"""生成与 NUC131 播放固件配套的 GD25Q64 动画资源。

输入必须是本项目确认的 1920x1080、30fps、1210 帧绿幕 MP4。工具调用
ffprobe 核对源参数，再通过 ffmpeg 去除绿色背景、按原始 16:9 比例缩入
128x128 画布，最后转换为 RGB565 变化矩形和 PackBits 数据。

输出同时包含实际长度 YXMV 资源包、尾部填充 0xFF 的完整 8MiB 镜像、
逐帧 manifest/CSV、固件资源头文件和黑底 MP4 预览。所有地址均为
GD25Q64 内部绝对地址，不能与 NUC131 APROM 地址混用。
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import struct
import subprocess
import sys
import zlib
from pathlib import Path


# ======================== 固件同步常量 ========================

LCD_WIDTH = 128                         # LCD 逻辑画布宽度，单位像素。
LCD_HEIGHT = 128                        # LCD 逻辑画布高度，单位像素。
FLASH_SIZE = 8 * 1024 * 1024            # GD25Q64 完整容量，单位字节。
HEADER_SIZE = 256                       # YXMV 固定包头长度，同时也是索引起始地址。
INDEX_OFFSET = 0x100                    # 帧索引在 Flash 镜像中的绝对地址。
MAGIC = b"YXMV"                        # 固件启动时首先校验的四字节标识。
FORMAT_VERSION = 1                      # 当前资源格式版本，不兼容时必须递增。
PIXEL_FORMAT_RGB565_BE = 1              # 像素高字节在前，便于 LCD 流式发送。
COMPRESSION_DELTA_RECT_PACKBITS = 1     # 每帧变化矩形加 PackBits 编码。
FRAME_RECORD_HEADER_SIZE = 16           # x/y/w/h、长度、像素数和 CRC32。


def align_up(value: int, alignment: int) -> int:
    """把数值向上对齐到指定整数边界。

    参数：
        value：需要对齐的非负字节长度或地址。
        alignment：对齐粒度；调用者必须传入正整数，本项目固定使用 256。

    返回：
        大于或等于 value 的最小 alignment 整数倍。
    """
    return (value + alignment - 1) // alignment * alignment


def parse_fraction(value: str) -> tuple[int, int]:
    """解析 ffprobe 返回的 ``分子/分母`` 字符串。

    参数：
        value：例如帧率 ``30/1`` 或时间基 ``1/16000``。

    返回：
        ``(分子, 分母)`` 整数元组；格式错误时由 split/int 抛出异常。
    """
    numerator, denominator = value.split("/", 1)
    return int(numerator), int(denominator)


def probe_video(input_path: Path) -> dict:
    """读取源视频中影响时间轴和画面映射的元数据。

    参数：
        input_path：原始绿幕 MP4 的绝对路径或可解析路径。

    返回：
        包含宽高、平均帧率、时间基、时长计数和实际解码帧数的字典。

    说明：
        使用 ``-count_frames`` 得到真实可读取帧数，不只相信容器声明值；
        subprocess 或 JSON 解析失败会直接抛出异常并终止资源生成。
    """
    command = [
        "ffprobe",
        "-v",
        "error",
        "-count_frames",
        "-select_streams",
        "v:0",
        "-show_entries",
        "format=duration:stream=width,height,r_frame_rate,avg_frame_rate,time_base,duration_ts,nb_read_frames",
        "-of",
        "json",
        str(input_path),
    ]
    result = subprocess.run(command, check=True, capture_output=True, text=True)
    metadata = json.loads(result.stdout)
    stream = metadata["streams"][0]
    fps_num, fps_den = parse_fraction(stream["avg_frame_rate"])
    time_base_num, time_base_den = parse_fraction(stream["time_base"])
    return {
        "width": int(stream["width"]),
        "height": int(stream["height"]),
        "fps_num": fps_num,
        "fps_den": fps_den,
        "time_base_num": time_base_num,
        "time_base_den": time_base_den,
        "duration_ts": int(stream["duration_ts"]),
        "duration_seconds": float(metadata["format"]["duration"]),
        "frame_count": int(stream["nb_read_frames"]),
    }


def build_filter(metadata: dict) -> str:
    """构造保持原始构图和时间轴不变的 ffmpeg 视频滤镜。

    参数：
        metadata：``probe_video`` 返回的源视频信息。

    返回：
        可直接传给 ffmpeg ``-vf`` 的滤镜字符串。

    说明：
        chromakey 只去除绿色背景；premultiply 防止透明边缘缩放后出现绿色杂边；
        scale 使用 Lanczos 等比例缩放，pad 将 16:9 画面垂直居中到 128x128。
        这里没有 fps、setpts、trim 或 crop 滤镜，因此不会改速、抽帧或裁切构图。
    """
    if metadata["width"] <= 0 or metadata["height"] <= 0:
        raise ValueError("源视频尺寸无效")

    return (
        "chromakey=0x1B8B3B:0.10:0.035,"
        "format=rgba,"
        "premultiply=inplace=1,"
        f"scale={LCD_WIDTH}:{LCD_HEIGHT}:force_original_aspect_ratio=decrease:flags=lanczos,"
        f"pad={LCD_WIDTH}:{LCD_HEIGHT}:(ow-iw)/2:(oh-ih)/2:color=black@0,"
        "format=rgba"
    )


def rgba_to_rgb565_pixels(rgba: bytes) -> list[int]:
    """把一帧固定尺寸 RGBA 数据转换为 RGB565 整数列表。

    参数：
        rgba：ffmpeg 输出的 128x128x4 字节帧数据。

    返回：
        按行优先排列的 16384 个 RGB565 像素。

    说明：
        滤镜已对颜色执行 Alpha 预乘，透明区域的 RGB 分量为 0，因此在无 Alpha
        通道的 LCD 上自然显示为黑色。长度不符表示 ffmpeg 帧输出不完整。
    """
    expected = LCD_WIDTH * LCD_HEIGHT * 4
    if len(rgba) != expected:
        raise ValueError(f"RGBA 帧长度错误：{len(rgba)}，期望 {expected}")

    pixels = [0] * (LCD_WIDTH * LCD_HEIGHT)
    source_index = 0
    for pixel_index in range(len(pixels)):
        red = rgba[source_index]
        green = rgba[source_index + 1]
        blue = rgba[source_index + 2]
        pixels[pixel_index] = ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)
        source_index += 4
    return pixels


def find_delta_rectangle(current: list[int], previous: list[int]) -> tuple[int, int, int, int]:
    """计算当前帧相对上一帧的最小外接变化矩形。

    参数：
        current：当前完整 128x128 RGB565 帧。
        previous：上一完整 RGB565 帧；第 0 帧以前使用全黑帧。

    返回：
        ``(x, y, width, height)``；两帧完全相同时返回四个 0。

    说明：
        矩形内会保存全部像素而不仅是变化像素，这使 MCU 能按连续窗口直接写屏，
        无需 32KiB 整帧缓冲区或逐像素坐标命令。
    """
    min_x = LCD_WIDTH
    min_y = LCD_HEIGHT
    max_x = -1
    max_y = -1

    for index, (current_pixel, previous_pixel) in enumerate(zip(current, previous)):
        if current_pixel != previous_pixel:
            y, x = divmod(index, LCD_WIDTH)
            if x < min_x:
                min_x = x
            if x > max_x:
                max_x = x
            if y < min_y:
                min_y = y
            if y > max_y:
                max_y = y

    if max_x < 0:
        return 0, 0, 0, 0
    return min_x, min_y, max_x - min_x + 1, max_y - min_y + 1


def extract_rectangle(pixels: list[int], x: int, y: int, width: int, height: int) -> list[int]:
    """按 LCD 行扫描顺序提取一个矩形内的 RGB565 像素。"""
    rectangle: list[int] = []
    for row in range(y, y + height):
        row_start = row * LCD_WIDTH + x
        rectangle.extend(pixels[row_start : row_start + width])
    return rectangle


def encode_packbits_rgb565(pixels: list[int]) -> bytes:
    """把 RGB565 像素编码为 MCU 可边读边解码的 PackBits 数据。

    参数：
        pixels：变化矩形内按行优先排列的 RGB565 像素。

    返回：
        PackBits 字节流。控制字 bit7=1 表示重复色，bit7=0 表示原样像素，
        两类令牌的像素数均为低 7 位加 1，范围 1～128。

    说明：
        连续相同颜色达到 3 个才使用重复令牌，避免短重复反而增大数据；
        RGB565 均用大端字节序写入，与 LCD 高字节先发的接口一致。
    """
    output = bytearray()
    index = 0
    pixel_count = len(pixels)

    while index < pixel_count:
        run_length = 1
        while (
            index + run_length < pixel_count
            and run_length < 128
            and pixels[index + run_length] == pixels[index]
        ):
            run_length += 1

        if run_length >= 3:
            output.append(0x80 | (run_length - 1))
            output.extend(struct.pack(">H", pixels[index]))
            index += run_length
            continue

        literal_start = index
        index += run_length
        while index < pixel_count and (index - literal_start) < 128:
            next_run = 1
            while (
                index + next_run < pixel_count
                and next_run < 128
                and pixels[index + next_run] == pixels[index]
            ):
                next_run += 1
            if next_run >= 3:
                break
            if (index - literal_start) + next_run > 128:
                break
            index += next_run

        literal_length = index - literal_start
        output.append(literal_length - 1)
        for pixel in pixels[literal_start:index]:
            output.extend(struct.pack(">H", pixel))

    return bytes(output)


def pixels_to_big_endian_bytes(pixels: list[int]) -> bytes:
    """把 RGB565 整数列表序列化为高字节在前的连续字节，用于 CRC32。"""
    output = bytearray(len(pixels) * 2)
    offset = 0
    for pixel in pixels:
        struct.pack_into(">H", output, offset, pixel)
        offset += 2
    return bytes(output)


def generate_preview(input_path: Path, output_path: Path, filter_chain: str) -> None:
    """生成与资源处理链一致的黑底 MP4 预览。

    参数：
        input_path：原始绿幕视频。
        output_path：输出预览 MP4 路径。
        filter_chain：与资源生成共用的去绿幕、缩放和补边滤镜。

    说明：
        yuv420p 不支持 Alpha，因此透明区域显示为黑色；``-fps_mode passthrough``
        保留输入时间戳，预览不承担资源编码，只用于电脑端人工核对构图和速度。
    """
    preview_filter = filter_chain.rsplit(",format=rgba", 1)[0] + ",format=yuv420p"
    command = [
        "ffmpeg",
        "-y",
        "-v",
        "error",
        "-i",
        str(input_path),
        "-vf",
        preview_filter,
        "-c:v",
        "libx264",
        "-preset",
        "medium",
        "-crf",
        "12",
        "-fps_mode",
        "passthrough",
        "-movflags",
        "+faststart",
        str(output_path),
    ]
    subprocess.run(command, check=True)


def write_generated_header(path: Path, manifest: dict) -> None:
    """根据同一份 manifest 生成 MCU 编译使用的资源常量头文件。

    参数：
        path：``animation_resource.h`` 输出路径。
        manifest：已经计算完成的资源布局和帧率信息。

    说明：
        地址、长度、帧数和格式宏禁止手工维护，必须与本次资源包同源生成，
        防止 MCU 固件使用旧偏移读取新镜像。
    """
    text = f"""#ifndef YUEXINMIAO_ANIMATION_RESOURCE_H
#define YUEXINMIAO_ANIMATION_RESOURCE_H

/* 本文件由 Tools/generate_animation_pack.py 自动生成，请勿手工修改。 */

#define ANIMATION_FLASH_SIZE_BYTES          0x{FLASH_SIZE:08X}UL
#define ANIMATION_PACKAGE_BASE_ADDRESS      0x00000000UL
#define ANIMATION_PACKAGE_MAGIC             0x564D5859UL
#define ANIMATION_PACKAGE_VERSION           {FORMAT_VERSION}U
#define ANIMATION_HEADER_SIZE               {HEADER_SIZE}U
#define ANIMATION_FRAME_WIDTH               {LCD_WIDTH}U
#define ANIMATION_FRAME_HEIGHT              {LCD_HEIGHT}U
#define ANIMATION_FPS_NUMERATOR              {manifest['fps_num']}U
#define ANIMATION_FPS_DENOMINATOR            {manifest['fps_den']}U
#define ANIMATION_FRAME_COUNT                {manifest['frame_count']}UL
#define ANIMATION_INDEX_OFFSET               0x{manifest['index_offset']:08X}UL
#define ANIMATION_DATA_OFFSET                0x{manifest['data_offset']:08X}UL
#define ANIMATION_DATA_LENGTH                0x{manifest['data_length']:08X}UL
#define ANIMATION_PACKAGE_LENGTH             0x{manifest['package_length']:08X}UL
#define ANIMATION_PIXEL_FORMAT_RGB565_BE     {PIXEL_FORMAT_RGB565_BE}U
#define ANIMATION_COMPRESSION_DELTA_PACKBITS {COMPRESSION_DELTA_RECT_PACKBITS}U
#define ANIMATION_GD25Q64_JEDEC_ID            0xC84017UL

#endif
"""
    path.write_text(text, encoding="utf-8", newline="\n")


def main() -> int:
    """执行完整资源生成流程。

    返回：
        成功返回 0；可预期的文件、参数、ffmpeg 或容量错误由模块入口统一转成退出码 1。

    输出：
        在 ``--output-dir`` 中覆盖生成资源包、8MiB 镜像、manifest、CSV、
        ``animation_resource.h`` 和 MP4 预览。调用前应确认输出目录允许更新。
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path, help="原始绿幕 MP4")
    parser.add_argument("--output-dir", required=True, type=Path, help="资源输出目录")
    args = parser.parse_args()

    input_path = args.input.resolve()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    # ======================== 源视频硬约束检查 ========================

    metadata = probe_video(input_path)
    if (metadata["fps_num"], metadata["fps_den"]) != (30, 1):
        raise RuntimeError(f"源视频帧率不是 30fps：{metadata['fps_num']}/{metadata['fps_den']}")
    if metadata["frame_count"] != 1210:
        raise RuntimeError(f"源视频帧数不是 1210：{metadata['frame_count']}")

    # 源文件哈希写入包头和 manifest，确保后续可以追溯资源使用了哪一份 MP4。
    source_sha256 = hashlib.sha256(input_path.read_bytes()).digest()
    filter_chain = build_filter(metadata)
    frame_size = LCD_WIDTH * LCD_HEIGHT * 4

    # ffmpeg 通过 stdout 连续输出固定长度 RGBA 帧，避免在磁盘上产生 1210 张临时图片。
    ffmpeg_command = [
        "ffmpeg",
        "-v",
        "error",
        "-i",
        str(input_path),
        "-vf",
        filter_chain,
        "-f",
        "rawvideo",
        "-pix_fmt",
        "rgba",
        "-",
    ]

    process = subprocess.Popen(ffmpeg_command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if process.stdout is None or process.stderr is None:
        raise RuntimeError("无法启动 ffmpeg 管道")

    # ======================== 索引与帧数据生成 ========================

    frame_count = metadata["frame_count"]
    # 索引额外保存一个末尾偏移，固件读取相邻两个值即可得到任意帧的 [start, end)。
    index_length = (frame_count + 1) * 4
    data_offset = align_up(INDEX_OFFSET + index_length, 256)
    data_blob = bytearray()
    frame_offsets: list[int] = []
    frame_manifest: list[dict] = []
    previous_pixels = [0] * (LCD_WIDTH * LCD_HEIGHT)

    for frame_index in range(frame_count):
        # read 可能返回短数据；必须当场失败，禁止把缺帧资源包装成正常 1210 帧镜像。
        rgba = process.stdout.read(frame_size)
        if len(rgba) != frame_size:
            error_text = process.stderr.read().decode("utf-8", errors="replace")
            raise RuntimeError(f"第 {frame_index} 帧读取失败：{len(rgba)} 字节\n{error_text}")

        current_pixels = rgba_to_rgb565_pixels(rgba)
        x, y, width, height = find_delta_rectangle(current_pixels, previous_pixels)
        rectangle_pixels = extract_rectangle(current_pixels, x, y, width, height) if width else []
        encoded = encode_packbits_rgb565(rectangle_pixels)
        decoded_bytes = pixels_to_big_endian_bytes(rectangle_pixels)

        # 帧 CRC 针对解码后的矩形像素，验证器可独立确认压缩/解压过程没有改变颜色。
        decoded_crc32 = zlib.crc32(decoded_bytes) & 0xFFFFFFFF
        frame_address = data_offset + len(data_blob)
        frame_offsets.append(frame_address)

        # 帧头固定 16 字节且使用小端字段；RGB565 payload 本身仍为高字节在前。
        record_header = struct.pack(
            "<BBBBIII",
            x,
            y,
            width,
            height,
            len(encoded),
            len(rectangle_pixels),
            decoded_crc32,
        )
        data_blob.extend(record_header)
        data_blob.extend(encoded)

        frame_manifest.append(
            {
                "frame": frame_index,
                "time_seconds": frame_index * metadata["fps_den"] / metadata["fps_num"],
                "flash_offset": frame_address,
                "x": x,
                "y": y,
                "width": width,
                "height": height,
                "pixel_count": len(rectangle_pixels),
                "raw_length": len(decoded_bytes),
                "encoded_length": len(encoded),
                "record_length": len(record_header) + len(encoded),
                "decoded_crc32": f"{decoded_crc32:08X}",
            }
        )
        previous_pixels = current_pixels

    # 读取一个额外字节用于确认 ffmpeg 没有输出第 1211 帧，同时回收子进程错误信息。
    trailing = process.stdout.read(1)
    stderr_text = process.stderr.read().decode("utf-8", errors="replace")
    return_code = process.wait()
    if return_code != 0:
        raise RuntimeError(f"ffmpeg 返回 {return_code}\n{stderr_text}")
    if trailing:
        raise RuntimeError("ffmpeg 输出帧数超过 1210")

    # ======================== YXMV 包头与 Flash 镜像组装 ========================

    frame_offsets.append(data_offset + len(data_blob))
    index_blob = b"".join(struct.pack("<I", value) for value in frame_offsets)
    data_length = len(data_blob)
    package_length = align_up(data_offset + data_length, 256)
    if package_length > FLASH_SIZE:
        raise RuntimeError(f"资源包 {package_length} 字节超过 GD25Q64 容量 {FLASH_SIZE} 字节")

    # 索引和帧记录分别计算 CRC；包头 CRC 只覆盖自身 0x00～0x6F 字节。
    index_crc32 = zlib.crc32(index_blob) & 0xFFFFFFFF
    data_crc32 = zlib.crc32(data_blob) & 0xFFFFFFFF
    header = bytearray([0x00] * HEADER_SIZE)
    struct.pack_into("<4sHH", header, 0x00, MAGIC, FORMAT_VERSION, HEADER_SIZE)
    struct.pack_into("<HHHH", header, 0x08, LCD_WIDTH, LCD_HEIGHT, metadata["fps_num"], metadata["fps_den"])
    struct.pack_into("<I", header, 0x10, frame_count)
    struct.pack_into("<IIIIII", header, 0x14, INDEX_OFFSET, index_length, data_offset, data_length, package_length, FLASH_SIZE)
    struct.pack_into("<II", header, 0x2C, PIXEL_FORMAT_RGB565_BE, COMPRESSION_DELTA_RECT_PACKBITS)
    struct.pack_into("<HH", header, 0x34, metadata["width"], metadata["height"])
    struct.pack_into("<IIII", header, 0x38, metadata["frame_count"], metadata["duration_ts"], metadata["time_base_num"], metadata["time_base_den"])
    header[0x48 : 0x68] = source_sha256
    struct.pack_into("<II", header, 0x68, index_crc32, data_crc32)
    header_crc32 = zlib.crc32(header[:0x70]) & 0xFFFFFFFF
    struct.pack_into("<I", header, 0x70, header_crc32)

    # 实际资源包按 256 字节填充，未使用区域为 0xFF，便于 Flash 编程器和地址对齐。
    package = bytearray([0xFF] * package_length)
    package[:HEADER_SIZE] = header
    package[INDEX_OFFSET : INDEX_OFFSET + len(index_blob)] = index_blob
    package[data_offset : data_offset + len(data_blob)] = data_blob

    # 完整镜像固定为 8MiB：前缀是 YXMV 包，其余保留区保持擦除态 0xFF。
    full_image = bytearray([0xFF] * FLASH_SIZE)
    full_image[: len(package)] = package

    pack_path = output_dir / "release_yuexinmiao_animation_pack.bin"
    image_path = output_dir / "release_yuexinmiao_gd25q64_8MiB.bin"
    preview_path = output_dir / "animation_128x128_black_preview.mp4"
    pack_path.write_bytes(package)
    image_path.write_bytes(full_image)

    # ======================== 可追溯清单与配套文件 ========================

    # JSON 保存资源整体参数和全部帧明细，是头文件、CSV 与验证报告的上游事实来源。
    manifest = {
        "format": "YXMV delta-rectangle RGB565 animation",
        "format_version": FORMAT_VERSION,
        "source_file": input_path.name,
        "source_sha256": source_sha256.hex().upper(),
        "source_width": metadata["width"],
        "source_height": metadata["height"],
        "source_frame_count": metadata["frame_count"],
        "source_duration_ts": metadata["duration_ts"],
        "source_time_base": f"{metadata['time_base_num']}/{metadata['time_base_den']}",
        "source_duration_seconds": metadata["duration_seconds"],
        "width": LCD_WIDTH,
        "height": LCD_HEIGHT,
        "fps_num": metadata["fps_num"],
        "fps_den": metadata["fps_den"],
        "frame_count": frame_count,
        "pixel_format": "RGB565 big-endian",
        "background": "green keyed to transparent, composited over RGB565 black for standalone LCD playback",
        "geometry": "full 16:9 source scaled proportionally into 128x128; no crop, centered letterbox",
        "compression": "per-frame changed rectangle + PackBits RLE",
        "flash_model": "GD25Q64",
        "flash_size": FLASH_SIZE,
        "flash_base_address": 0,
        "index_offset": INDEX_OFFSET,
        "index_length": index_length,
        "index_crc32": f"{index_crc32:08X}",
        "data_offset": data_offset,
        "data_length": data_length,
        "data_crc32": f"{data_crc32:08X}",
        "package_length": package_length,
        "package_sha256": hashlib.sha256(package).hexdigest().upper(),
        "full_image_length": FLASH_SIZE,
        "full_image_sha256": hashlib.sha256(full_image).hexdigest().upper(),
        "frames": frame_manifest,
    }
    manifest_path = output_dir / "animation_manifest.json"
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8", newline="\n")

    csv_path = output_dir / "animation_frames.csv"
    with csv_path.open("w", encoding="utf-8-sig", newline="") as csv_file:
        # 使用 UTF-8 BOM 便于 Windows Excel 正确显示中文字段或后续扩展内容。
        writer = csv.DictWriter(csv_file, fieldnames=frame_manifest[0].keys())
        writer.writeheader()
        writer.writerows(frame_manifest)

    write_generated_header(output_dir / "animation_resource.h", manifest)
    generate_preview(input_path, preview_path, filter_chain)

    # 控制台只输出生产核对所需摘要，完整逐帧数据保存在 manifest/CSV。
    summary = {
        "frames": frame_count,
        "data_length": data_length,
        "package_length": package_length,
        "flash_utilization_percent": round(package_length * 100.0 / FLASH_SIZE, 3),
        "max_frame_record": max(item["record_length"] for item in frame_manifest),
        "average_frame_record": round(sum(item["record_length"] for item in frame_manifest) / frame_count, 2),
        "empty_frames": sum(1 for item in frame_manifest if item["width"] == 0),
    }
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        # 统一转换为非零退出码，便于批处理或生产脚本阻止错误资源继续交付。
        print(f"错误：{error}", file=sys.stderr)
        raise SystemExit(1)
