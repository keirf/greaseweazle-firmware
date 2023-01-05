
#include "../stm32/f1_regs.h"

#define RCC_CFGR_PLLRANGE_GT72MHZ (1u<<31)
#define RCC_CFGR_USBPSC_3  ((uint32_t)0x08400000)
#define RCC_CFGR_HSE_PREDIV2 (1u<<17)
#define RCC_CFGR_APB2PSC_2 (4u<<11)
#define RCC_CFGR_APB1PSC_2 (4u<< 8)

#define RCC_PLL (&rcc->cfgr2)
#define RCC_PLL_PLLCFGEN  (1u<<31)
#define RCC_PLL_FREF_MASK (7u<<24)
#define RCC_PLL_FREF_8M   (2u<<24)

#define RCC_APB2ENR_ACCEN (1u<<22)

static volatile uint32_t * const RCC_MISC = (uint32_t *)(RCC_BASE + 0x30);
#define RCC_MISC_HSI_NODIV    (1u<<25)

static volatile uint32_t * const RCC_MISC2 = (uint32_t *)(RCC_BASE + 0x54);
#define RCC_MISC2_HSI_FOR_USB (1u<< 8)
#define RCC_MISC2_AUTOSTEP_EN (3u<< 4)

#define TIM_CR1_PMEN (1u<<10)

/* HSI Auto Clock Calibration */
struct acc {
    uint32_t sts;    /* 00: Status */
    uint32_t ctrl1;  /* 04: Control #1 */
    uint32_t ctrl2;  /* 08: Control #2 */
    uint32_t c1;     /* 0C: Compare value #1 */
    uint32_t c2;     /* 10: Compare value #2 */
    uint32_t c3;     /* 14: Compare value #3 */
};

#define ACC_CTRL1_ENTRIM (1u<<1)
#define ACC_CTRL1_CALON  (1u<<0)

#define ACC_BASE 0x40015800
