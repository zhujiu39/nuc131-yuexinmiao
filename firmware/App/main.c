#include <stdint.h>

#include "NUC131.h"
#include "animation_player.h"
#include "gd25q64.h"
#include "lcd_driver.h"

/* ======================== 系统与播放参数 ======================== */

#define SYSTEM_CORE_CLOCK_HZ         48000000UL  /* NUC131 目标核心频率，单位 Hz。 */
#define PLAYBACK_FRAME_RATE_HZ       30UL        /* 与资源包固定帧率一致，不允许动态变速。 */
#define FRAME_DUE_LIMIT              4U          /* 最多缓存 4 个待播放事件，超过即判定实时性失败。 */

/* ======================== 中断与主循环共享状态 ======================== */

static volatile uint8_t frame_due_count = 0U;    /* TIMER0 产生、主循环消费的待播放帧计数。 */
static volatile uint8_t playback_overrun = 0U;   /* 帧事件来不及消费时由 ISR 置位。 */

/**
 ******************************************************************************
  @功能：初始化 NUC131 系统、SPI0 和 TIMER0 时钟
  @日期：2026-07-13
  @参数：无
  @返回值：uint8_t - HIRC 稳定且核心时钟设置完成返回 1，否则返回 0
  @使用说明：调用前必须解锁保护寄存器；核心目标频率为 48MHz，SPI0 接 GD25Q64，
             TIMER0 只提供 30Hz 播放节拍。函数不配置 SPI0 与 TIMER0 的工作模式。
 ******************************************************************************
 */
static uint8_t system_clock_init(void)
{
    CLK_EnableXtalRC(CLK_PWRCON_OSC22M_EN_Msk);
    if(CLK_WaitClockReady(CLK_CLKSTATUS_OSC22M_STB_Msk) == 0U)
    {
        return 0U;
    }

    CLK_SetHCLK(CLK_CLKSEL0_HCLK_S_HIRC, CLK_CLKDIV_HCLK(1));

    /* 外部 12MHz 晶振用于厂商时钟函数建立 48MHz 核心时钟；等待结果由后续设置函数处理。 */
    CLK_EnableXtalRC(CLK_PWRCON_XTL12M_EN_Msk);
    (void)CLK_WaitClockReady(CLK_CLKSTATUS_XTL12M_STB_Msk);
    (void)CLK_SetCoreClock(SYSTEM_CORE_CLOCK_HZ);
    SystemCoreClockUpdate();

    /* 这里只打开模块时钟，具体协议模式分别由 GD25Q64_Init 和 playback_timer_start 配置。 */
    CLK_SetModuleClock(SPI0_MODULE, CLK_CLKSEL1_SPI0_S_HCLK, MODULE_NoMsk);
    CLK_EnableModuleClock(SPI0_MODULE);
    CLK_SetModuleClock(TMR0_MODULE, CLK_CLKSEL1_TMR0_S_HCLK, MODULE_NoMsk);
    CLK_EnableModuleClock(TMR0_MODULE);
    return 1U;
}

/**
 ******************************************************************************
  @功能：启动 30Hz 动画帧定时事件
  @日期：2026-07-13
  @参数：无
  @返回值：无
  @使用说明：TIMER0 使用 HCLK 作为时钟源并由厂商驱动计算重装值。中断只累计待处理帧数，
             GD25Q64 读取、RLE 解码和 LCD 刷新均在主循环执行。
 ******************************************************************************
 */
static void playback_timer_start(void)
{
    TIMER_Open(TIMER0, TIMER_PERIODIC_MODE, PLAYBACK_FRAME_RATE_HZ);
    TIMER_EnableInt(TIMER0);
    NVIC_EnableIRQ(TMR0_IRQn);
    TIMER_Start(TIMER0);
}

/**
 ******************************************************************************
  @功能：从待播放计数中原子地领取一个帧事件
  @日期：2026-07-13
  @参数：无
  @返回值：uint8_t - 成功领取事件返回 1，没有待处理事件返回 0
  @使用说明：frame_due_count 同时由 TIMER0 ISR 和主循环访问，因此在极短临界区内关中断。
             临界区只包含一次判断和一次减法，不会覆盖 Flash 读取或 LCD 刷新过程。
 ******************************************************************************
 */
static uint8_t take_frame_event(void)
{
    uint8_t event_available;

    __disable_irq();
    if(frame_due_count > 0U)
    {
        frame_due_count--;
        event_available = 1U;
    }
    else
    {
        event_available = 0U;
    }
    __enable_irq();
    return event_available;
}

