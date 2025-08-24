#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "util.h"
#define STM32U5A5xx
#include <stm32u5xx.h>

//defines needed by nucleo-common.h
#define NUCLEO_USART USART1
#define SYSTEM_CLOCK_MHZ 4

#define TXD_PIN                      GPIO_BSRR_BS9 /*!< Select pin 9 */
#define RXD_PIN                      GPIO_BSRR_BS10 /*!< Select pin 10 */
#define GPIO_AF7_USART1        ((uint8_t)0x07)  /* USART1 Alternate Function mapping                         */
#define NUCLEO_UART_GPIO GPIOA
#define NUCLEO_UART_GPIO_AF GPIO_AF7_USART1

#include "nucleo-common.h"

//STM32 "LL" HAL
static void LL_AHB2_GRP1_EnableClock(uint32_t Periphs){
  __IO uint32_t tmpreg;
  SET_BIT(RCC->AHB2ENR1, Periphs);
  /* Delay after an RCC peripheral clock enabling */
  tmpreg = READ_BIT(RCC->AHB2ENR1, Periphs);
  (void)tmpreg;
}

/**
  * @brief  Enable APB2 peripherals clock.
  * @rmtoll APB2ENR      TIM1EN        LL_APB2_GRP1_EnableClock\n
  *         APB2ENR      SPI1EN        LL_APB2_GRP1_EnableClock\n
  *         APB2ENR      TIM8EN        LL_APB2_GRP1_EnableClock\n
  *         APB2ENR      USART1EN      LL_APB2_GRP1_EnableClock\n
  *         APB2ENR      TIM15EN       LL_APB2_GRP1_EnableClock\n
  *         APB2ENR      TIM16EN       LL_APB2_GRP1_EnableClock\n
  *         APB2ENR      TIM17EN       LL_APB2_GRP1_EnableClock\n
  *         APB2ENR      SAI1EN        LL_APB2_GRP1_EnableClock\n
  *         APB2ENR      SAI2EN        LL_APB2_GRP1_EnableClock\n
  *         APB2ENR      USBEN         LL_APB2_GRP1_EnableClock\n
  *         APB2ENR      GFXTIMEN      LL_APB2_GRP1_EnableClock\n
  *         APB2ENR      LTDCEN        LL_APB2_GRP1_EnableClock\n
  *         APB2ENR      DSIHOSTEN     LL_APB2_GRP1_EnableClock\n
  * @param  Periphs This parameter can be a combination of the following values:
  *         @arg @ref LL_APB2_GRP1_PERIPH_ALL
  *         @arg @ref LL_APB2_GRP1_PERIPH_TIM1
  *         @arg @ref LL_APB2_GRP1_PERIPH_SPI1
  *         @arg @ref LL_APB2_GRP1_PERIPH_TIM8
  *         @arg @ref LL_APB2_GRP1_PERIPH_USART1
  *         @arg @ref LL_APB2_GRP1_PERIPH_TIM15
  *         @arg @ref LL_APB2_GRP1_PERIPH_TIM16
  *         @arg @ref LL_APB2_GRP1_PERIPH_TIM17
  *         @arg @ref LL_APB2_GRP1_PERIPH_SAI1
  *         @arg @ref LL_APB2_GRP1_PERIPH_SAI2 (*)
  *         @arg @ref LL_APB2_GRP1_PERIPH_USB_FS (*)
  *         @arg @ref LL_APB2_GRP1_PERIPH_GFXTIM (*)
  *         @arg @ref LL_APB2_GRP1_PERIPH_LTDC (*)
  *         @arg @ref LL_APB2_GRP1_PERIPH_DSI (*)
  *
  *        (*) value not defined in all devices.
  * @retval None
  */
static void LL_APB2_GRP1_EnableClock(uint32_t Periphs)
{
  __IO uint32_t tmpreg;
  SET_BIT(RCC->APB2ENR, Periphs);
  /* Delay after an RCC peripheral clock enabling */
  tmpreg = READ_BIT(RCC->APB2ENR, Periphs);
  (void)tmpreg;
}

/* Offset used to access to RCC_CCIPR1 and RCC_CCIPR2 registers */
#define RCC_OFFSET_CCIPR1       0U
#define RCC_OFFSET_CCIPR2       0x04U
#define RCC_OFFSET_CCIPR3       0x08U

/** @defgroup RCC_LL_EC_USART_CLKSOURCE  Peripheral USARTx clock source selection
  * @{
  */
