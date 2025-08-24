#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "util.h"

#define ONE_MEGAHERTZ_HZ (1000*1000)
#define SYSTEM_CLOCK_HZ (SYSTEM_CLOCK_MHZ*ONE_MEGAHERTZ_HZ)

//Stuff copied from STM32 "LL" HAL
#define LL_GPIO_MODE_INPUT                 (0x00000000U) /*!< Select input mode */
#define LL_GPIO_MODE_OUTPUT                GPIO_MODER_MODE0_0  /*!< Select output mode */
#define LL_GPIO_MODE_ALTERNATE             GPIO_MODER_MODE0_1  /*!< Select alternate function mode */

#define LL_GPIO_PULL_NO                    (0x00000000U) /*!< Select I/O no pull */
#define LL_GPIO_PULL_UP                    GPIO_PUPDR_PUPD0_0 /*!< Select I/O pull up */
#define LL_GPIO_PULL_DOWN                  GPIO_PUPDR_PUPD0_1 /*!< Select I/O pull down */

#define LL_GPIO_OUTPUT_PUSHPULL            (0x00000000U) /*!< Select push-pull as output type */
#define LL_GPIO_OUTPUT_OPENDRAIN           GPIO_OTYPER_OT0 /*!< Select open-drain as output type */

#define LL_GPIO_SPEED_FREQ_HIGH            GPIO_OSPEEDR_OSPEED0_1 /*!< Select I/O fast output speed   */

#define LL_USART_DATAWIDTH_7B                   USART_CR1_M1            /*!< 7 bits word length : Start bit, 7 data bits, n stop bits */
#define LL_USART_DATAWIDTH_8B                   0x00000000U             /*!< 8 bits word length : Start bit, 8 data bits, n stop bits */
#define LL_USART_DATAWIDTH_9B                   USART_CR1_M0            /*!< 9 bits word length : Start bit, 9 data bits, n stop bits */

/**
  * @brief  Compute USARTDIV value according to Peripheral Clock and
  *         expected Baud Rate in 16 bits sampling mode (32 bits value of USARTDIV is returned)
  * @param  __PERIPHCLK__ Peripheral Clock frequency used for USART instance
  * @param  __PRESCALER__ This parameter can be one of the following values:
  *         @arg @ref LL_USART_PRESCALER_DIV1
  *         @arg @ref LL_USART_PRESCALER_DIV2
  *         @arg @ref LL_USART_PRESCALER_DIV4
  *         @arg @ref LL_USART_PRESCALER_DIV6
  *         @arg @ref LL_USART_PRESCALER_DIV8
  *         @arg @ref LL_USART_PRESCALER_DIV10
  *         @arg @ref LL_USART_PRESCALER_DIV12
  *         @arg @ref LL_USART_PRESCALER_DIV16
  *         @arg @ref LL_USART_PRESCALER_DIV32
  *         @arg @ref LL_USART_PRESCALER_DIV64
  *         @arg @ref LL_USART_PRESCALER_DIV128
  *         @arg @ref LL_USART_PRESCALER_DIV256
  * @param  __BAUDRATE__ Baud rate value to achieve
  * @retval USARTDIV value to be used for BRR register filling in OverSampling_16 case
  */
#define __LL_USART_DIV_SAMPLING16(__PERIPHCLK__, __PRESCALER__, __BAUDRATE__) \
  ((((__PERIPHCLK__)/(USART_PRESCALER_TAB[(__PRESCALER__)]))\
    + ((__BAUDRATE__)/2U))/(__BAUDRATE__))


/** @defgroup USART_LL_EC_DIRECTION Communication Direction
  * @{
  */
#define LL_USART_DIRECTION_NONE                 0x00000000U                        /*!< Transmitter and Receiver are disabled */
#define LL_USART_DIRECTION_RX                   USART_CR1_RE                       /*!< Transmitter is disabled and Receiver is enabled */
#define LL_USART_DIRECTION_TX                   USART_CR1_TE                       /*!< Transmitter is enabled and Receiver is disabled */
#define LL_USART_DIRECTION_TX_RX                (USART_CR1_TE |USART_CR1_RE)       /*!< Transmitter and Receiver are enabled */

/** @defgroup USART_LL_EC_HWCONTROL Hardware Control
  * @{
  */
