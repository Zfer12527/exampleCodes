/* STM32F103C8 + TJA1050：PA12 为 CAN 发送脚，PA11 为 CAN 接收脚。 */
#include "stm32f10x.h"       // 引入芯片寄存器定义和标准外设库声明。
#include "stm32f10x_it.h"    // 引入中断相关声明，以及 1 ms 计数变量 g_msTicks。

#define CAN_TX_TIMEOUT_MS 10U     // 等待 CAN 发送结果的超时时间，单位为毫秒。
#define CAN_MESSAGE_ID 0x123U     // 按键发送报文使用的 11 位标准标识符。
#define BUTTON_DEBOUNCE_MS 30U    // 按下、松开都必须保持稳定的消抖时间，单位为毫秒。

/* 以下全局变量供调试器观察；volatile 要求编译器保留对变量的实际访问。 */
volatile uint32_t g_canTxSuccessCount;    // CAN 发送成功次数；全局变量默认初始化为 0。
volatile uint32_t g_canTxFailureCount;    // 无空闲邮箱、发送失败或超时的累计次数。
volatile uint32_t g_canRxCount;           // 从 CAN 接收 FIFO0 中取出的报文总数。
volatile uint32_t g_canRxMatchCount;      // 数据匹配并触发 PC13 翻转的报文数量。
volatile uint32_t g_initFailed;          // 初始化失败标志：0 为未记录失败，1 为失败。

static void Button_Config(void)    // 配置 PA6 按键输入和 PC13 LED 输出，仅本文件可调用。
{    // GPIO 配置函数开始。
    GPIO_InitTypeDef gpio;    // 保存本次 GPIO 初始化所需的引脚、模式和速度。

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOC, ENABLE);    // 开启 GPIOA 和 GPIOC 外设时钟。

    /* 按钮连接 PA6 与 3.3 V；松开时由内部下拉保持低电平。 */
    gpio.GPIO_Pin = GPIO_Pin_6;            // 选择 PA6 引脚。
    gpio.GPIO_Mode = GPIO_Mode_IPD;        // 设置为内部下拉输入，按下按钮读到高电平。
    gpio.GPIO_Speed = GPIO_Speed_2MHz;     // 填写 GPIO 速度字段；输入模式下不决定采样频率。
    GPIO_Init(GPIOA, &gpio);               // 将上述配置写入 GPIOA 的相关寄存器。

    /* 板载 PC13 LED 低电平点亮，因此先预置高电平，使其初始化后熄灭。 */
    GPIO_SetBits(GPIOC, GPIO_Pin_13);       // 先将 PC13 输出数据位设为 1，避免切换为输出时先拉低。
    gpio.GPIO_Pin = GPIO_Pin_13;           // 将配置目标改为 PC13。
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;     // 设置为推挽输出，速度沿用上面的 2 MHz。
    GPIO_Init(GPIOC, &gpio);               // 应用 PC13 输出配置，初始输出为高电平。
}    // GPIO 配置函数结束。

static void CAN1_ProcessReceive(void)    // 轮询接收 CAN 报文，匹配指定数据后翻转 PC13。
{    // CAN 接收处理函数开始。
    CanRxMsg message;    // 保存从接收 FIFO 中读取的一帧报文。

    /* FIFO0 最多缓存三帧；每次最多处理三帧，给主循环中的按键检测留出执行机会。 */
    for (uint8_t count = 0U;    // 初始化本轮已处理的报文数量。
         count < 3U && CAN_MessagePending(CAN1, CAN_FIFO0) != 0U;    // 数量未到三帧且 FIFO0 非空时继续处理。
         ++count)    // 每处理一帧，循环计数增加 1。
    {    // 本帧报文处理开始。
        uint8_t matches = 1U;    // 先假定数据匹配，遇到任何不同字节时再清零。

        /* 标准库的 CAN_Receive() 读取报文后，会同时释放对应的 FIFO 条目。 */
        CAN_Receive(CAN1, CAN_FIFO0, &message);    // 从 CAN1 的 FIFO0 取出一帧，写入 message。
        ++g_canRxCount;                           // 接收报文总数加 1，包括之后会被忽略的报文。
        if (message.RTR != CAN_RTR_Data || message.DLC != 8U)    // 只接受数据帧，且数据长度必须等于 8 字节。
        {    // 帧类型或长度不符合要求时进入此分支。
            continue;    // 忽略本帧，继续处理 FIFO0 中的下一帧。
        }    // 帧类型、长度检查结束。

        for (uint8_t i = 0U; i < 8U; ++i)    // 从第 0 字节到第 7 字节依次检查全部数据。
        {    // 单字节匹配检查开始。
            if (message.Data[i] != i + 1U)    // 目标值依次为十六进制 01、02、03、04、05、06、07、08。
            {    // 发现当前字节与目标值不同。
                matches = 0U;    // 标记整帧数据不匹配。
                break;           // 只要一个字节不同，就不必继续比较其余字节。
            }    // 当前字节不匹配分支结束。
        }    // 数据逐字节比较结束。

        if (matches != 0U)    // 只有八个字节全部匹配时才执行输出动作，不额外限制报文 ID。
        {    // 匹配报文处理开始。
            ++g_canRxMatchCount;    // 匹配报文计数加 1。
            GPIO_WriteBit(GPIOC, GPIO_Pin_13,    // 准备更新 PC13 的输出电平。
                          (BitAction)!GPIO_ReadOutputDataBit(GPIOC, GPIO_Pin_13));    // 读取输出锁存值后取反，实现每帧翻转一次。
        }    // 匹配报文处理结束。
    }    // 本轮最多三帧的接收循环结束。
}    // CAN 接收处理函数结束。