#define LL_RCC_USART1_CLKSOURCE_PCLK2       ((RCC_OFFSET_CCIPR1 << 24U)| (RCC_CCIPR1_USART1SEL_Pos << 16U))  /*!< PCLK2 clock used as USART1 clock source */
#define LL_RCC_USART1_CLKSOURCE_SYSCLK      ((RCC_OFFSET_CCIPR1 << 24U) |(RCC_CCIPR1_USART1SEL_Pos << 16U) | (RCC_CCIPR1_USART1SEL_0 >> RCC_CCIPR1_USART1SEL_Pos)) /*!< SYSCLK clock used as USART1 clock source */
#define LL_RCC_USART1_CLKSOURCE_HSI         ((RCC_OFFSET_CCIPR1 << 24U) |(RCC_CCIPR1_USART1SEL_Pos << 16U) | (RCC_CCIPR1_USART1SEL_1 >> RCC_CCIPR1_USART1SEL_Pos)) /*!< HSI clock used as USART1 clock source */
#define LL_RCC_USART1_CLKSOURCE_LSE         ((RCC_OFFSET_CCIPR1 << 24U) |(RCC_CCIPR1_USART1SEL_Pos << 16U) | (RCC_CCIPR1_USART1SEL >> RCC_CCIPR1_USART1SEL_Pos))   /*!< LSE clock used as USART1 clock source */

/**
  * @brief  Configure USARTx clock source
  * @rmtoll CCIPR1       USART1SEL     LL_RCC_SetUSARTClockSource\n
  *         CCIPR1       USART2SEL     LL_RCC_SetUSARTClockSource\n
  *         CCIPR1       USART3SEL     LL_RCC_SetUSARTClockSource\n
  *         CCIPR2       USART6SEL     LL_RCC_SetUSARTClockSource
  * @param  USARTxSource This parameter can be one of the following values:
  *         @arg @ref LL_RCC_USART1_CLKSOURCE_PCLK2
  *         @arg @ref LL_RCC_USART1_CLKSOURCE_SYSCLK
  *         @arg @ref LL_RCC_USART1_CLKSOURCE_HSI
  *         @arg @ref LL_RCC_USART1_CLKSOURCE_LSE
  *         @arg @ref LL_RCC_USART2_CLKSOURCE_PCLK1 (*)
  *         @arg @ref LL_RCC_USART2_CLKSOURCE_SYSCLK (*)
  *         @arg @ref LL_RCC_USART2_CLKSOURCE_HSI (*)
  *         @arg @ref LL_RCC_USART2_CLKSOURCE_LSE (*)
  *         @arg @ref LL_RCC_USART3_CLKSOURCE_PCLK1
  *         @arg @ref LL_RCC_USART3_CLKSOURCE_SYSCLK
  *         @arg @ref LL_RCC_USART3_CLKSOURCE_HSI
  *         @arg @ref LL_RCC_USART3_CLKSOURCE_LSE
  *         @arg @ref LL_RCC_USART6_CLKSOURCE_PCLK1 (*)
  *         @arg @ref LL_RCC_USART6_CLKSOURCE_SYSCLK (*)
  *         @arg @ref LL_RCC_USART6_CLKSOURCE_HSI (*)
  *         @arg @ref LL_RCC_USART6_CLKSOURCE_LSE (*)
  *
  *         (*) Availability depends on devices.

  * @retval None
  */
static void LL_RCC_SetUSARTClockSource(uint32_t USARTxSource)
{
  __IO uint32_t *reg = (__IO uint32_t *)(uint32_t)(RCC_BASE + 0xE0U + (USARTxSource >> 24U));
  MODIFY_REG(*reg, 3UL << ((USARTxSource & 0x001F0000U) >> 16U), ((USARTxSource & 0x000000FFU) << \
                                                                  ((USARTxSource & 0x001F0000U) >> 16U)));
}

void init(){
  //enable clock for GPIOC (LED, button)
  LL_AHB2_GRP1_EnableClock(RCC_AHB2ENR1_GPIOCEN);
  //enable LED pin as output
  LL_GPIO_SetPinMode(LED1_GPIO_Port, LED1_Pin, LL_GPIO_MODE_OUTPUT);
  //enable button pin as input
  LL_GPIO_SetPinMode(USER_BUTTON_GPIO_Port, USER_BUTTON_Pin, LL_GPIO_MODE_INPUT);

  //select clock source for USART1
  LL_RCC_SetUSARTClockSource(LL_RCC_USART1_CLKSOURCE_PCLK2);
  //enable USART1 clock
  LL_APB2_GRP1_EnableClock(RCC_APB2ENR_USART1EN);
  //enable clock for GPIOA (UART1)
  LL_AHB2_GRP1_EnableClock(RCC_AHB2ENR1_GPIOAEN);
  
  set_gpio_as_uart();
  set_uart_config();
}