#define LL_USART_HWCONTROL_NONE                 0x00000000U                          /*!< CTS and RTS hardware flow control disabled */
#define LL_USART_HWCONTROL_RTS                  USART_CR3_RTSE                       /*!< RTS output enabled, data is only requested when there is space in the receive buffer */
#define LL_USART_HWCONTROL_CTS                  USART_CR3_CTSE                       /*!< CTS mode enabled, data is only transmitted when the nCTS input is asserted (tied to 0) */
#define LL_USART_HWCONTROL_RTS_CTS              (USART_CR3_RTSE | USART_CR3_CTSE)    /*!< CTS and RTS hardware flow control enabled */

/** @defgroup USART_LL_EC_OVERSAMPLING Oversampling
  * @{
  */
#define LL_USART_OVERSAMPLING_16                0x00000000U            /*!< Oversampling by 16 */
#define LL_USART_OVERSAMPLING_8                 USART_CR1_OVER8        /*!< Oversampling by 8 */

/** @defgroup USART_LL_EC_PARITY Parity Control
  * @{
  */
#define LL_USART_PARITY_NONE                    0x00000000U                          /*!< Parity control disabled */
#define LL_USART_PARITY_EVEN                    USART_CR1_PCE                        /*!< Parity control enabled and Even Parity is selected */
#define LL_USART_PARITY_ODD                     (USART_CR1_PCE | USART_CR1_PS)       /*!< Parity control enabled and Odd Parity is selected */

/** @defgroup USART_LL_EC_PRESCALER Clock Source Prescaler
  * @{
  */
#define LL_USART_PRESCALER_DIV1                 0x00000000U                                                                   /*!< Input clock not divided   */
#define LL_USART_PRESCALER_DIV2                 (USART_PRESC_PRESCALER_0)                                                     /*!< Input clock divided by 2  */
#define LL_USART_PRESCALER_DIV4                 (USART_PRESC_PRESCALER_1)                                                     /*!< Input clock divided by 4  */
#define LL_USART_PRESCALER_DIV6                 (USART_PRESC_PRESCALER_1 | USART_PRESC_PRESCALER_0)                           /*!< Input clock divided by 6  */
#define LL_USART_PRESCALER_DIV8                 (USART_PRESC_PRESCALER_2)                                                     /*!< Input clock divided by 8  */
#define LL_USART_PRESCALER_DIV10                (USART_PRESC_PRESCALER_2 | USART_PRESC_PRESCALER_0)                           /*!< Input clock divided by 10 */
#define LL_USART_PRESCALER_DIV12                (USART_PRESC_PRESCALER_2 | USART_PRESC_PRESCALER_1)                           /*!< Input clock divided by 12 */
#define LL_USART_PRESCALER_DIV16                (USART_PRESC_PRESCALER_2 | USART_PRESC_PRESCALER_1 | USART_PRESC_PRESCALER_0) /*!< Input clock divided by 16 */
#define LL_USART_PRESCALER_DIV32                (USART_PRESC_PRESCALER_3)                                                     /*!< Input clock divided by 32 */
#define LL_USART_PRESCALER_DIV64                (USART_PRESC_PRESCALER_3 | USART_PRESC_PRESCALER_0)                           /*!< Input clock divided by 64 */
#define LL_USART_PRESCALER_DIV128               (USART_PRESC_PRESCALER_3 | USART_PRESC_PRESCALER_1)                           /*!< Input clock divided by 128 */
#define LL_USART_PRESCALER_DIV256               (USART_PRESC_PRESCALER_3 | USART_PRESC_PRESCALER_1 | USART_PRESC_PRESCALER_0) /*!< Input clock divided by 256 */

/** @defgroup USART_LL_EC_STOPBITS Stop Bits
  * @{
  */
#define LL_USART_STOPBITS_0_5                   USART_CR2_STOP_0                           /*!< 0.5 stop bit */
#define LL_USART_STOPBITS_1                     0x00000000U                                /*!< 1 stop bit */
#define LL_USART_STOPBITS_1_5                   (USART_CR2_STOP_0 | USART_CR2_STOP_1)      /*!< 1.5 stop bits */
#define LL_USART_STOPBITS_2                     USART_CR2_STOP_1                           /*!< 2 stop bits */

