#ifndef YUEXINMIAO_ANIMATION_PLAYER_H
#define YUEXINMIAO_ANIMATION_PLAYER_H

#include <stdint.h>

/* ======================== 动画播放状态 ======================== */

typedef enum
{
    ANIMATION_STATUS_OK = 0,             /* 当前操作完成，播放器状态有效。 */
    ANIMATION_STATUS_NOT_READY = 1,      /* 尚未初始化，或此前播放错误已使播放器失效。 */
    ANIMATION_STATUS_FLASH_ERROR = 2,    /* GD25Q64 读取失败或 SPI0 超时。 */
    ANIMATION_STATUS_HEADER_ERROR = 3,   /* 包头字段、尺寸、帧率、格式或头 CRC 不匹配。 */
    ANIMATION_STATUS_INDEX_ERROR = 4,    /* 帧索引 CRC、偏移顺序或数据区边界非法。 */
    ANIMATION_STATUS_FRAME_ERROR = 5,    /* 帧记录长度、矩形或 PackBits 数据非法。 */
    ANIMATION_STATUS_LCD_ERROR = 6       /* LCD 窗口参数或流式像素写入失败。 */
} AnimationStatus;

/**
 ******************************************************************************
  @功能：校验 GD25Q64 中的动画包头和帧索引
  @日期：2026-07-13
  @参数：无
  @返回值：AnimationStatus - 包头、关键参数和索引 CRC 均正确时返回 ANIMATION_STATUS_OK
  @使用说明：调用前必须完成 GD25Q64_Init 和 LCD_Init。函数会从 Flash 地址 0 读取 256 字节包头，
             并分块校验整张帧索引；成功后下一次 RenderNext 从第 0 帧开始。
 ******************************************************************************
 */
AnimationStatus AnimationPlayer_Init(void);

/**
 ******************************************************************************
  @功能：按资源索引顺序渲染下一帧并在末帧后循环
  @日期：2026-07-13
  @参数：无
  @返回值：AnimationStatus - Flash、RLE 或 LCD 写入异常时返回对应错误
  @使用说明：由主循环在 30Hz 定时事件到达后调用，不允许在定时器中断中刷屏。每次成功只推进一帧，
             第 1209 帧完成后回到第 0 帧；失败后播放器失效，必须重新初始化才能继续。
 ******************************************************************************
 */
AnimationStatus AnimationPlayer_RenderNext(void);

/**
 ******************************************************************************
  @功能：查询下一次将要渲染的帧索引
  @日期：2026-07-13
  @参数：无
  @返回值：uint32_t - 范围为 0～ANIMATION_FRAME_COUNT-1 的下一帧索引
  @使用说明：只读取播放器内部状态，不触发 Flash 或 LCD 操作；当前工程仅供调试观察使用。
 ******************************************************************************
 */
uint32_t AnimationPlayer_GetFrameIndex(void);

#endif