/**
 ******************************************************************************
  @功能：原子读取并清除播放积压标志
  @日期：2026-07-13
  @参数：无
  @返回值：uint8_t - 已发生帧事件积压返回 1，否则返回 0
  @使用说明：标志由 TIMER0 ISR 置位，主循环读取后清零；该错误被视为无法维持原始 30fps，
             上层会进入青屏停机状态，禁止通过丢帧掩盖速度不足。
 ******************************************************************************
 */
static uint8_t take_playback_overrun(void)
{
    uint8_t overrun;

    __disable_irq();
    overrun = playback_overrun;
    playback_overrun = 0U;
    __enable_irq();
    return overrun;
}

/**
 ******************************************************************************
  @功能：停止播放并用纯色屏幕锁存不可恢复错误
  @日期：2026-07-13
  @参数：[输入] color - 表示错误类别的 RGB565 颜色
  @返回值：无
  @使用说明：停止 TIMER0 后进入 WFI 永久等待，避免错误资源继续读写。当前产品使用重新上电或复位恢复；
             红色=Flash，洋红色=资源，黄色=播放链路，青色=实时性超限。
 ******************************************************************************
 */
static void enter_visible_error(uint16_t color)
{
    TIMER_Stop(TIMER0);
    (void)LCD_Clear(color);
    while(1)
    {
        __WFI();
    }
}

/**
 ******************************************************************************
  @功能：GD25Q64 动画播放程序入口
  @日期：2026-07-13
  @参数：无
  @返回值：int - 裸机主循环不返回
  @使用说明：红屏表示 Flash/ID 异常，洋红屏表示资源包异常，黄屏表示播放期读写或解码异常，青屏表示刷新超时。
 ******************************************************************************
 */
int main(void)
{
    AnimationStatus animation_status;

    /* 第一阶段：保护寄存器解锁期间只完成系统与外设模块时钟配置。 */
    SYS_UnlockReg();
    if(system_clock_init() == 0U)
    {
        SYS_LockReg();
        while(1)
        {
        }
    }
    SYS_LockReg();

    /* 第二阶段：先建立显示输出，再检查外部 Flash 型号和资源包一致性。 */
    (void)LCD_Init();
    if(GD25Q64_Init() != GD25Q64_STATUS_OK)
    {
        enter_visible_error(LCD_COLOR_RED);
    }

    animation_status = AnimationPlayer_Init();
    if(animation_status != ANIMATION_STATUS_OK)
    {
        enter_visible_error(LCD_COLOR_MAGENTA);
    }

    /* 第 0 帧在定时器启动前立即绘制，随后每个 30Hz 事件严格推进一帧。 */
    (void)LCD_Clear(LCD_COLOR_BLACK);
    animation_status = AnimationPlayer_RenderNext();
    if(animation_status != ANIMATION_STATUS_OK)
    {
        enter_visible_error(LCD_COLOR_YELLOW);
    }

    playback_timer_start();
    while(1)
    {
        /* 优先检查积压，确保固件不会通过静默丢帧改变动画速度和动作节奏。 */
        if(take_playback_overrun() != 0U)
        {
            enter_visible_error(LCD_COLOR_CYAN);
        }

        if(take_frame_event() != 0U)
        {
            animation_status = AnimationPlayer_RenderNext();
            if(animation_status != ANIMATION_STATUS_OK)
            {
                enter_visible_error(LCD_COLOR_YELLOW);
            }
        }
        else
        {
            /* 无事件时休眠，任一已使能中断均可唤醒内核。 */
            __WFI();
        }
    }
}

/**
 ******************************************************************************
  @功能：TIMER0 30Hz 播放节拍中断
  @日期：2026-07-13
  @参数：无
  @返回值：无
  @使用说明：只清除中断标志、累计帧事件或置积压标志；不访问 Flash、不解码、不刷屏。
             累计达到上限说明单帧刷新超出实时预算，但仍不跳过动画帧。
 ******************************************************************************
 */
void TMR0_IRQHandler(void)
{
    if(TIMER_GetIntFlag(TIMER0) != 0U)
    {
        TIMER_ClearIntFlag(TIMER0);
        if(frame_due_count < FRAME_DUE_LIMIT)
        {
            frame_due_count++;
        }
        else
        {
            playback_overrun = 1U;
        }
    }
}
