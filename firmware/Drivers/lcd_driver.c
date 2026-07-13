#include <stdint.h>

#include "NUC131.h"
#include "lcd_config.h"
#include "lcd_driver.h"

/* ======================== 控制器命令 ======================== */

#define LCD_CMD_SLEEP_OUT        0x11U
#define LCD_CMD_COLUMN_ADDRESS   0x2AU
#define LCD_CMD_ROW_ADDRESS      0x2BU
#define LCD_CMD_MEMORY_WRITE     0x2CU
#define LCD_CMD_MEMORY_ACCESS    0x36U
#define LCD_CMD_PIXEL_FORMAT     0x3AU
#define LCD_CMD_DISPLAY_ON       0x29U

static uint8_t lcd_stream_active = 0U; /* 为 1 时 LCD CS 保持低电平，像素数据可连续写入。 */

/* ======================== 底层串行写入 ======================== */

/**
 ******************************************************************************
  @功能：使用 NUC131 SysTick 微秒延时组合毫秒级 LCD 复位等待
  @日期：2026-07-13
  @参数：[输入] delay_ms - 延时长度，单位毫秒
  @返回值：无
  @使用说明：只在上电初始化和硬复位阶段调用，不用于动画调度；依赖 SystemCoreClock 已正确更新。
 ******************************************************************************
 */
static void lcd_delay_ms(uint16_t delay_ms)
{
    uint16_t index;

    for(index = 0U; index < delay_ms; index++)
    {
        CLK_SysTickDelay(1000U);
    }
}

/**
 ******************************************************************************
  @功能：通过 GPIO 模拟串行接口发送一个字节
  @日期：2026-07-13
  @参数：[输入] data - 待发送的命令或数据字节
  @返回值：无
  @使用说明：按最高位优先发送，数据在 SCL 上升沿前稳定。函数不控制 CS 和 DC，由调用者选择事务类型。
 ******************************************************************************
 */
static void lcd_write_byte(uint8_t data)
{
    uint8_t bit_index;

    for(bit_index = 0U; bit_index < 8U; bit_index++)
    {
        if((data & 0x80U) != 0U)
        {
            LCD_SDA_HIGH();
        }
        else
        {
            LCD_SDA_LOW();
        }

        LCD_SCL_LOW();
        LCD_SCL_HIGH();
        data <<= 1U;
    }
}

/**
 ******************************************************************************
  @功能：发送一个 LCD 控制器命令字节
  @日期：2026-07-13
  @参数：[输入] command - 控制器命令
  @返回值：无
  @使用说明：独立产生一次 CS 低脉冲，期间 DC=0；不得在连续像素流已打开时调用。
 ******************************************************************************
 */
static void lcd_write_command(uint8_t command)
{
    LCD_CS_LOW();
    LCD_DC_LOW();
    lcd_write_byte(command);
    LCD_CS_HIGH();
}

/**
 ******************************************************************************
  @功能：发送一个 LCD 控制器参数字节
  @日期：2026-07-13
  @参数：[输入] data - 命令参数
  @返回值：无
  @使用说明：独立产生一次 CS 低脉冲，期间 DC=1；用于初始化和设置显存窗口。
 ******************************************************************************
 */
static void lcd_write_data8(uint8_t data)
{
    LCD_CS_LOW();
    LCD_DC_HIGH();
    lcd_write_byte(data);
    LCD_CS_HIGH();
}

/**
 ******************************************************************************
  @功能：在已选中的 LCD 数据事务内重复写入同一 RGB565 颜色
  @日期：2026-07-13
  @参数：[输入] color - RGB565 像素值
        [输入] pixel_count - 写入像素数量
  @返回值：无
  @使用说明：按颜色高字节、低字节发送；函数不检查窗口状态，也不切换 CS/DC，只供驱动内部调用。
 ******************************************************************************
 */
