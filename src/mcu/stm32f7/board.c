/*
 * f7/board.c
 * 
 * Board-specific setup and management.
 * 
 * Written & released by Keir Fraser <keir.xen@gmail.com>
 * 
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

#define gpio_led gpiob
#define pin_led 13

const static struct pin_mapping _user_pins_F7SM_v1[] = {
    {  2, _B, 12, _OD },
    { 10, _B,  1, _OD },
    { 12, _B,  0, _OD },
    { 14, _B, 11, _OD },
    { 16, _B, 10, _OD },
    {  0,  0,  0, _OD } };

const static struct pin_mapping _user_pins_F7SM_ant_goffart_f7_plus_v1[] = {
    {  2, _B, 12, _OD }, /* board bug: B12 isn't buffered */
    {  4, _C,  6, _PP },
    { 10, _B,  1, _PP },
    { 12, _B,  0, _PP },
    { 14, _B, 11, _PP },
    { 16, _B, 10, _PP },
    {  0,  0,  0, _PP } };

const static struct pin_mapping _user_pins_F7SM_lightning[] = {
    {  2, _B, 12, _PP },
    {  4, _E, 15, _PP },
    {  6, _E, 14, _PP },
    { 10, _B,  1, _PP },
    { 12, _B,  0, _PP },
    { 14, _B, 11, _PP },
    { 16, _B, 10, _PP },
    {  0,  0,  0, _PP } };

const static struct pin_mapping _user_pins_F7SM_v2[] = {
    {  2, _B, 12, _OD },
    {  4, _C,  8, _OD },
    {  6, _C,  7, _OD },
    { 10, _B,  1, _OD },
    { 12, _B,  0, _OD },
    { 14, _B, 11, _OD },
    { 16, _B, 10, _OD },
    {  0,  0,  0, _OD } };

const static struct pin_mapping _user_pins_F7SM_ant_goffart_f7_plus_v2[] = {
    {  2, _B, 12, _PP },
    {  4, _C,  8, _PP },
    {  6, _C,  7, _PP },
    { 10, _B,  1, _PP },
    { 12, _B,  0, _PP },
    { 14, _B, 11, _PP },
    { 16, _B, 10, _PP },
    {  0,  0,  0, _PP } };

const static struct pin_mapping _user_pins_F7SM_lightning_plus[] = {
    {  2, _B, 12, _PP },
    {  4, _E, 15, _PP },
    {  6, _E, 14, _PP },
    { 10, _B,  1, _PP },
    { 12, _B,  0, _PP },
    { 14, _B, 11, _PP },
    { 16, _B, 10, _PP },
    {  0,  0,  0, _PP } };

const static struct pin_mapping _user_pins_F7SM_slim[] = {
    { 10, _B,  1, _PP },
    { 14, _B, 11, _PP },
    {  0,  0,  0, _PP } };

const static struct pin_mapping _user_pins_F7SM_v3[] = {
    {  2, _B, 12, _PP },
    {  4, _C,  8, _PP },
    {  6, _C,  7, _PP },
    { 10, _B,  1, _PP },
    { 12, _B,  0, _PP },
    { 14, _B, 11, _PP },
    { 16, _B, 10, _PP },
    {  0,  0,  0, _PP } };

const static struct board_config _board_config[] = {
    [F7SM_v1] = {
        .hse_mhz   = 8,
        .user_pins = _user_pins_F7SM_v1 },
    [F7SM_ant_goffart_f7_plus_v1] = {
        .hse_mhz   = 8,
        .user_pins = _user_pins_F7SM_ant_goffart_f7_plus_v1 },
    [F7SM_lightning] = {
        .hse_mhz   = 16,
        .hs_usb    = TRUE,
        .user_pins = _user_pins_F7SM_lightning },
    [F7SM_v2] = {
        .hse_mhz   = 8,
        .user_pins = _user_pins_F7SM_v2 },
    [F7SM_ant_goffart_f7_plus_v2] = {
        .hse_mhz   = 8,
        .user_pins = _user_pins_F7SM_ant_goffart_f7_plus_v2 },
    [F7SM_lightning_plus] = {
        .hse_mhz   = 16,
        .hse_byp   = TRUE,
        .hs_usb    = TRUE,
        .flippy    = TRUE,
        .user_pins = _user_pins_F7SM_lightning_plus },
    [F7SM_slim] = {
        .hse_mhz   = 16,
        .hse_byp   = TRUE,
        .user_pins = _user_pins_F7SM_slim },
    [F7SM_v3] = {
        .hse_mhz   = 8,
        .flippy    = TRUE,
        .user_pins = _user_pins_F7SM_v3 }
};

/* Blink the activity LED to indicate fatal error. */
void early_fatal(int blinks)
{
    int i;
    rcc->ahb1enr |= RCC_AHB1ENR_GPIOBEN;
    delay_ticks(10);
    gpio_configure_pin(gpio_led, pin_led, GPO_pushpull(IOSPD_LOW, HIGH));
    for (;;) {
        for (i = 0; i < blinks; i++) {
            gpio_write_pin(gpio_led, pin_led, LOW);
            early_delay_ms(150);
            gpio_write_pin(gpio_led, pin_led, HIGH);
            early_delay_ms(150);
        }
        early_delay_ms(2000);
    }
}