static void CAN1_SendMessage(void)    // 向 CAN1 提交一帧固定数据，并短暂等待发送结果。
{    // CAN 发送函数开始。
    CanTxMsg message = {0};    // 将发送结构体的全部成员清零，避免使用未初始化的数据。
    uint8_t mailbox;           // 保存 CAN_Transmit() 返回的发送邮箱编号，可能为 0、1、2 或无邮箱标志。
    uint8_t status;            // 保存 CAN_TransmitStatus() 返回的等待、成功或失败状态。
    uint32_t started;          // 保存开始等待发送结果时的系统毫秒计数。

    message.StdId = CAN_MESSAGE_ID;    // 设置标准报文 ID 为 0x123。
    message.IDE = CAN_Id_Standard;     // 使用 11 位标准帧格式，不使用 29 位扩展帧。
    message.RTR = CAN_RTR_Data;        // 使用数据帧，不使用远程帧。
    message.DLC = 8U;                 // 设置数据长度为 8 字节。
    for (uint8_t i = 0U; i < 8U; ++i)    // 依次填充 8 个数据字节。
    {    // 数据填充循环开始。
        message.Data[i] = i + 1U;    // 将报文数据设置为 01 02 03 04 05 06 07 08。
    }    // 数据填充循环结束。

    mailbox = CAN_Transmit(CAN1, &message);    // 将报文写入空闲邮箱并请求发送；返回值不代表发送成功。
    if (mailbox == CAN_TxStatus_NoMailBox)    // 检查三个发送邮箱是否均被占用。
    {    // 无可用发送邮箱时进入此分支。
        ++g_canTxFailureCount;    // 将本次提交失败计入发送失败次数。
        return;                  // 无邮箱可供查询，直接结束本次发送函数。
    }    // 空闲邮箱检查结束。

    started = g_msTicks;    // 记录开始等待发送状态的时刻；g_msTicks 在 SysTick 中断中递增。
    do    // 至少查询一次发送状态；这里存在最多约 10 ms 的轮询等待。
    {    // 发送状态查询循环开始。
        status = CAN_TransmitStatus(CAN1, mailbox);    // 根据刚才的邮箱编号查询硬件发送状态。
    } while (status == CAN_TxStatus_Pending &&    // 只有报文仍在等待发送时才可能继续循环。
             (uint32_t)(g_msTicks - started) < CAN_TX_TIMEOUT_MS);    // 等待时间达到 10 ms 时退出，无符号差值可处理计数回绕。

    if (status == CAN_TxStatus_Ok)    // 判断硬件是否报告本次报文发送成功。
    {    // 发送成功处理开始。
        ++g_canTxSuccessCount;    // 只累加成功次数，不直接改变 PC13。
    }    // 发送成功处理结束。
    else    // 发送失败，或者直到超时时仍未完成发送。
    {    // 发送失败、超时处理开始。
        CAN_CancelTransmit(CAN1, mailbox);    // 请求取消该邮箱的发送；已经完成的传输不会被撤回。
        ++g_canTxFailureCount;               // 将本次发送计入失败次数。
    }    // 发送失败、超时处理结束。
}    // CAN 发送函数结束。