static void lcd_write_color_repeat_raw(uint16_t color, uint32_t pixel_count)
{
    uint8_t color_high;
    uint8_t color_low;

    color_high = (uint8_t)(color >> 8U);
    color_low = (uint8_t)color;

    while(pixel_count > 0U)
    {
        lcd_write_byte(color_high);
        lcd_write_byte(color_low);
        pixel_count--;
    }
}

/**
 ******************************************************************************
  @功能：配置 LCD GPIO 方向并建立安全的上电默认电平
  @日期：2026-07-13
  @参数：无
  @返回值：无
  @引脚连接：PB15=SCL，PC14=SDA，PC15=CS，PC7=DC，PA7=RST，PC6=背光
  @使用说明：初始化期间背光保持关闭，CS/SCL/SDA/DC/RST 置高，避免上电毛刺被控制器解释为命令。
 ******************************************************************************
 */
static void lcd_gpio_init(void)
{
    GPIO_SetMode(PB, BIT15, GPIO_PMD_OUTPUT);
    GPIO_SetMode(PC, BIT14, GPIO_PMD_OUTPUT);
    GPIO_SetMode(PC, BIT15, GPIO_PMD_OUTPUT);
    GPIO_SetMode(PC, BIT6, GPIO_PMD_OUTPUT);
    GPIO_SetMode(PC, BIT7, GPIO_PMD_OUTPUT);
    GPIO_SetMode(PA, BIT7, GPIO_PMD_OUTPUT);

    LCD_CS_HIGH();
    LCD_SCL_HIGH();
    LCD_SDA_HIGH();
    LCD_DC_HIGH();
    LCD_RESET_HIGH();
    LCD_BACKLIGHT_LOW();
}

/**
 ******************************************************************************
  @功能：对 LCD 控制器执行低有效硬复位
  @日期：2026-07-13
  @参数：无
  @返回值：无
  @使用说明：RST 低保持 100ms，释放后等待 50ms；时序沿用已实物验证的参考工程配置。
 ******************************************************************************
 */
static void lcd_reset(void)
{
    LCD_RESET_LOW();
    lcd_delay_ms(100U);
    LCD_RESET_HIGH();
    lcd_delay_ms(50U);
}

/**
 ******************************************************************************
  @功能：设置 LCD 显存列、行范围并发出 Memory Write 命令
  @日期：2026-07-13
  @参数：[输入] x_start - 逻辑起始列
        [输入] y_start - 逻辑起始行
        [输入] x_end - 逻辑结束列，包含该列
        [输入] y_end - 逻辑结束行，包含该行
  @返回值：LcdStatus - 坐标顺序和边界有效返回 OK，否则返回 INVALID_PARAM
  @使用说明：逻辑坐标自动叠加面板 X+1、Y+2 偏移；调用结束后仅完成开窗命令，尚未保持 CS 低电平。
 ******************************************************************************
 */
static LcdStatus lcd_set_region(uint16_t x_start,
                                uint16_t y_start,
                                uint16_t x_end,
                                uint16_t y_end)
{
    if((x_start > x_end) || (y_start > y_end) ||
       (x_end >= LCD_WIDTH) || (y_end >= LCD_HEIGHT))
    {
        return LCD_STATUS_INVALID_PARAM;
    }

    lcd_write_command(LCD_CMD_COLUMN_ADDRESS);
    lcd_write_data8(0x00U);
    lcd_write_data8((uint8_t)(x_start + LCD_X_OFFSET));
    lcd_write_data8(0x00U);
    lcd_write_data8((uint8_t)(x_end + LCD_X_OFFSET));

    lcd_write_command(LCD_CMD_ROW_ADDRESS);
    lcd_write_data8(0x00U);
    lcd_write_data8((uint8_t)(y_start + LCD_Y_OFFSET));
    lcd_write_data8(0x00U);
    lcd_write_data8((uint8_t)(y_end + LCD_Y_OFFSET));

    lcd_write_command(LCD_CMD_MEMORY_WRITE);
    return LCD_STATUS_OK;
}

