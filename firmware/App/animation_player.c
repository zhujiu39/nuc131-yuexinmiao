#include <stdint.h>

#include "animation_player.h"
#include "animation_resource.h"
#include "gd25q64.h"
#include "lcd_driver.h"

/* ======================== YXMV 资源格式参数 ======================== */

#define PACKAGE_HEADER_READ_SIZE     256U    /* 固定包头长度，与生成工具 HEADER_SIZE 一致。 */
#define FRAME_RECORD_HEADER_SIZE     16U     /* x/y/w/h、压缩长度、像素数、帧 CRC32。 */
#define INDEX_CRC_CHUNK_SIZE         64U     /* 启动校验缓冲区大小，避免占用过多 NUC131 SRAM。 */

/* ======================== 播放器内部状态 ======================== */

static uint8_t animation_ready = 0U;          /* 包头与索引通过校验后置 1，任一播放错误后清 0。 */
static uint32_t animation_next_frame = 0U;    /* 下一次 RenderNext 需要读取的资源帧号。 */

/**
 ******************************************************************************
  @功能：从字节流读取一个 16 位小端整数
  @日期：2026-07-13
  @参数：[输入] data - 至少包含 2 个有效字节的只读地址
  @返回值：uint16_t - 按低字节在前组合后的数值
  @使用说明：仅供已完成长度检查的包头和索引解析使用，调用者负责保证指针有效。
 ******************************************************************************
 */
static uint16_t read_u16_le(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8U);
}

/**
 ******************************************************************************
  @功能：从字节流读取一个 32 位小端整数
  @日期：2026-07-13
  @参数：[输入] data - 至少包含 4 个有效字节的只读地址
  @返回值：uint32_t - 按低字节在前组合后的数值
  @使用说明：YXMV 包头、帧索引和帧记录的多字节字段均使用小端格式。
 ******************************************************************************
 */
static uint32_t read_u32_le(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8U) |
           ((uint32_t)data[2] << 16U) |
           ((uint32_t)data[3] << 24U);
}

/**
 ******************************************************************************
  @功能：增量计算标准反射式 CRC32
  @日期：2026-07-13
  @参数：[输入] crc - 上一次计算结果，首次调用传入 0xFFFFFFFF
        [输入] data - 本次参与计算的数据
        [输入] length - 本次数据长度，单位字节
  @返回值：uint32_t - 尚未执行最终异或的中间 CRC
  @使用说明：多段数据可连续调用，全部处理完成后再与 0xFFFFFFFF 异或；多项式为 0xEDB88320。
 ******************************************************************************
 */
static uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t length)
{
    uint32_t byte_index;
    uint8_t bit_index;

    for(byte_index = 0U; byte_index < length; byte_index++)
    {
        crc ^= data[byte_index];
        for(bit_index = 0U; bit_index < 8U; bit_index++)
        {
            if((crc & 1U) != 0U)
            {
                crc = (crc >> 1U) ^ 0xEDB88320UL;
            }
            else
            {
                crc >>= 1U;
            }
        }
    }
    return crc;
}

/**
 ******************************************************************************
  @功能：分块读取并校验 GD25Q64 中的整张帧索引
  @日期：2026-07-13
  @参数：[输入] index_offset - 索引在 GD25Q64 中的绝对起始地址
        [输入] index_length - 索引总长度，单位字节
        [输入] expected_crc - 包头记录的索引 CRC32
  @返回值：AnimationStatus - 读取失败返回 FLASH_ERROR，CRC 不符返回 INDEX_ERROR
  @使用说明：每次只占用 64 字节栈缓冲区。该函数在启动阶段调用，不在 30Hz 播放路径中重复执行。
 ******************************************************************************
 */
static AnimationStatus verify_index_crc(uint32_t index_offset, uint32_t index_length, uint32_t expected_crc)
{
    uint8_t buffer[INDEX_CRC_CHUNK_SIZE];
    uint32_t address;
    uint32_t remaining;
    uint32_t chunk;
    uint32_t crc;

    address = index_offset;
    remaining = index_length;
    crc = 0xFFFFFFFFUL;
    while(remaining > 0U)
    {
        chunk = (remaining > INDEX_CRC_CHUNK_SIZE) ? INDEX_CRC_CHUNK_SIZE : remaining;
        if(GD25Q64_Read(address, buffer, chunk) != GD25Q64_STATUS_OK)
        {
            return ANIMATION_STATUS_FLASH_ERROR;
        }
        crc = crc32_update(crc, buffer, chunk);
        address += chunk;
        remaining -= chunk;
    }

    if((crc ^ 0xFFFFFFFFUL) != expected_crc)
    {
        return ANIMATION_STATUS_INDEX_ERROR;
    }
    return ANIMATION_STATUS_OK;
}