static void Button_Process(void)    // 使用时间戳检测按键，不通过延时函数等待消抖。
{    // 按键处理函数开始。
    static uint8_t lastRawLevel;    // 保存上次采样的原始电平；静态变量默认值为 0，并跨调用保留。
    static uint8_t stableLevel;     // 保存已通过消抖确认的电平，初始视为松开。
    static uint32_t lastChange;     // 保存最近一次原始电平变化的时刻。
    const uint32_t now = g_msTicks;    // 读取当前系统毫秒计数，供本次检测使用。
    const uint8_t rawLevel = GPIO_ReadInputDataBit(GPIOA, GPIO_Pin_6);    // 采样 PA6：1 为按下，0 为松开。

    if (rawLevel != lastRawLevel)    // 原始采样电平发生变化时，重新开始计算稳定时间。
    {    // 原始电平变化处理开始。
        lastRawLevel = rawLevel;    // 更新最近采样到的电平。
        lastChange = now;           // 将此次变化时刻作为消抖计时起点。
        return;                     // 立即返回主循环，下次调用再检查，不在这里延时等待。
    }    // 原始电平变化处理结束。

    /* 按下和松开都需要消抖；消抖等待本身不会阻塞主循环。 */
    if (rawLevel != stableLevel &&    // 仅在采样电平与已确认电平不同的时候检查稳定时间。
        (uint32_t)(now - lastChange) >= BUTTON_DEBOUNCE_MS)    // 两次采样之间未再观察到变化，且距最近变化至少 30 ms。
    {    // 接受新的稳定按键状态。
        stableLevel = rawLevel;    // 更新消抖后的按键状态，避免同一次长按重复触发。
        if (stableLevel != 0U)     // 只在确认按下时触发；确认松开时仅更新状态，为下次按下做准备。
        {    // 有效按下处理开始。
            CAN1_SendMessage();    // 每次有效按下发送一次报文；此发送函数可能等待约 10 ms。
        }    // 有效按下处理结束。
    }    // 新稳定状态处理结束。
}    // 按键处理函数结束。