/**
 ******************************************************************************
  @功能：发送参考工程中已实物验证的 LCD 控制器初始化序列
  @日期：2026-07-13
  @参数：无
  @返回值：无
  @使用说明：配置帧率、电源、扫描方向、Gamma、RGB565 格式和显示开关。具体寄存器值与当前面板绑定，
             更换控制器或扫描方向时必须整体复核，不能只修改宽高宏。
 ******************************************************************************
 */
static void lcd_controller_init_sequence(void)
{
    /* 退出睡眠后必须保留控制器稳定时间，随后才能写电源和显示参数。 */
    lcd_write_command(LCD_CMD_SLEEP_OUT);
    lcd_delay_ms(120U);

    /* B1/B2/B3：正常、空闲、局部显示模式的帧率参数。 */
    lcd_write_command(0xB1U);
    lcd_write_data8(0x01U);
    lcd_write_data8(0x2CU);
    lcd_write_data8(0x2DU);

    lcd_write_command(0xB2U);
    lcd_write_data8(0x01U);
    lcd_write_data8(0x2CU);
    lcd_write_data8(0x2DU);

    lcd_write_command(0xB3U);
    lcd_write_data8(0x01U);
    lcd_write_data8(0x2CU);
    lcd_write_data8(0x2DU);
    lcd_write_data8(0x01U);
    lcd_write_data8(0x2CU);
    lcd_write_data8(0x2DU);

    /* B4：显示反转控制。 */
    lcd_write_command(0xB4U);
    lcd_write_data8(0x07U);

    /* C0～C5：电源、VCOM 与驱动电压参数，保持参考面板实测值。 */
    lcd_write_command(0xC0U);
    lcd_write_data8(0xA2U);
    lcd_write_data8(0x02U);
    lcd_write_data8(0x84U);

    lcd_write_command(0xC1U);
    lcd_write_data8(0xC5U);

    lcd_write_command(0xC2U);
    lcd_write_data8(0x0AU);
    lcd_write_data8(0x00U);

    lcd_write_command(0xC3U);
    lcd_write_data8(0x8AU);
    lcd_write_data8(0x2AU);

    lcd_write_command(0xC4U);
    lcd_write_data8(0x8AU);
    lcd_write_data8(0xEEU);

    lcd_write_command(0xC5U);
    lcd_write_data8(0x0EU);

    /* 0x36=0x68：固定当前扫描方向和 RGB/BGR 顺序。 */
    lcd_write_command(LCD_CMD_MEMORY_ACCESS);
    lcd_write_data8(0x68U);

    /* E0/E1：正、负极性 Gamma 曲线。 */
    lcd_write_command(0xE0U);
    lcd_write_data8(0x0FU);
    lcd_write_data8(0x1AU);
    lcd_write_data8(0x0FU);
    lcd_write_data8(0x18U);
    lcd_write_data8(0x2FU);
    lcd_write_data8(0x28U);
    lcd_write_data8(0x20U);
    lcd_write_data8(0x22U);
    lcd_write_data8(0x1FU);
    lcd_write_data8(0x1BU);
    lcd_write_data8(0x23U);
    lcd_write_data8(0x37U);
    lcd_write_data8(0x00U);
    lcd_write_data8(0x07U);
    lcd_write_data8(0x02U);
    lcd_write_data8(0x10U);

    lcd_write_command(0xE1U);
    lcd_write_data8(0x0FU);
    lcd_write_data8(0x1BU);
    lcd_write_data8(0x0FU);
    lcd_write_data8(0x17U);
    lcd_write_data8(0x33U);
    lcd_write_data8(0x2CU);
    lcd_write_data8(0x29U);
    lcd_write_data8(0x2EU);
    lcd_write_data8(0x30U);
    lcd_write_data8(0x30U);
    lcd_write_data8(0x39U);
    lcd_write_data8(0x3FU);
    lcd_write_data8(0x00U);
    lcd_write_data8(0x07U);
    lcd_write_data8(0x03U);
    lcd_write_data8(0x10U);

    /* 初始化控制器可访问范围；实际绘制窗口还会叠加 X/Y 面板偏移。 */
    lcd_write_command(LCD_CMD_COLUMN_ADDRESS);
    lcd_write_data8(0x00U);
    lcd_write_data8(0x00U);
    lcd_write_data8(0x00U);
    lcd_write_data8(0x83U);

    lcd_write_command(LCD_CMD_ROW_ADDRESS);
    lcd_write_data8(0x00U);
    lcd_write_data8(0x00U);
    lcd_write_data8(0x00U);
    lcd_write_data8(0x83U);

    /* F0/F6 为参考工程保留的接口相关寄存器；当前面板已验证，不对未公开字段语义另作假设。 */
    lcd_write_command(0xF0U);
    lcd_write_data8(0x01U);

    lcd_write_command(0xF6U);
    lcd_write_data8(0x00U);

    /* 0x3A=0x05 选择每像素 16 位 RGB565，必须与资源包字节序保持一致。 */
    lcd_write_command(LCD_CMD_PIXEL_FORMAT);
    lcd_write_data8(0x05U);

    /* 所有参数写入完成后再打开显示输出。 */
    lcd_write_command(LCD_CMD_DISPLAY_ON);
}

