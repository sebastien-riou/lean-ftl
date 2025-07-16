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

#define FLASH_WRITE_SIZE 16

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

static void nvm_ll_erase_page(uint64_t*page_addr){
  const uintptr_t bank_addr = (uintptr_t)NVM_BANK_BASE(page_addr);
  const uint32_t bank = (bank_addr - FLASH_BASE_NS)/FLASH_BANK_SIZE;
  const uint32_t page = ((uintptr_t)page_addr - bank_addr)/FLASH_PAGE_SIZE;
  
  //select the bank
  WRITE_BIT(FLASH_NS->NSCR, FLASH_NSCR_BKER,bank);

  //select the page
  MODIFY_REG(FLASH_NS->NSCR, (FLASH_NSCR_PNB | FLASH_NSCR_PER), ((page << FLASH_NSCR_PNB_Pos) | FLASH_NSCR_PER));
  
  //erase the page
  SET_BIT(FLASH_NS->NSCR, FLASH_NSCR_STRT);

  //wait end of operation
  while(READ_BIT(FLASH_NS->NSSR, FLASH_NSSR_BSY));
  WRITE_REG(FLASH_NS->NSCR, 0);//clear NSCR
}

static void nvm_ll_write_unaligned_src(uint64_t*dst, const uint8_t* buf, uint32_t n_words32){
  uint32_t*dst32 = (uint32_t*)dst;
  WRITE_REG(FLASH_NS->NSCR, FLASH_NSCR_PG);
  for(unsigned int i=0;i<n_words32;i+=4){
    uint32_t buf32[4];
    memcpy(buf32,buf,sizeof(buf32));
    buf+=sizeof(buf32);
    dst32[i] = buf32[0];
    dst32[i+1] = buf32[1];
    dst32[i+2] = buf32[2];
    dst32[i+3] = buf32[3];

    //wait end of operation
    while(READ_BIT(FLASH_NS->NSSR, FLASH_NSSR_BSY));
  }
  WRITE_REG(FLASH_NS->NSCR, 0);//clear PG
}

static void nvm_ll_write(uint64_t*dst, const uint64_t*const buf, uint32_t n_words32){
  if((uintptr_t)buf % sizeof(uint32_t)) {
    nvm_ll_write_unaligned_src(dst,(const uint8_t*)buf,n_words32);
  } else {
    uint32_t*dst32 = (uint32_t*)dst;
    const uint32_t*const buf32 = (const uint32_t*const)buf;
    WRITE_REG(FLASH_NS->NSCR, FLASH_NSCR_PG);
    for(unsigned int i=0;i<n_words32;i+=4){
      dst32[i] = buf32[i];
      dst32[i+1] = buf32[i+1];
      dst32[i+2] = buf32[i+2];
      dst32[i+3] = buf32[i+3];

      //wait end of operation
      while(READ_BIT(FLASH_NS->NSSR, FLASH_NSSR_BSY));
    }
    WRITE_REG(FLASH_NS->NSCR, 0);//clear PG
  }
}

static void nvm_ll_write_page(uint64_t*page_addr, const uint64_t*const buf){
  nvm_ll_write(page_addr, buf, NVM_PAGE_SIZE32);
}

static void nvm_write_page(uint64_t*page_addr, const uint64_t*const buf){
  //erase the page
  nvm_ll_erase_page(page_addr);
  //write
  nvm_ll_write_page(page_addr, buf);
}

static void nvm_update_page(uint64_t*nvm_addr, const uint64_t*const buf, uint32_t size){
  //save whole page in RAM
  uint64_t*page_base = NVM_PAGE_BASE(nvm_addr);
  uint64_t page_buffer[SIZE64(FLASH_PAGE_SIZE)];
  nvm_read64(page_base, page_buffer, FLASH_PAGE_SIZE);

  //update the page in RAM
  uintptr_t offset = ((uintptr_t)nvm_addr - (uintptr_t)page_base)/sizeof(uint64_t);
  for(unsigned int i=0;i<size/sizeof(uint64_t);i++){
    page_buffer[offset+i] = buf[i];
  }

  //write the page in NVM
  nvm_write_page(page_base,page_buffer);
}