/** @defgroup USART_LL_Private_Variables USART Private Variables
  * @{
  */
/* Array used to get the USART prescaler division decimal values versus @ref USART_LL_EC_PRESCALER values */
static const uint32_t USART_PRESCALER_TAB[] =
{
  1UL,
  2UL,
  4UL,
  6UL,
  8UL,
  10UL,
  12UL,
  16UL,
  32UL,
  64UL,
  128UL,
  256UL
};


/**
  * @brief  Perform basic configuration of USART for enabling use in Asynchronous Mode (UART)
  * @note   In UART mode, the following bits must be kept cleared:
  *           - LINEN bit in the USART_CR2 register,
  *           - CLKEN bit in the USART_CR2 register,
  *           - SCEN bit in the USART_CR3 register,
  *           - IREN bit in the USART_CR3 register,
  *           - HDSEL bit in the USART_CR3 register.
  * @note   Call of this function is equivalent to following function call sequence :
  *         - Clear LINEN in CR2 using @ref LL_USART_DisableLIN() function
  *         - Clear CLKEN in CR2 using @ref LL_USART_DisableSCLKOutput() function
  *         - Clear SCEN in CR3 using @ref LL_USART_DisableSmartcard() function
  *         - Clear IREN in CR3 using @ref LL_USART_DisableIrda() function
  *         - Clear HDSEL in CR3 using @ref LL_USART_DisableHalfDuplex() function
  * @note   Other remaining configurations items related to Asynchronous Mode
  *         (as Baud Rate, Word length, Parity, ...) should be set using
  *         dedicated functions
  * @rmtoll CR2          LINEN         LL_USART_ConfigAsyncMode\n
  *         CR2          CLKEN         LL_USART_ConfigAsyncMode\n
  *         CR3          SCEN          LL_USART_ConfigAsyncMode\n
  *         CR3          IREN          LL_USART_ConfigAsyncMode\n
  *         CR3          HDSEL         LL_USART_ConfigAsyncMode
  * @param  USARTx USART Instance
  * @retval None
  */
static void LL_USART_ConfigAsyncMode(USART_TypeDef *USARTx)
{
  /* In Asynchronous mode, the following bits must be kept cleared:
  - LINEN, CLKEN bits in the USART_CR2 register,
  - SCEN, IREN and HDSEL bits in the USART_CR3 register.
  */
  CLEAR_BIT(USARTx->CR2, (USART_CR2_LINEN | USART_CR2_CLKEN));
  CLEAR_BIT(USARTx->CR3, (USART_CR3_SCEN | USART_CR3_IREN | USART_CR3_HDSEL));
}

/**
  * @brief  Check if the USART Receive Enable Acknowledge Flag is set or not
  * @rmtoll ISR          REACK         LL_USART_IsActiveFlag_REACK
  * @param  USARTx USART Instance
  * @retval State of bit (1 or 0).
  */
static uint32_t LL_USART_IsActiveFlag_REACK(const USART_TypeDef *USARTx)
{
  return ((READ_BIT(USARTx->ISR, USART_ISR_REACK) == (USART_ISR_REACK)) ? 1UL : 0UL);
}

/**
  * @brief  Check if the USART Transmit Enable Acknowledge Flag is set or not
  * @rmtoll ISR          TEACK         LL_USART_IsActiveFlag_TEACK
  * @param  USARTx USART Instance
  * @retval State of bit (1 or 0).
  */
static uint32_t LL_USART_IsActiveFlag_TEACK(const USART_TypeDef *USARTx)
{
  return ((READ_BIT(USARTx->ISR, USART_ISR_TEACK) == (USART_ISR_TEACK)) ? 1UL : 0UL);
}

/**
  * @brief  Check if the USART Transmission Complete Flag is set or not
  * @rmtoll ISR          TC            LL_USART_IsActiveFlag_TC
  * @param  USARTx USART Instance
  * @retval State of bit (1 or 0).
  */
__STATIC_INLINE uint32_t LL_USART_IsActiveFlag_TC(const USART_TypeDef *USARTx)
{
  return ((READ_BIT(USARTx->ISR, USART_ISR_TC) == (USART_ISR_TC)) ? 1UL : 0UL);
}