/* ======================== 公共接口 ======================== */

/**
 ******************************************************************************
  @功能：完成 LCD GPIO、硬复位、控制器寄存器、清屏和背光初始化
  @日期：2026-07-13
  @参数：无
  @返回值：LcdStatus - 当前单向写接口完成初始化后固定返回 OK
  @使用说明：背光在初始化和清屏完成前保持关闭，避免用户看到随机显存。LCD 无 MISO/读回线，
             本函数无法确认控制器是否真实响应，硬件连接问题需通过实际画面判断。
 ******************************************************************************
 */
LcdStatus LCD_Init(void)
{
    lcd_gpio_init();
    lcd_reset();
    lcd_controller_init_sequence();
    (void)LCD_Clear(LCD_COLOR_BLACK);
    LCD_SetBacklight(1U);

    return LCD_STATUS_OK;
}

/**
 ******************************************************************************
  @功能：设置 PC6 高有效背光输出
  @日期：2026-07-13
  @参数：[输入] enabled - 0 关闭，非 0 打开
  @返回值：无
  @使用说明：只控制背光电平，不改变 LCD 控制器睡眠或显示状态。
 ******************************************************************************
 */
void LCD_SetBacklight(uint8_t enabled)
{
    if(enabled != 0U)
    {
        LCD_BACKLIGHT_HIGH();
    }
    else
    {
        LCD_BACKLIGHT_LOW();
    }
}

/**
 ******************************************************************************
  @功能：用指定颜色同步覆盖完整逻辑屏幕
  @日期：2026-07-13
  @参数：[输入] color - RGB565 颜色值
  @返回值：LcdStatus - 透传全屏 LCD_Fill 结果
  @使用说明：完整写入 16384 个像素，当前仅用于上电黑屏和错误纯色提示。
 ******************************************************************************
 */
LcdStatus LCD_Clear(uint16_t color)
{
    return LCD_Fill(0U, 0U, LCD_WIDTH, LCD_HEIGHT, color);
}

/**
 ******************************************************************************
  @功能：校验并打开一个连续 RGB565 像素写入窗口
  @日期：2026-07-13
  @参数：[输入] x - 逻辑左上角 X
        [输入] y - 逻辑左上角 Y
        [输入] width - 窗口宽度
        [输入] height - 窗口高度
  @返回值：LcdStatus - 参数和窗口有效返回 OK，否则返回 INVALID_PARAM
  @使用说明：若发现上一次流未关闭，会先释放 CS 并复位软件状态。成功后 CS=0、DC=1，
             之后只能发送像素数据，直到调用 LCD_EndRegionWrite。
 ******************************************************************************
 */