static uint8_t CAN1_Config(void)    // 配置 CAN1 和接收过滤器；返回 1 表示成功，0 表示失败。
{    // CAN 初始化函数开始。
    GPIO_InitTypeDef gpio;           // 保存 CAN 收发引脚的 GPIO 配置。
    CAN_InitTypeDef can;             // 保存 CAN 工作模式、位时序和控制选项。
    CAN_FilterInitTypeDef filter;    // 保存 CAN 接收过滤器配置。
    RCC_ClocksTypeDef clocks;        // 保存系统各总线的实际配置时钟频率。

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_AFIO, ENABLE);    // 开启 GPIOA 和复用功能模块时钟。
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_CAN1, ENABLE);    // 开启 APB1 总线上的 CAN1 外设时钟。
    GPIO_PinRemapConfig(GPIO_Remap1_CAN1, DISABLE);        // 禁用 CAN1 的此项重映射，使用本工程的默认 PA11/PA12 引脚。

    gpio.GPIO_Pin = GPIO_Pin_11;           // 选择 PA11，作为 CAN_RX 接收引脚。
    gpio.GPIO_Mode = GPIO_Mode_IPU;        // 配置为上拉输入，接收收发器 RXD 输出。
    gpio.GPIO_Speed = GPIO_Speed_50MHz;     // 填写 GPIO 速度字段；接收输入的采样时序由 CAN 外设控制。
    GPIO_Init(GPIOA, &gpio);               // 应用 PA11 接收引脚配置。
    gpio.GPIO_Pin = GPIO_Pin_12;           // 将配置目标改为 PA12，即 CAN_TX 发送引脚。
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;      // 配置为复用推挽输出，由 CAN1 外设控制电平。
    GPIO_Init(GPIOA, &gpio);               // 应用 PA12 配置，输出速度字段沿用上面的 50 MHz。

    /* 工程使用 8 MHz 外部晶振，系统时钟为 72 MHz，APB1 时钟为 36 MHz。 */
    /* 位速率 = 36 MHz / [20 × (1 + 15 + 2)] = 100 kbit/s，采样点约为 88.9%。 */
    RCC_GetClocksFreq(&clocks);    // 根据 RCC 配置获取系统时钟和各总线频率。
    if (clocks.PCLK1_Frequency != 36000000U)    // 当前位时序按 36 MHz 设计，因此先检查 APB1 时钟。
    {    // APB1 时钟与预期不一致。
        return 0U;    // 返回初始化失败，避免按错误波特率参与总线通信。
    }    // CAN 时钟检查结束。

    CAN_DeInit(CAN1);                // 复位 CAN1 外设寄存器，清除之前的配置和状态。
    CAN_StructInit(&can);            // 使用标准库默认值初始化结构体，之后再覆盖本工程需要的字段。
    can.CAN_Mode = CAN_Mode_Normal;  // 设置为正常通信模式，不启用内部自发自收的回环模式。
    can.CAN_ABOM = ENABLE;           // 允许硬件在满足恢复条件后自动退出 bus-off 状态。
    /* 每次按键只尝试发送一次，对端不提供 ACK 时也不自动重复发送。 */
    can.CAN_NART = ENABLE;           // 启用“禁止自动重发”选项；ENABLE 在这里表示关闭自动重发。
    can.CAN_SJW = CAN_SJW_1tq;       // 设置重新同步跳转宽度为 1 个时间份额 tq。
    can.CAN_BS1 = CAN_BS1_15tq;      // 设置位时序第 1 段为 15 tq。
    can.CAN_BS2 = CAN_BS2_2tq;       // 设置位时序第 2 段为 2 tq，另有固定的同步段 1 tq。
    can.CAN_Prescaler = 20U;         // 将 CAN 外设时钟分频 20 倍，得到每个 tq 的时钟。
    if (CAN_Init(CAN1, &can) != CAN_InitStatus_Success)    // 应用 CAN 配置，并检查初始化过程是否成功。
    {    // CAN 硬件初始化失败。
        return 0U;    // 向调用者返回失败标志。
    }    // CAN 初始化结果检查结束。

    /* 过滤器接收所有 ID 的标准帧和扩展帧；帧类型、长度、数据由软件进一步判断。 */
    filter.CAN_FilterNumber = 0U;                      // 使用第 0 个过滤器组。
    filter.CAN_FilterMode = CAN_FilterMode_IdMask;      // 使用标识符掩码模式，而不是标识符列表模式。
    filter.CAN_FilterScale = CAN_FilterScale_32bit;     // 使用 32 位过滤器格式，包含标识符及帧格式等位。
    filter.CAN_FilterIdHigh = 0U;                      // 设置过滤标识符的高 16 位；掩码为 0 时此值不影响匹配。
    filter.CAN_FilterIdLow = 0U;                       // 设置过滤标识符的低 16 位。
    filter.CAN_FilterMaskIdHigh = 0U;                  // 高 16 位掩码全部为 0，表示这些位均不参与匹配限制。
    filter.CAN_FilterMaskIdLow = 0U;                   // 低 16 位掩码也全部为 0，因此接收所有 ID、IDE 和 RTR 组合。
    filter.CAN_FilterFIFOAssignment = CAN_Filter_FIFO0;    // 将通过此过滤器的报文存入接收 FIFO0。
    filter.CAN_FilterActivation = ENABLE;                  // 启用第 0 组过滤器。
    CAN_FilterInit(&filter);                               // 将过滤器参数写入硬件寄存器。
    return 1U;    // CAN1 和过滤器配置完成，返回成功标志。
}    // CAN 初始化函数结束。


int main(void)    // 程序入口，由启动文件完成基础初始化后调用。
{    // 主函数开始。
    SystemCoreClockUpdate();    // 根据当前 RCC 配置更新 SystemCoreClock 变量，不在此函数中重新配置 PLL。
    Button_Config();           // 初始化 PA6 和 PC13，PC13 LED 默认熄灭。

    if (!CAN1_Config() || SysTick_Config(SystemCoreClock / 1000U) != 0U)    // 先配置 CAN1，成功后再设置 SysTick 每 1 ms 中断一次。
    {    // CAN 初始化失败，或 SysTick 重装载值设置失败时进入此分支；|| 具有短路求值特性。
        g_initFailed = 1U;    // 设置初始化失败标志，方便调试器定位问题。
        while (1)            // 初始化失败后停在这里，不进入正常收发流程。
        {    // 故障停留循环开始。
        }    // 故障停留循环结束，持续等待复位或调试处理。
    }    // 初始化失败处理结束。

    while (1)    // 主循环持续运行，轮流处理 CAN 接收和按键状态。
    {    // 正常工作主循环开始。
        CAN1_ProcessReceive();    // 每轮最多处理三帧接收报文，匹配时翻转 PC13。
        Button_Process();         // 检查 PA6 的消抖状态，有效按下时发送一帧 CAN 报文。
    }    // 本轮主循环结束，返回循环起点继续执行。
}    // 主函数结束；正常运行时不会到达此处。
