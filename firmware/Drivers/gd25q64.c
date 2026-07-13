#include <stdint.h>

#include "NUC131.h"
#include "gd25q64.h"

/* ======================== 指令与总线参数 ======================== */

#define GD25Q64_CMD_READ_DATA        0x03U        /* 普通读：指令后发送 24 位地址，无需空周期。 */
#define GD25Q64_CMD_READ_JEDEC_ID    0x9FU        /* 读取制造商、存储类型和容量代码。 */
#define GD25Q64_SPI_CLOCK_HZ         10000000UL   /* 实物验证通过的 SPI0 时钟频率。 */
#define GD25Q64_SPI_TIMEOUT_LOOPS    100000UL     /* 单字节传输软件轮询上限，防止硬件异常卡死。 */

/* ======================== 连续读取状态 ======================== */

static uint8_t gd25q64_read_open = 0U;            /* 为 1 时 CS# 保持低电平，禁止开启第二个事务。 */

/**
 ******************************************************************************
  @功能：通过 SPI0 全双工交换一个字节并执行有限等待
  @日期：2026-07-13
  @参数：[输入] transmit - 写入 SPI0 发送寄存器的字节
        [输出] receive - 保存同一时钟周期内收到的字节
  @返回值：Gd25q64Status - 参数有效且传输结束返回 OK，超时返回 SPI_TIMEOUT
  @使用说明：调用前 SPI0 必须已配置为 Mode0、8 位主机；函数不控制 CS#，由上层事务管理。
             超时值是软件循环边界，不作为精确时间基准。
 ******************************************************************************
 */
static Gd25q64Status gd25q64_transfer(uint8_t transmit, uint8_t *receive)
{
    uint32_t timeout;

    if(receive == 0)
    {
        return GD25Q64_STATUS_INVALID_PARAM;
    }

    timeout = GD25Q64_SPI_TIMEOUT_LOOPS;
    SPI_WRITE_TX(SPI0, transmit);
    SPI_TRIGGER(SPI0);
    while((SPI_IS_BUSY(SPI0) != 0U) && (timeout > 0U))
    {
        timeout--;
    }

    if(timeout == 0U)
    {
        return GD25Q64_STATUS_SPI_TIMEOUT;
    }

    *receive = (uint8_t)SPI_READ_RX(SPI0);
    return GD25Q64_STATUS_OK;
}

/**
 ******************************************************************************
  @功能：发送一个无需保留返回值的 SPI 字节
  @日期：2026-07-13
  @参数：[输入] data - 需要发送的指令或地址字节
  @返回值：Gd25q64Status - 透传底层交换结果
  @使用说明：Flash 指令和地址阶段仍会产生接收字节，本函数将其丢弃。
 ******************************************************************************
 */
static Gd25q64Status gd25q64_send(uint8_t data)
{
    uint8_t discard;
    return gd25q64_transfer(data, &discard);
}

/**
 ******************************************************************************
  @功能：配置 GD25Q64 的 SPI0 接口并确认器件型号
  @日期：2026-07-13
  @参数：无
  @返回值：Gd25q64Status - SPI 读取成功且 JEDEC ID 为 C8 40 17 时返回 OK
  @使用说明：SPI0 使用 PC0～PC3、Mode0、8 位、10MHz；初始化后 CS# 保持高电平。
             当前实现会重写 GPC_MFP/ALT_MFP 复用寄存器；本板其余 PC 引脚按 GPIO 使用，移植时必须复核。
 ******************************************************************************
 */
Gd25q64Status GD25Q64_Init(void)
{
    uint32_t jedec_id;
    Gd25q64Status status;

    SPI_Open(SPI0, SPI_MASTER, SPI_MODE_0, 8U, GD25Q64_SPI_CLOCK_HZ);
    SYS->GPC_MFP = SYS_GPC_MFP_PC0_SPI0_SS0 |
                   SYS_GPC_MFP_PC1_SPI0_CLK |
                   SYS_GPC_MFP_PC2_SPI0_MISO0 |
                   SYS_GPC_MFP_PC3_SPI0_MOSI0;
    SYS->ALT_MFP = SYS_ALT_MFP_PC0_SPI0_SS0 |
                   SYS_ALT_MFP_PC1_SPI0_CLK |
                   SYS_ALT_MFP_PC2_SPI0_MISO0 |
                   SYS_ALT_MFP_PC3_SPI0_MOSI0;
    SPI_SET_SS0_HIGH(SPI0);
    gd25q64_read_open = 0U;

    /* 上电即核对完整三字节 ID，避免将其他容量 Flash 按 8MiB 边界访问。 */
    status = GD25Q64_ReadJedecId(&jedec_id);
    if(status != GD25Q64_STATUS_OK)
    {
        return status;
    }
    if(jedec_id != GD25Q64_EXPECTED_JEDEC_ID)
    {
        return GD25Q64_STATUS_ID_MISMATCH;
    }
    return GD25Q64_STATUS_OK;
}

/**
 ******************************************************************************
  @功能：执行 0x9F 指令并读取三字节 JEDEC ID
  @日期：2026-07-13
  @参数：[输出] jedec_id - 低 24 位返回 manufacturer/type/capacity
  @返回值：Gd25q64Status - 成功返回 OK，空指针或任一 SPI 字节超时返回对应错误
  @使用说明：无论中间步骤是否失败，函数退出前都会拉高 CS#，不会遗留未结束事务。
 ******************************************************************************
 */
