
#include "../stm32/f1.h"

#undef CORTEX_M3
#define RISCV 1

#undef FLASH_PAGE_SIZE
extern unsigned int FLASH_PAGE_SIZE;

/* On reset, SYSCLK=HSI at 8MHz. SYSCLK runs at 1MHz. */
void early_fatal(int blinks) __attribute__((noreturn));
#define early_delay_ms(ms) (delay_ticks((ms)*1000))
#define early_delay_us(us) (delay_ticks((us)*1))

#undef SYSCLK_MHZ
#define SYSCLK_MHZ 144
#define AHB_MHZ (SYSCLK_MHZ / 1)  /* 144MHz */
#define APB1_MHZ (SYSCLK_MHZ / 2) /* 72MHz */
#define APB2_MHZ (SYSCLK_MHZ / 2) /* 72MHz */