/**
 ******************************************************************************
  @功能：读取并校验 YXMV 动画包，使播放器进入可播放状态
  @日期：2026-07-13
  @参数：无
  @返回值：AnimationStatus - 包头字段、边界和索引 CRC 全部正确时返回 OK
  @使用说明：包头中的 data CRC 和每帧 CRC 已由离线校验工具逐帧验证；运行时为避免上电扫描 2MB 数据，
             只校验头 CRC、索引 CRC，并在播放每帧时继续检查记录长度、矩形和解码像素边界。
 ******************************************************************************
 */
AnimationStatus AnimationPlayer_Init(void)
{
    uint8_t header[PACKAGE_HEADER_READ_SIZE];
    uint32_t header_crc;
    uint32_t index_offset;
    uint32_t index_length;
    uint32_t data_offset;
    uint32_t data_length;
    uint32_t package_length;
    uint32_t index_crc;
    AnimationStatus status;

    animation_ready = 0U;
    animation_next_frame = 0U;
    if(GD25Q64_Read(ANIMATION_PACKAGE_BASE_ADDRESS, header, sizeof(header)) != GD25Q64_STATUS_OK)
    {
        return ANIMATION_STATUS_FLASH_ERROR;
    }

    /* 头 CRC 字段位于 0x70，因此 CRC 覆盖范围固定为 [0x00, 0x6F]。 */
    header_crc = crc32_update(0xFFFFFFFFUL, header, 0x70U) ^ 0xFFFFFFFFUL;

    /* 第一组检查锁定资源语义，防止错误版本、错误屏幕尺寸或错误压缩格式被继续解析。 */
    if((read_u32_le(&header[0x00]) != ANIMATION_PACKAGE_MAGIC) ||
       (read_u16_le(&header[0x04]) != ANIMATION_PACKAGE_VERSION) ||
       (read_u16_le(&header[0x06]) != ANIMATION_HEADER_SIZE) ||
       (read_u16_le(&header[0x08]) != ANIMATION_FRAME_WIDTH) ||
       (read_u16_le(&header[0x0A]) != ANIMATION_FRAME_HEIGHT) ||
       (read_u16_le(&header[0x0C]) != ANIMATION_FPS_NUMERATOR) ||
       (read_u16_le(&header[0x0E]) != ANIMATION_FPS_DENOMINATOR) ||
       (read_u32_le(&header[0x10]) != ANIMATION_FRAME_COUNT) ||
       (read_u32_le(&header[0x2C]) != ANIMATION_PIXEL_FORMAT_RGB565_BE) ||
       (read_u32_le(&header[0x30]) != ANIMATION_COMPRESSION_DELTA_PACKBITS) ||
       (read_u32_le(&header[0x70]) != header_crc))
    {
        return ANIMATION_STATUS_HEADER_ERROR;
    }

    index_offset = read_u32_le(&header[0x14]);
    index_length = read_u32_le(&header[0x18]);
    data_offset = read_u32_le(&header[0x1C]);
    data_length = read_u32_le(&header[0x20]);
    package_length = read_u32_le(&header[0x24]);
    index_crc = read_u32_le(&header[0x68]);

    /* 第二组检查锁定分区布局，确保静态头文件与实际烧入 GD25Q64 的资源镜像完全配套。 */
    if((index_offset != ANIMATION_INDEX_OFFSET) ||
       (index_length != ((ANIMATION_FRAME_COUNT + 1UL) * 4UL)) ||
       (data_offset != ANIMATION_DATA_OFFSET) ||
       (data_length != ANIMATION_DATA_LENGTH) ||
       (package_length != ANIMATION_PACKAGE_LENGTH) ||
       (package_length > ANIMATION_FLASH_SIZE_BYTES) ||
       (data_offset + data_length > package_length))
    {
        return ANIMATION_STATUS_HEADER_ERROR;
    }

    status = verify_index_crc(index_offset, index_length, index_crc);
    if(status != ANIMATION_STATUS_OK)
    {
        return status;
    }

    animation_ready = 1U;
    return ANIMATION_STATUS_OK;
}

/**
 ******************************************************************************
  @功能：读取指定帧的起始地址和下一帧地址
  @日期：2026-07-13
  @参数：[输入] frame_index - 目标帧号，必须小于总帧数
        [输出] start - 当前帧记录在 GD25Q64 中的绝对起始地址
        [输出] end - 当前帧记录结束后的首地址
  @返回值：AnimationStatus - 参数或偏移非法返回 INDEX_ERROR，Flash 读取失败返回 FLASH_ERROR
  @使用说明：索引包含 frame_count+1 个偏移，因此一次连续读取 8 字节即可得到当前帧的 [start,end)。
 ******************************************************************************
 */
