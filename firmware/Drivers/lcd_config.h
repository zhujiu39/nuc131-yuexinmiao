#ifndef YUEXINMIAO_LCD_CONFIG_H
#define YUEXINMIAO_LCD_CONFIG_H

/* ======================== 屏幕硬件配置 ======================== */

/* 参考 zhongtuo-src 当前量产工程并经本机实物验证的 128x128 RGB565 显示配置。 */
#define LCD_WIDTH                128U
#define LCD_HEIGHT               128U
#define LCD_X_OFFSET             1U      /* 逻辑 X=0 对应控制器显存列 1。 */
#define LCD_Y_OFFSET             2U      /* 逻辑 Y=0 对应控制器显存行 2。 */

/* GPIO 模拟串行接口：最高位先发，SCL 上升沿锁存；LCD 只写，无数据回读引脚。 */
#define LCD_SCL_HIGH()           (PB15 = 1)
#define LCD_SCL_LOW()            (PB15 = 0)
#define LCD_SDA_HIGH()           (PC14 = 1)
#define LCD_SDA_LOW()            (PC14 = 0)
#define LCD_CS_HIGH()            (PC15 = 1)
#define LCD_CS_LOW()             (PC15 = 0)
#define LCD_DC_HIGH()            (PC7 = 1)
#define LCD_DC_LOW()             (PC7 = 0)

/* PA7 为低有效硬复位，PC6 为高有效背光；初始化完成前背光保持关闭。 */
#define LCD_RESET_HIGH()         (PA7 = 1)
#define LCD_RESET_LOW()          (PA7 = 0)
#define LCD_BACKLIGHT_HIGH()     (PC6 = 1)
#define LCD_BACKLIGHT_LOW()      (PC6 = 0)

#endif