LcdStatus LCD_BeginRegionWrite(uint16_t x, uint16_t y, uint16_t width, uint16_t height)
{
    if(lcd_stream_active != 0U)
    {
        LCD_CS_HIGH();
        lcd_stream_active = 0U;
    }

    if((width == 0U) || (height == 0U) ||
       (x >= LCD_WIDTH) || (y >= LCD_HEIGHT) ||
       (width > (LCD_WIDTH - x)) || (height > (LCD_HEIGHT - y)))
    {
        return LCD_STATUS_INVALID_PARAM;
    }

    if(lcd_set_region(x,
                      y,
                      (uint16_t)(x + width - 1U),
                      (uint16_t)(y + height - 1U)) != LCD_STATUS_OK)
    {
        return LCD_STATUS_INVALID_PARAM;
    }

    LCD_CS_LOW();
    LCD_DC_HIGH();
    lcd_stream_active = 1U;
    return LCD_STATUS_OK;
}

/**
 ******************************************************************************
  @功能：向当前窗口写入一个高字节在前的 RGB565 像素
  @日期：2026-07-13
  @参数：[输入] color - RGB565 像素值
  @返回值：LcdStatus - 流打开返回 OK，否则返回 INVALID_PARAM
  @使用说明：不重新设置坐标，控制器按窗口扫描方向自动移动到下一像素。
 ******************************************************************************
 */
LcdStatus LCD_StreamWritePixel(uint16_t color)
{
    if(lcd_stream_active == 0U)
    {
        return LCD_STATUS_INVALID_PARAM;
    }

    lcd_write_byte((uint8_t)(color >> 8U));
    lcd_write_byte((uint8_t)color);
    return LCD_STATUS_OK;
}

/**
 ******************************************************************************
  @功能：向当前窗口重复写入同一 RGB565 像素
  @日期：2026-07-13
  @参数：[输入] color - RGB565 像素值
        [输入] count - 重复次数
  @返回值：LcdStatus - 流打开且次数非零返回 OK，否则返回 INVALID_PARAM
  @使用说明：不检查 count 是否超过剩余窗口像素，调用者必须以资源帧解码像素总数约束写入量。
 ******************************************************************************
 */
LcdStatus LCD_StreamWriteRepeat(uint16_t color, uint16_t count)
{
    if((lcd_stream_active == 0U) || (count == 0U))
    {
        return LCD_STATUS_INVALID_PARAM;
    }

    lcd_write_color_repeat_raw(color, count);
    return LCD_STATUS_OK;
}

/**
 ******************************************************************************
  @功能：释放 LCD 片选并清除连续写入状态
  @日期：2026-07-13
  @参数：无
  @返回值：无
  @使用说明：只有流已打开时才切换 CS；允许正常路径和错误清理路径重复调用。
 ******************************************************************************
 */
void LCD_EndRegionWrite(void)
{
    if(lcd_stream_active != 0U)
    {
        LCD_CS_HIGH();
        lcd_stream_active = 0U;
    }
}

/**
 ******************************************************************************
  @功能：用单色填充一个逻辑矩形
  @日期：2026-07-13
  @参数：[输入] x - 左上角 X
        [输入] y - 左上角 Y
        [输入] width - 宽度
        [输入] height - 高度
        [输入] color - RGB565 颜色
  @返回值：LcdStatus - 区域完整位于屏幕内返回 OK，否则返回 INVALID_PARAM
  @使用说明：先使用减法形式检查区域剩余空间，再以 32 位乘法计算像素数，避免 16 位乘法溢出。
 ******************************************************************************
 */