static AnimationStatus read_frame_offsets(uint32_t frame_index, uint32_t *start, uint32_t *end)
{
    uint8_t offsets[8];

    if((start == 0) || (end == 0) || (frame_index >= ANIMATION_FRAME_COUNT))
    {
        return ANIMATION_STATUS_INDEX_ERROR;
    }
    if(GD25Q64_Read(ANIMATION_INDEX_OFFSET + (frame_index * 4UL), offsets, sizeof(offsets)) != GD25Q64_STATUS_OK)
    {
        return ANIMATION_STATUS_FLASH_ERROR;
    }

    *start = read_u32_le(&offsets[0]);
    *end = read_u32_le(&offsets[4]);
    if((*start < ANIMATION_DATA_OFFSET) || (*end < *start) ||
       (*end > (ANIMATION_DATA_OFFSET + ANIMATION_DATA_LENGTH)) ||
       ((*end - *start) < FRAME_RECORD_HEADER_SIZE))
    {
        return ANIMATION_STATUS_INDEX_ERROR;
    }
    return ANIMATION_STATUS_OK;
}

/**
 ******************************************************************************
  @功能：从 GD25Q64 流式解码一帧变化矩形并直接写入 LCD
  @日期：2026-07-13
  @参数：[输入] frame_index - 需要渲染的资源帧号
  @返回值：AnimationStatus - 返回索引、Flash、帧格式或 LCD 对应错误
  @使用说明：函数不申请整帧缓冲区。Flash CS 与 LCD CS 分别保持连续有效，PackBits 边读边解码边写屏；
             所有退出路径都会关闭已打开的 Flash/LCD 流，避免片选长期保持低电平。
 ******************************************************************************
 */