/**
  * @brief  Check if the USART Transmit Data Register Empty or USART TX FIFO Not Full Flag is set or not
  * @note   Macro IS_UART_FIFO_INSTANCE(USARTx) can be used to check whether or not
  *         FIFO mode feature is supported by the USARTx instance.
  * @rmtoll ISR          TXE_TXFNF     LL_USART_IsActiveFlag_TXE_TXFNF
  * @param  USARTx USART Instance
  * @retval State of bit (1 or 0).
  */
static uint32_t LL_USART_IsActiveFlag_TXE_TXFNF(const USART_TypeDef *USARTx)
{
  return ((READ_BIT(USARTx->ISR, USART_ISR_TXE_TXFNF) == (USART_ISR_TXE_TXFNF)) ? 1UL : 0UL);
}


static void LL_GPIO_SetPinMode(GPIO_TypeDef *GPIOx, uint32_t Pin, uint32_t Mode){
  MODIFY_REG(GPIOx->MODER, (GPIO_MODER_MODE0 << (POSITION_VAL(Pin) * 2U)), (Mode << (POSITION_VAL(Pin) * 2U)));
}

static void LL_GPIO_SetPinOutputType(GPIO_TypeDef *GPIOx, uint32_t PinMask, uint32_t OutputType){
  MODIFY_REG(GPIOx->OTYPER, PinMask, (PinMask * OutputType));
}

static void LL_GPIO_SetPinSpeed(GPIO_TypeDef *GPIOx, uint32_t Pin, uint32_t  Speed){
  MODIFY_REG(GPIOx->OSPEEDR, (GPIO_OSPEEDR_OSPEED0 << (POSITION_VAL(Pin) * 2U)),
             (Speed << (POSITION_VAL(Pin) * 2U)));
}

static void LL_GPIO_SetPinPull(GPIO_TypeDef *GPIOx, uint32_t Pin, uint32_t Pull){
  MODIFY_REG(GPIOx->PUPDR, (GPIO_PUPDR_PUPD0 << (POSITION_VAL(Pin) * 2U)), (Pull << (POSITION_VAL(Pin) * 2U)));
}

static void LL_GPIO_SetAFPin_8_15(GPIO_TypeDef *GPIOx, uint32_t Pin, uint32_t Alternate){
  MODIFY_REG(GPIOx->AFR[1], (GPIO_AFRH_AFSEL8 << (POSITION_VAL(Pin >> 8U) * 4U)),
             (Alternate << (POSITION_VAL(Pin >> 8U) * 4U)));
}

static uint32_t LL_GPIO_IsInputPinSet(GPIO_TypeDef *GPIOx, uint32_t PinMask){
  return ((READ_BIT(GPIOx->IDR, PinMask) == (PinMask)) ? 1UL : 0UL);
}

//custom helpers on top of STM32 LL
static void set_gpio_as_uart(){
  LL_GPIO_SetPinSpeed(NUCLEO_UART_GPIO, RXD_PIN, LL_GPIO_SPEED_FREQ_HIGH);
  LL_GPIO_SetPinPull(NUCLEO_UART_GPIO, RXD_PIN, LL_GPIO_PULL_UP);
  LL_GPIO_SetAFPin_8_15(NUCLEO_UART_GPIO, RXD_PIN, NUCLEO_UART_GPIO_AF);
  LL_GPIO_SetPinMode(NUCLEO_UART_GPIO, RXD_PIN, LL_GPIO_MODE_ALTERNATE);
  
  LL_GPIO_SetPinSpeed(NUCLEO_UART_GPIO, TXD_PIN, LL_GPIO_SPEED_FREQ_HIGH);
  LL_GPIO_SetPinOutputType(NUCLEO_UART_GPIO, TXD_PIN, LL_GPIO_OUTPUT_PUSHPULL);
  LL_GPIO_SetPinPull(NUCLEO_UART_GPIO, TXD_PIN, LL_GPIO_PULL_UP);
  LL_GPIO_SetAFPin_8_15(NUCLEO_UART_GPIO, TXD_PIN, NUCLEO_UART_GPIO_AF);
  LL_GPIO_SetPinMode(NUCLEO_UART_GPIO, TXD_PIN, LL_GPIO_MODE_ALTERNATE);
} 