LcdStatus LCD_Fill(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint16_t color)
{
    uint32_t pixel_count;

    if((width == 0U) || (height == 0U) ||
       (x >= LCD_WIDTH) || (y >= LCD_HEIGHT) ||
       (width > (LCD_WIDTH - x)) || (height > (LCD_HEIGHT - y)))
    {
        return LCD_STATUS_INVALID_PARAM;
    }

    if(LCD_BeginRegionWrite(x, y, width, height) != LCD_STATUS_OK)
    {
        return LCD_STATUS_INVALID_PARAM;
    }

    pixel_count = (uint32_t)width * (uint32_t)height;
    lcd_write_color_repeat_raw(color, pixel_count);
    LCD_EndRegionWrite();
    return LCD_STATUS_OK;
}

/**
 ******************************************************************************
  @功能：设置单像素窗口并写入一个 RGB565 像素
  @日期：2026-07-13
  @参数：[输入] x - 像素 X
        [输入] y - 像素 Y
        [输入] color - RGB565 颜色
  @返回值：LcdStatus - 坐标有效返回 OK，否则返回 INVALID_PARAM
  @使用说明：接口会为每个像素重新开窗，适合低频图元；动画刷新应使用连续流式写入。
 ******************************************************************************
 */
LcdStatus LCD_DrawPixel(uint16_t x, uint16_t y, uint16_t color)
{
    if((x >= LCD_WIDTH) || (y >= LCD_HEIGHT))
    {
        return LCD_STATUS_INVALID_PARAM;
    }

    if(LCD_BeginRegionWrite(x, y, 1U, 1U) != LCD_STATUS_OK)
    {
        return LCD_STATUS_INVALID_PARAM;
    }

    (void)LCD_StreamWritePixel(color);
    LCD_EndRegionWrite();
    return LCD_STATUS_OK;
}

/**
 ******************************************************************************
  @功能：使用整数 Bresenham 算法连接两个屏内端点
  @日期：2026-07-13
  @参数：[输入] x0 - 起点 X
        [输入] y0 - 起点 Y
        [输入] x1 - 终点 X
        [输入] y1 - 终点 Y
        [输入] color - RGB565 颜色
  @返回值：LcdStatus - 端点有效且绘制完成返回 OK，否则返回 INVALID_PARAM
  @使用说明：不做线段裁剪，任一端点越界即拒绝；算法每步调用 DrawPixel，不用于动画高频路径。
 ******************************************************************************
 */
LcdStatus LCD_DrawLine(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color)
{
    int32_t current_x;
    int32_t current_y;
    int32_t target_x;
    int32_t target_y;
    int32_t delta_x;
    int32_t delta_y;
    int32_t step_x;
    int32_t step_y;
    int32_t error;
    int32_t error_twice;

    if((x0 >= LCD_WIDTH) || (x1 >= LCD_WIDTH) ||
       (y0 >= LCD_HEIGHT) || (y1 >= LCD_HEIGHT))
    {
        return LCD_STATUS_INVALID_PARAM;
    }

    current_x = (int32_t)x0;
    current_y = (int32_t)y0;
    target_x = (int32_t)x1;
    target_y = (int32_t)y1;
    delta_x = (current_x < target_x) ? (target_x - current_x) : (current_x - target_x);
    delta_y = (current_y < target_y) ? (target_y - current_y) : (current_y - target_y);
    step_x = (current_x < target_x) ? 1 : -1;
    step_y = (current_y < target_y) ? 1 : -1;
    error = delta_x - delta_y;

    for(;;)
    {
        (void)LCD_DrawPixel((uint16_t)current_x, (uint16_t)current_y, color);
        if((current_x == target_x) && (current_y == target_y))
        {
            break;
        }

        error_twice = error << 1;
        if(error_twice > -delta_y)
        {
            error -= delta_y;
            current_x += step_x;
        }
        if(error_twice < delta_x)
        {
            error += delta_x;
            current_y += step_y;
        }
    }

    return LCD_STATUS_OK;
}