static AnimationStatus render_frame(uint32_t frame_index)
{
    uint8_t record[FRAME_RECORD_HEADER_SIZE];
    uint8_t control;
    uint8_t high_byte;
    uint8_t low_byte;
    uint16_t color;
    uint16_t token_pixels;
    uint32_t start;
    uint32_t end;
    uint32_t encoded_length;
    uint32_t expected_pixels;
    uint32_t encoded_consumed;
    uint32_t decoded_pixels;
    uint32_t pixel_index;
    uint8_t x;
    uint8_t y;
    uint8_t width;
    uint8_t height;
    uint8_t lcd_open;
    uint8_t flash_open;
    AnimationStatus status;

    status = read_frame_offsets(frame_index, &start, &end);
    if(status != ANIMATION_STATUS_OK)
    {
        return status;
    }
    if(GD25Q64_Read(start, record, sizeof(record)) != GD25Q64_STATUS_OK)
    {
        return ANIMATION_STATUS_FLASH_ERROR;
    }

    /* 16 字节帧头：x、y、width、height、压缩长度、解码像素数、离线帧 CRC32。 */
    x = record[0];
    y = record[1];
    width = record[2];
    height = record[3];
    encoded_length = read_u32_le(&record[4]);
    expected_pixels = read_u32_le(&record[8]);
    if((encoded_length + FRAME_RECORD_HEADER_SIZE != (end - start)) ||
       (expected_pixels != ((uint32_t)width * (uint32_t)height)) ||
       ((uint16_t)x + width > ANIMATION_FRAME_WIDTH) ||
       ((uint16_t)y + height > ANIMATION_FRAME_HEIGHT))
    {
        return ANIMATION_STATUS_FRAME_ERROR;
    }
    if(expected_pixels == 0U)
    {
        /* 画面与上一帧完全一致时记录为空，只推进时间轴，不重复刷新 LCD。 */
        return ANIMATION_STATUS_OK;
    }

    /* 先打开 Flash 连续读，再打开 LCD 变化矩形窗口，减少每字节/每像素重复发送地址的开销。 */
    lcd_open = 0U;
    flash_open = 0U;
    if(GD25Q64_BeginRead(start + FRAME_RECORD_HEADER_SIZE) != GD25Q64_STATUS_OK)
    {
        return ANIMATION_STATUS_FLASH_ERROR;
    }
    flash_open = 1U;
    if(LCD_BeginRegionWrite(x, y, width, height) != LCD_STATUS_OK)
    {
        GD25Q64_EndRead();
        return ANIMATION_STATUS_LCD_ERROR;
    }
    lcd_open = 1U;

    encoded_consumed = 0U;
    decoded_pixels = 0U;
    while(encoded_consumed < encoded_length)
    {
        if(GD25Q64_ReadByte(&control) != GD25Q64_STATUS_OK)
        {
            status = ANIMATION_STATUS_FLASH_ERROR;
            break;
        }
        encoded_consumed++;
        token_pixels = (uint16_t)((control & 0x7FU) + 1U);
        if((decoded_pixels + token_pixels) > expected_pixels)
        {
            status = ANIMATION_STATUS_FRAME_ERROR;
            break;
        }

        if((control & 0x80U) != 0U)
        {
            /* bit7=1：后续 2 字节为一个 RGB565 色值，重复次数为低 7 位加 1。 */
            if((encoded_consumed + 2U) > encoded_length)
            {
                status = ANIMATION_STATUS_FRAME_ERROR;
                break;
            }
            if((GD25Q64_ReadByte(&high_byte) != GD25Q64_STATUS_OK) ||
               (GD25Q64_ReadByte(&low_byte) != GD25Q64_STATUS_OK))
            {
                status = ANIMATION_STATUS_FLASH_ERROR;
                break;
            }
            encoded_consumed += 2U;
            color = ((uint16_t)high_byte << 8U) | low_byte;
            if(LCD_StreamWriteRepeat(color, token_pixels) != LCD_STATUS_OK)
            {
                status = ANIMATION_STATUS_LCD_ERROR;
                break;
            }
        }
        else
        {
            /* bit7=0：后续紧跟 token_pixels 个原样 RGB565 像素，每像素高字节在前。 */
            if((encoded_consumed + ((uint32_t)token_pixels * 2U)) > encoded_length)
            {
                status = ANIMATION_STATUS_FRAME_ERROR;
                break;
            }
            for(pixel_index = 0U; pixel_index < token_pixels; pixel_index++)
            {
                if((GD25Q64_ReadByte(&high_byte) != GD25Q64_STATUS_OK) ||
                   (GD25Q64_ReadByte(&low_byte) != GD25Q64_STATUS_OK))
                {
                    status = ANIMATION_STATUS_FLASH_ERROR;
                    break;
                }
                encoded_consumed += 2U;
                color = ((uint16_t)high_byte << 8U) | low_byte;
                if(LCD_StreamWritePixel(color) != LCD_STATUS_OK)
                {
                    status = ANIMATION_STATUS_LCD_ERROR;
                    break;
                }
            }
            if(status != ANIMATION_STATUS_OK)
            {
                break;
            }
        }
        decoded_pixels += token_pixels;
    }

    if(lcd_open != 0U)
    {
        LCD_EndRegionWrite();
    }
    if(flash_open != 0U)
    {
        GD25Q64_EndRead();
    }
    if(status != ANIMATION_STATUS_OK)
    {
        return status;
    }
    if((encoded_consumed != encoded_length) || (decoded_pixels != expected_pixels))
    {
        /* 同时检查压缩字节和解码像素，防止合法前缀后附带垃圾数据或提前结束。 */
        return ANIMATION_STATUS_FRAME_ERROR;
    }
    return ANIMATION_STATUS_OK;
}

/**
 ******************************************************************************
  @功能：渲染当前帧并将播放索引推进到下一帧
  @日期：2026-07-13
  @参数：无
  @返回值：AnimationStatus - 成功返回 OK，失败返回底层错误并清除 ready 状态
  @使用说明：每次调用严格处理一帧；末帧完成后回绕到第 0 帧。失败时不推进帧号，避免错误后时间轴失真。
 ******************************************************************************
 */
AnimationStatus AnimationPlayer_RenderNext(void)
{
    AnimationStatus status;

    if(animation_ready == 0U)
    {
        return ANIMATION_STATUS_NOT_READY;
    }

    status = render_frame(animation_next_frame);
    if(status != ANIMATION_STATUS_OK)
    {
        animation_ready = 0U;
        return status;
    }

    animation_next_frame++;
    if(animation_next_frame >= ANIMATION_FRAME_COUNT)
    {
        animation_next_frame = 0U;
    }
    return ANIMATION_STATUS_OK;
}

/**
 ******************************************************************************
  @功能：返回下一次计划渲染的帧号
  @日期：2026-07-13
  @参数：无
  @返回值：uint32_t - 当前 animation_next_frame 值
  @使用说明：只用于状态观察，不改变播放器状态，也不保证与 ISR 帧事件计数同步。
 ******************************************************************************
 */
uint32_t AnimationPlayer_GetFrameIndex(void)
{
    return animation_next_frame;
}