static void set_uart_config(){
  /*---------------------------- USART CR1 Configuration ---------------------*/
  MODIFY_REG(NUCLEO_USART->CR1,
              (USART_CR1_M | USART_CR1_PCE | USART_CR1_PS |
              USART_CR1_TE | USART_CR1_RE | USART_CR1_OVER8),
              (LL_USART_DATAWIDTH_8B | LL_USART_PARITY_NONE |
              LL_USART_DIRECTION_TX_RX | LL_USART_OVERSAMPLING_16));
  MODIFY_REG(NUCLEO_USART->CR2, USART_CR2_STOP, LL_USART_STOPBITS_1);
  MODIFY_REG(NUCLEO_USART->CR3, USART_CR3_RTSE | USART_CR3_CTSE, LL_USART_HWCONTROL_NONE);
  uint32_t periphclkHz = SYSTEM_CLOCK_HZ;
  WRITE_REG(NUCLEO_USART->BRR,(uint16_t)(__LL_USART_DIV_SAMPLING16(periphclkHz, (uint8_t)LL_USART_PRESCALER_DIV1, 115200)));
  MODIFY_REG(NUCLEO_USART->PRESC, USART_PRESC_PRESCALER, (uint16_t)LL_USART_PRESCALER_DIV1);
  CLEAR_BIT(NUCLEO_USART->CR1, USART_CR1_FIFOEN);//disable FIFO
  LL_USART_ConfigAsyncMode(NUCLEO_USART);
  SET_BIT(NUCLEO_USART->CR1, USART_CR1_UE);//enable UART
  /* Polling USART initialisation */
  while ((!(LL_USART_IsActiveFlag_TEACK(NUCLEO_USART))) || (!(LL_USART_IsActiveFlag_REACK(NUCLEO_USART))));
}


//Our application level HAL
#define USER_BUTTON_Pin GPIO_BSRR_BS13 /*!< Select pin 13 */
#define USER_BUTTON_GPIO_Port GPIOC
#define USER_BUTTON_EXTI_IRQn EXTI13_IRQn
#define LED1_Pin GPIO_BSRR_BS7 /*!< Select pin 7 */
#define LED1_GPIO_Port GPIOC

void led1(bool on){
  if(on) WRITE_REG(LED1_GPIO_Port->BSRR, LED1_Pin);
  else WRITE_REG(LED1_GPIO_Port->BRR, LED1_Pin);
}

bool button(){
  return LL_GPIO_IsInputPinSet(USER_BUTTON_GPIO_Port, USER_BUTTON_Pin);
}

void com_tx(const void *const buf, unsigned int size){
  const uint8_t*const buf8 = (const uint8_t*const)buf;
  for(unsigned int i=0;i<size;i++){
    //wait tx buffer empty
    while (!LL_USART_IsActiveFlag_TXE_TXFNF(NUCLEO_USART));

    //if last char to be sent, clear TC flag
    if (i == (size - 1)){
      WRITE_REG(NUCLEO_USART->ICR, USART_ICR_TCCF);
    }

    //send the byte
    WRITE_REG(NUCLEO_USART->TDR, buf8[i]);
  }

  //wait for completion of the transmission of the last byte (TC flag=1)
  while (!LL_USART_IsActiveFlag_TC(NUCLEO_USART));
}

void com_rx(void *const buf, unsigned int size){
  uint8_t*const buf8 = (uint8_t*const)buf;
  for(unsigned int i=0;i<size;i++){
    //wait reception of a byte
    while(!READ_BIT(NUCLEO_USART->ISR,USART_ISR_RXNE_RXFNE));
    //store it, don't bother with com errors
    buf8[i] = READ_REG(NUCLEO_USART->RDR);
  }
}

static void delay_250us(){//TODO: place in RAM to guarantee 1 cycle/inst
  const uint32_t inst_ns = 1000 / SYSTEM_CLOCK_MHZ;
  const uint32_t nloops = 250000/(inst_ns*4);
  for(unsigned int i=0;i<nloops;i++){
    __NOP();
  }
}
void delay_ms(unsigned int ms){
  for(unsigned int i=0;i<ms;i++){
    delay_250us();
    delay_250us();
    delay_250us();
    delay_250us();
  }
}

#include "type.h"
extern data_flash_t nvm __attribute__ ((section (".data_flash")));