Gd25q64Status GD25Q64_ReadJedecId(uint32_t *jedec_id)
{
    uint8_t manufacturer;
    uint8_t memory_type;
    uint8_t capacity;
    Gd25q64Status status;

    if(jedec_id == 0)
    {
        return GD25Q64_STATUS_INVALID_PARAM;
    }

    SPI_SET_SS0_LOW(SPI0);
    status = gd25q64_send(GD25Q64_CMD_READ_JEDEC_ID);
    if(status == GD25Q64_STATUS_OK)
    {
        status = gd25q64_transfer(0xFFU, &manufacturer);
    }
    if(status == GD25Q64_STATUS_OK)
    {
        status = gd25q64_transfer(0xFFU, &memory_type);
    }
    if(status == GD25Q64_STATUS_OK)
    {
        status = gd25q64_transfer(0xFFU, &capacity);
    }
    SPI_SET_SS0_HIGH(SPI0);

    if(status != GD25Q64_STATUS_OK)
    {
        return status;
    }

    *jedec_id = ((uint32_t)manufacturer << 16U) |
                ((uint32_t)memory_type << 8U) |
                (uint32_t)capacity;
    return GD25Q64_STATUS_OK;
}

/**
 ******************************************************************************
  @功能：打开从指定地址开始的 0x03 连续读取事务
  @日期：2026-07-13
  @参数：[输入] address - 三字节绝对地址
  @返回值：Gd25q64Status - 地址有效且命令阶段发送完成返回 OK
  @使用说明：按高、中、低顺序发送 24 位地址；成功后 CS# 保持低电平，GD25Q64 会自动递增内部地址。
 ******************************************************************************
 */
Gd25q64Status GD25Q64_BeginRead(uint32_t address)
{
    Gd25q64Status status;

    if((address >= GD25Q64_CAPACITY_BYTES) || (gd25q64_read_open != 0U))
    {
        return GD25Q64_STATUS_INVALID_PARAM;
    }

    SPI_SET_SS0_LOW(SPI0);
    status = gd25q64_send(GD25Q64_CMD_READ_DATA);
    if(status == GD25Q64_STATUS_OK)
    {
        status = gd25q64_send((uint8_t)(address >> 16U));
    }
    if(status == GD25Q64_STATUS_OK)
    {
        status = gd25q64_send((uint8_t)(address >> 8U));
    }
    if(status == GD25Q64_STATUS_OK)
    {
        status = gd25q64_send((uint8_t)address);
    }

    if(status != GD25Q64_STATUS_OK)
    {
        SPI_SET_SS0_HIGH(SPI0);
        return status;
    }

    gd25q64_read_open = 1U;
    return GD25Q64_STATUS_OK;
}

/**
 ******************************************************************************
  @功能：从当前连续读取位置接收一个字节
  @日期：2026-07-13
  @参数：[输出] data - Flash 返回的数据字节
  @返回值：Gd25q64Status - 状态、参数和 SPI 传输结果
  @使用说明：每次发送 0xFF 产生 8 个时钟；器件内部地址自动加 1。超时会立即释放 CS#。
 ******************************************************************************
 */
Gd25q64Status GD25Q64_ReadByte(uint8_t *data)
{
    Gd25q64Status status;

    if(data == 0)
    {
        return GD25Q64_STATUS_INVALID_PARAM;
    }
    if(gd25q64_read_open == 0U)
    {
        return GD25Q64_STATUS_READ_NOT_OPEN;
    }

    status = gd25q64_transfer(0xFFU, data);
    if(status != GD25Q64_STATUS_OK)
    {
        GD25Q64_EndRead();
    }
    return status;
}

/**
 ******************************************************************************
  @功能：结束 GD25Q64 连续读取事务
  @日期：2026-07-13
  @参数：无
  @返回值：无
  @使用说明：拉高 SPI0 SS0 并清除软件流状态；重复调用是安全的。
 ******************************************************************************
 */
void GD25Q64_EndRead(void)
{
    SPI_SET_SS0_HIGH(SPI0);
    gd25q64_read_open = 0U;
}

/**
 ******************************************************************************
  @功能：一次性读取指定地址范围到调用者缓冲区
  @日期：2026-07-13
  @参数：[输入] address - 起始绝对地址
        [输出] buffer - 接收缓冲区
        [输入] length - 读取字节数
  @返回值：Gd25q64Status - 参数、边界或底层 SPI 错误
  @使用说明：使用减法形式检查 length，避免 address+length 发生无符号溢出。函数为同步读取，
             适合包头和索引小块数据；动画 payload 使用 BeginRead/ReadByte 流式接口。
 ******************************************************************************
 */
Gd25q64Status GD25Q64_Read(uint32_t address, uint8_t *buffer, uint32_t length)
{
    uint32_t index;
    Gd25q64Status status;

    if((buffer == 0) || (address >= GD25Q64_CAPACITY_BYTES) ||
       (length > (GD25Q64_CAPACITY_BYTES - address)))
    {
        return GD25Q64_STATUS_INVALID_PARAM;
    }
    if(length == 0U)
    {
        return GD25Q64_STATUS_OK;
    }

    status = GD25Q64_BeginRead(address);
    if(status != GD25Q64_STATUS_OK)
    {
        return status;
    }

    for(index = 0U; index < length; index++)
    {
        status = GD25Q64_ReadByte(&buffer[index]);
        if(status != GD25Q64_STATUS_OK)
        {
            return status;
        }
    }

    GD25Q64_EndRead();
    return GD25Q64_STATUS_OK;
}