void identify_board_config(void)
{
    uint16_t low, high;
    uint8_t id = 0;
    int i;

    rcc->ahb1enr |= RCC_AHB1ENR_GPIOCEN;
    early_delay_us(2);

    /* Pull PC[15:13] low, and check which are tied HIGH. */
    for (i = 0; i < 3; i++)
        gpio_configure_pin(gpioc, 13+i, GPI_pull_down);
    early_delay_us(10);
    high = (gpioc->idr >> 13) & 7;

    /* Pull PC[15:13] high, and check which are tied LOW. */
    for (i = 0; i < 3; i++)
        gpio_configure_pin(gpioc, 13+i, GPI_pull_up);
    early_delay_us(10);
    low = (~gpioc->idr >> 13) & 7;

    /* Each PCx pin defines a 'trit': 0=float, 1=low, 2=high. 
     * We build a 3^3 ID space from the resulting three-trit ID. */
    for (i = 0; i < 3; i++) {
        id *= 3;
        switch ((high>>1&2) | (low>>2&1)) {
        case 0: break;          /* float = 0 */
        case 1: id += 1; break; /* LOW   = 1 */
        case 2: id += 2; break; /* HIGH  = 2 */
        case 3: early_fatal(1); /* cannot be tied HIGH *and* LOW! */
        }
        high <<= 1;
        low <<= 1;
    }

    /* Panic if the ID is unrecognised. */
    if (id >= ARRAY_SIZE(_board_config))
        early_fatal(2);

    gw_info.hw_submodel = id;
    board_config = &_board_config[id];

    gw_info.mcu_mhz = SYSCLK_MHZ;
    gw_info.mcu_sram_kb = 256;
}

static void mcu_board_init(void)
{
    uint16_t pu[] = {
        [_A] = 0x9930, /* PA4-5,8,11-12,15 */
        [_B] = 0x2ffb, /* PB0-1,3-11,13 */
        [_C] = 0xffe7, /* PC0-2,5-15 */
        [_D] = 0xffff, /* PD0-15 */
        [_E] = 0xffff, /* PE0-15 */
        [_F] = 0xffff, /* PF0-15 */
        [_G] = 0xffff, /* PG0-15 */
        [_H] = 0xffff, /* PH0-15 */
        [_I] = 0xffff, /* PI0-15 */
    };
    uint32_t ahb1enr = rcc->ahb1enr;
    const struct pin_mapping *upin;

    /* Enable all GPIO bank register clocks to configure unused pins. */
    rcc->ahb1enr |= (RCC_AHB1ENR_GPIOAEN |
                     RCC_AHB1ENR_GPIOBEN |
                     RCC_AHB1ENR_GPIOCEN |
                     RCC_AHB1ENR_GPIODEN |
                     RCC_AHB1ENR_GPIOEEN |
                     RCC_AHB1ENR_GPIOFEN |
                     RCC_AHB1ENR_GPIOGEN |
                     RCC_AHB1ENR_GPIOHEN |
                     RCC_AHB1ENR_GPIOIEN);
    peripheral_clock_delay();

    /* Keep clock enabled for all banks containing user-modifiable pins. 
     * Also do not default these pins to pull-up mode. */
    for (upin = board_config->user_pins; upin->pin_id != 0; upin++) {
        ahb1enr |= 1u << upin->gpio_bank;
        pu[upin->gpio_bank] &= ~(1u << upin->gpio_pin);
    }

    /* Flippy TRK0_DISABLE output: Set inactive (LOW). */
    if (board_config->flippy) {
        gpio_configure_pin(gpioc, 1, GPO_pushpull(IOSPD_LOW, LOW));
        pu[_C] &= ~(1u << 1); /* PC1 */
    }

    switch (gw_info.hw_submodel) {

    case F7SM_slim:
        /* Extra pins should float in case they are inputs (drive->GW). */
        pu[_B] &= ~((1u << 0) | (1u << 12)); /* PB0, PB12 */
        pu[_C] &= ~((1u << 7) | ~(1u << 8)); /* PC7, PC8 */
        break;

    case F7SM_v3:
    case F7SM_lightning_plus:
        /* Floppy pin 34 input line is externally pulled up. */
        pu[_C] &= ~(1u << 2); /* PC2 */
        break;

    }

    gpio_pull_up_pins(gpioa, pu[_A]);
    gpio_pull_up_pins(gpiob, pu[_B]);
    gpio_pull_up_pins(gpioc, pu[_C]);
    gpio_pull_up_pins(gpiod, pu[_D]);
    gpio_pull_up_pins(gpioe, pu[_E]);
    gpio_pull_up_pins(gpiof, pu[_F]);
    gpio_pull_up_pins(gpiog, pu[_G]);
    gpio_pull_up_pins(gpioh, pu[_H]);
    gpio_pull_up_pins(gpioi, pu[_I]);

    /* Unused GPIO banks can have their clocks disabled again. They will 
     * statically hold their configuration state. */
    peripheral_clock_delay();
    rcc->ahb1enr = ahb1enr;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "Linux"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