//return 0 if success
bool nvm_write64(uint64_t*nvm_addr, const uint64_t*const buf, uint32_t size){
  if(0 == size) return 0;
  /* Disable interrupts to avoid any interruption */
  uint32_t primask_bit = __get_PRIMASK();
  __disable_irq();
  //unlock flash write
  WRITE_REG(FLASH->NSKEYR, FLASH_KEY1);
  WRITE_REG(FLASH->NSKEYR, FLASH_KEY2);

  uint64_t*dst = nvm_addr;
  const uint64_t*src = buf;
  uint32_t remaining = size;

  while(remaining){
    uint32_t size_in_page = NVM_SIZE_IN_PAGE(dst);
    if(remaining<size_in_page) size_in_page = remaining;
    nvm_update_page(dst,src,size_in_page);
    dst += size_in_page / sizeof(uint64_t);
    src += size_in_page / sizeof(uint64_t);
    remaining -= size_in_page;
  }

  //lock flash write
  SET_BIT(FLASH->NSCR, FLASH_NSCR_LOCK);

  bool fail = 0;
  for(unsigned int i=0;i<size/sizeof(uint64_t);i++){
    fail |= nvm_addr[i] != buf[i];
  }
  /* Restore interrupts enable bit */
  __set_PRIMASK(primask_bit);

  return fail;
}

uint8_t nvm_erase(void*base_address, unsigned int n_pages){
  if(0 == n_pages) return 0;
  const uintptr_t size = n_pages * FLASH_PAGE_SIZE;
  if(base_address < (void*)&nvm) return 1;
  if(((uintptr_t)base_address + size) > ((uintptr_t)&nvm + sizeof(nvm))) return 2;
  if(0 != ((uintptr_t)base_address % FLASH_PAGE_SIZE)) return 3;
  /* Disable interrupts to avoid any interruption */
  uint32_t primask_bit = __get_PRIMASK();
  __disable_irq();
  //unlock flash write
  WRITE_REG(FLASH->NSKEYR, FLASH_KEY1);
  WRITE_REG(FLASH->NSKEYR, FLASH_KEY2);

  uint64_t*dst64 = (uint64_t*)base_address;
  for(unsigned int i=0;i<n_pages;i++){
    nvm_ll_erase_page(dst64);
    dst64 += SIZE64(FLASH_PAGE_SIZE);
  }

  //lock flash write
  SET_BIT(FLASH->NSCR, FLASH_NSCR_LOCK);
  /* Restore interrupts enable bit */
  __set_PRIMASK(primask_bit);
  return 0;
}

uint8_t nvm_write(void*dst_nvm_addr, const void*const src, uintptr_t size){
  if(0 == size) return 0;
  if(dst_nvm_addr < (void*)&nvm) return 1;
  if(((uintptr_t)dst_nvm_addr + size) > ((uintptr_t)&nvm + sizeof(nvm))) return 2;
  if(0 != ((uintptr_t)dst_nvm_addr % FLASH_WRITE_SIZE)) return 3;
  if(0 != (size % FLASH_WRITE_SIZE)) return 4;
  /* Disable interrupts to avoid any interruption */
  uint32_t primask_bit = __get_PRIMASK();
  __disable_irq();
  //unlock flash write
  WRITE_REG(FLASH->NSKEYR, FLASH_KEY1);
  WRITE_REG(FLASH->NSKEYR, FLASH_KEY2);
  uint64_t*dst_nvm_addr64 = (uint64_t*)dst_nvm_addr;
  uint64_t*src64 = (uint64_t*)src;
  
  nvm_ll_write(dst_nvm_addr64, src64, size / sizeof(uint32_t));

  //lock flash write
  SET_BIT(FLASH->NSCR, FLASH_NSCR_LOCK);

  uint8_t fail = 0;
  if((uint32_t)src % sizeof(uint32_t)){
    fail = memcmp(dst_nvm_addr,src,size) ? 5 : 0;
  } else{
    for(unsigned int i=0;i<size/sizeof(uint64_t);i++){
      fail |= dst_nvm_addr64[i] != src64[i];
    }
    fail = fail ? 6 : 0;
  }
  /* Restore interrupts enable bit */
  __set_PRIMASK(primask_bit);

  return fail;
}

