/*
 * floppy.c
 * 
 * Floppy interface control.
 * 
 * Written & released by Keir Fraser <keir.xen@gmail.com>
 * 
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

#define m(bitnr) (1u<<(bitnr))

#define get_index()   gpio_read_pin(gpio_index, pin_index)
#define get_trk0()    gpio_read_pin(gpio_trk0, pin_trk0)
#define get_wrprot()  gpio_read_pin(gpio_wrprot, pin_wrprot)

#define configure_pin(pin, type) \
    gpio_configure_pin(gpio_##pin, pin_##pin, type)

#define SAMPLE_MHZ 72
#define TIM_PSC (SYSCLK_MHZ / SAMPLE_MHZ)
#define sample_ns(x) (((x) * SAMPLE_MHZ) / 1000)
#define sample_us(x) ((x) * SAMPLE_MHZ)
#define time_from_samples(x) udiv64((uint64_t)(x) * TIME_MHZ, SAMPLE_MHZ)

/* Track and modify states of output pins. */
static struct {
    bool_t dir;
    bool_t step;
    bool_t wgate;
    bool_t head;
} pins;
#define read_pin(pin) pins.pin
#define write_pin(pin, level) ({                                        \
    gpio_write_pin(gpio_##pin, pin_##pin, level ? O_TRUE : O_FALSE);    \
    pins.pin = level; })

static int bus_type = -1;
static int unit_nr = -1;
static struct unit {
    int cyl;
    bool_t initialised;
    bool_t is_flippy;
    bool_t motor;
} unit[3];

static struct gw_delay delay_params;
static const struct gw_delay factory_delay_params = {
    .select_delay = 10,
    .step_delay = 10000,
    .seek_settle = 15,
    .motor_delay = 750,
    .watchdog = 10000,
    .pre_write = 100,
    .post_write = 1000,
    .index_mask = 200
};

extern uint8_t u_buf[];

#if MCU == STM32F1
#include "mcu/stm32f1/floppy.c"
#elif MCU == STM32F7
#include "mcu/stm32f7/floppy.c"
#elif MCU == AT32F4
#include "mcu/at32f4/floppy.c"
#endif

static struct index {
    /* Main code can reset this at will. */
    volatile unsigned int count;
    /* For synchronising index pulse reporting to the RDATA flux stream. */
    volatile unsigned int rdata_cnt;
    /* Threshold and trigger for detecting a hard-sector index hole. */
    uint32_t hard_sector_thresh; /* hole-to-hole threshold to detect index */
    uint32_t hard_sector_trigger; /* != 0 -> trigger is primed */
    /* Last time at which index was triggered. */
    time_t trigger_time;
    /* Timer structure for index_timer() calls. */
    struct timer timer;
} index;

/* Timer to clean up stale index.trigger_time. */
#define INDEX_TIMER_PERIOD time_ms(5000)
static void index_timer(void *unused);

/* A DMA buffer for running a timer associated with a floppy-data I/O pin. */
static struct dma_ring {
    /* Indexes into the buf[] ring buffer. */
    uint16_t cons; /* dma_rd: our consumer index for flux samples */
    union {
        uint16_t prod; /* dma_wr: our producer index for flux samples */
        timcnt_t prev_sample; /* dma_rd: previous CCRx sample value */
    };
    /* DMA ring buffer of timer values (ARR or CCRx). */
    timcnt_t buf[512];
} dma;

static struct {
    time_t deadline;
    bool_t armed;
} watchdog;

/* Marshalling and unmarshalling of USB packets. */
static struct {
    /* TRUE if a packet is ready: .data and .len are valid. */
    bool_t ready;
    /* Length and contents of the packet (if ready==TRUE). */
    unsigned int len;
    uint8_t data[USB_HS_MPS];
} usb_packet;

/* Read, write, erase: Shared command state. */
static struct {
    union {
        time_t start; /* read, write: Time at which read/write started. */
        time_t end;   /* erase: Time at which to end the erasure. */
    };
    uint8_t status;
} flux_op;

static enum {
    ST_inactive,
    ST_command_wait,
    ST_zlp,
    ST_read_flux,
    ST_read_flux_drain,
    ST_write_flux_wait_data,
    ST_write_flux_wait_index,
    ST_write_flux,
    ST_write_flux_drain,
    ST_erase_flux,
    ST_source_bytes,
    ST_sink_bytes,
    ST_update_bootloader,
    ST_testmode,
} floppy_state = ST_inactive;

static uint32_t u_cons, u_prod;
#define U_MASK(x) ((x)&(U_BUF_SZ-1))

static struct {
    struct timer timer;
    unsigned int mask;
#define DELAY_read  (1u<<0)
#define DELAY_write (1u<<1)
#define DELAY_seek  (1u<<2)
#define DELAY_head  (1u<<3)
} op_delay;
static void op_delay_timer(void *unused);

/* Delay specified operation(s) by specified number of microseconds. */
static void op_delay_async(unsigned int mask, unsigned int usec)
{
    time_t deadline;

    /* Very long delays fall back to synchronous wait. */
    if (usec > 1000000u) {
        delay_us(usec);
        return;
    }

    deadline = time_now() + time_us(usec);
    timer_cancel(&op_delay.timer);
    if ((op_delay.mask != 0) &&
        (time_diff(op_delay.timer.deadline, deadline) < 0))
        deadline = op_delay.timer.deadline;
    op_delay.mask |= mask;
    timer_set(&op_delay.timer, deadline);
}

/* Wait for specified operation(s) to be permitted. */
static void op_delay_wait(unsigned int mask)
{
    while (op_delay.mask & mask)
        cpu_relax();
}

static void drive_deselect(void)
{
    int pin = -1;
    uint8_t rc;

    if (unit_nr == -1)
        return;

    switch (bus_type) {
    case BUS_IBMPC:
        switch (unit_nr) {
        case 0: pin = 14; break;
        case 1: pin = 12; break;
        }
        break;
    case BUS_SHUGART:
        switch (unit_nr) {
        case 0: pin = 10; break;
        case 1: pin = 12; break;
        case 2: pin = 14; break;
        }
        break;
    }

    rc = write_mapped_pin(board_config->user_pins, pin, O_FALSE);
    ASSERT(rc == ACK_OKAY);

    unit_nr = -1;
}

static uint8_t drive_select(uint8_t nr)
{
    int pin = -1;
    uint8_t rc;

    if (nr == unit_nr)
        return ACK_OKAY;

    drive_deselect();

    switch (bus_type) {
    case BUS_IBMPC:
        switch (nr) {
        case 0: pin = 14; break;
        case 1: pin = 12; break;
        default: return ACK_BAD_UNIT;
        }
        break;
    case BUS_SHUGART:
        switch (nr) {
        case 0: pin = 10; break;
        case 1: pin = 12; break;
        case 2: pin = 14; break;
        default: return ACK_BAD_UNIT;
        }
        break;
    default:
        return ACK_NO_BUS;
    }

    rc = write_mapped_pin(board_config->user_pins, pin, O_TRUE);
    if (rc != ACK_OKAY)
        return ACK_BAD_UNIT;

    unit_nr = nr;
    delay_us(delay_params.select_delay);

    return ACK_OKAY;
}

static uint8_t drive_motor(uint8_t nr, bool_t on)
{
    int pin = -1;
    uint8_t rc;

    switch (bus_type) {
    case BUS_IBMPC:
        if (nr >= 2) 
            return ACK_BAD_UNIT;
        if (unit[nr].motor == on)
            return ACK_OKAY;
        switch (nr) {
        case 0: pin = 10; break;
        case 1: pin = 16; break;
        }
        break;
    case BUS_SHUGART:
        if (nr >= 3)
            return ACK_BAD_UNIT;
        /* All shugart units share one motor line. Alias them all to unit 0. */
        nr = 0;
        if (unit[nr].motor == on)
            return ACK_OKAY;
        pin = 16;
        break;
    default:
        return ACK_NO_BUS;
    }

    rc = write_mapped_pin(board_config->user_pins, pin, on ? O_TRUE : O_FALSE);
    if (rc != ACK_OKAY)
        return ACK_BAD_UNIT;

    unit[nr].motor = on;
    if (on)
        delay_ms(delay_params.motor_delay);

    return ACK_OKAY;

}

static uint8_t drive_get_info(int nr, struct gw_drive_info *d)
{
    const struct unit *u;
    uint32_t flags = 0;

    if (nr < 0) {
        if (unit_nr < 0)
            return ACK_NO_UNIT;
        nr = unit_nr;
    }

    switch (bus_type) {
    case BUS_IBMPC:
        if (nr >= 2)
            return ACK_BAD_UNIT;
        break;
    case BUS_SHUGART:
        if (nr >= 3)
            return ACK_BAD_UNIT;
        break;
    default:
        return ACK_NO_BUS;
    }

    u = &unit[unit_nr];
    if (u->initialised)
        flags |= m(_GW_DF_cyl_valid);
    if (u->motor)
        flags |= m(_GW_DF_motor_on);
    if (u->is_flippy)
        flags |= m(_GW_DF_is_flippy);

    d->flags = flags;
    d->cyl = u->cyl;

    return ACK_OKAY;
}

static const struct pin_mapping *find_user_pin(unsigned int pin)
{
    const struct pin_mapping *upin;

    for (upin = board_config->user_pins; upin->pin_id != 0; upin++) {
        if (upin->pin_id == pin)
            return upin;
    }

    return NULL;
}

static uint8_t set_user_pin(unsigned int pin, unsigned int level)
{
    const struct pin_mapping *upin;

    upin = find_user_pin(pin);
    if (upin == NULL)
        return ACK_BAD_PIN;

    gpio_write_pin(gpio_from_id(upin->gpio_bank), upin->gpio_pin, level);
    return ACK_OKAY;
}

static uint8_t get_user_pin(unsigned int pin, uint8_t *p_level)
{
    const struct pin_mapping *upin;

    upin = find_user_pin(pin);
    if (upin == NULL)
        return ACK_BAD_PIN;

    *p_level = gpio_read_pin(gpio_from_id(upin->gpio_bank), upin->gpio_pin);
    return ACK_OKAY;
}

static void reset_user_pins(void)
{
    const struct pin_mapping *upin;

    for (upin = board_config->user_pins; upin->pin_id != 0; upin++)
        gpio_write_pin(gpio_from_id(upin->gpio_bank), upin->gpio_pin, O_FALSE);
}

#define flippy_trk0_sensor_disable() flippy_trk0_sensor(HIGH)
#define flippy_trk0_sensor_enable() flippy_trk0_sensor(LOW)

static bool_t flippy_detect(void)
{
    bool_t is_flippy;
    flippy_trk0_sensor_disable();
    is_flippy = (get_trk0() == HIGH);
    flippy_trk0_sensor_enable();
    return is_flippy;
}

static void step_dir_set(bool_t assert)
{
    write_pin(dir, assert);
    delay_us(10);
}
#define step_dir_out() step_dir_set(FALSE)
#define step_dir_in() step_dir_set(TRUE)

static void step_once(void)
{
    write_pin(step, TRUE);
    delay_us(15);
    write_pin(step, FALSE);
    delay_us(delay_params.step_delay);
}

static uint8_t floppy_seek_initialise(struct unit *u)
{
    int nr;
    uint8_t rc;

    /* Synchronise to cylinder 0. */
    step_dir_out();
    for (nr = 0; nr < 256; nr++) {
        if (get_trk0() == LOW)
            goto found_cyl0;
        step_once();
    }

    rc = ACK_NO_TRK0;
    goto out;

found_cyl0:

    u->cyl = 0;
    u->is_flippy = flippy_detect();

    if (u->is_flippy) {

        /* Trk0 sensor can be asserted at negative cylinder offsets. Seek
         * inwards until the sensor is deasserted. */
        delay_ms(delay_params.seek_settle); /* change of direction */
        step_dir_in();
        for (nr = 0; nr < 10; nr++) {
            step_once();
            if (get_trk0() == HIGH) {
                /* We are now at real cylinder 1. */
                u->cyl = 1;
                break;
            }
        }

        /* Bail if we didn't find cylinder 1. */
        if (u->cyl != 1) {
            rc = ACK_NO_TRK0;
            goto out;
        }

    }

    u->initialised = TRUE;
    rc = ACK_OKAY;

out:
    delay_ms(delay_params.seek_settle);
    return rc;
}

static uint8_t floppy_seek(int cyl)
{
    struct unit *u;
    int nr;

    if (unit_nr < 0)
        return ACK_NO_UNIT;
    u = &unit[unit_nr];

    op_delay_wait(DELAY_seek);

    if (!u->initialised) {
        uint8_t rc = floppy_seek_initialise(u);
        if (rc != ACK_OKAY)
            return rc;
    }

    if (cyl < (u->is_flippy ? -8 : 0))
        return ACK_BAD_CYLINDER;

    if (u->cyl < cyl) {
        nr = cyl - u->cyl;
        step_dir_in();
    } else if (u->cyl > cyl) {
        if (cyl < 0)
            flippy_trk0_sensor_disable();
        nr = u->cyl - cyl;
        step_dir_out();
    } else /* u->cyl == cyl */ {
        return ACK_OKAY;
    }

    while (nr--)
        step_once();

    flippy_trk0_sensor_enable();

    op_delay_async(DELAY_read | DELAY_write | DELAY_seek,
                   delay_params.seek_settle * 1000u);
    u->cyl = cyl;

    return ACK_OKAY;
}

static uint8_t floppy_noclick_step(void)
{
    uint8_t rc;

    /* Make sure the drive is at cylinder 0. */
    rc = floppy_seek(0);
    if (rc != ACK_OKAY)
        return rc;

    /* Step to cylinder -1 should be ignored, but reset Disk Change. */
    step_dir_out();
    step_once();

    /* Does it look like we actually stepped? Get back to cylinder 0 if so. */
    if (get_trk0() == HIGH) {
        delay_ms(delay_params.seek_settle); /* change of direction */
        step_dir_in();
        step_once();
        delay_ms(delay_params.seek_settle);
        /* Discourage further use of this command. */
        return ACK_BAD_CYLINDER;
    }

    return ACK_OKAY;
}

static void index_set_hard_sector_detection(uint32_t hard_sector_ticks)
{
    uint32_t hard_sector_time = time_from_samples(hard_sector_ticks);

    IRQ_global_disable();
    index.hard_sector_thresh = hard_sector_time * 3 / 4;
    index.hard_sector_trigger = 0;
    IRQ_global_enable();
}

static void floppy_flux_end(void)
{
    /* Turn off write pins. */
    if (read_pin(wgate)) {
        write_pin(wgate, FALSE);
        configure_pin(wdata, GPO_bus);
        op_delay_async(DELAY_write | DELAY_seek | DELAY_head,
                       delay_params.post_write);
    }

    /* Turn off timers. */
    tim_rdata->ccer = 0;
    tim_rdata->cr1 = 0;
    tim_rdata->sr = 0; /* dummy, drains any pending DMA */
    tim_wdata->ccer = 0;
    tim_wdata->cr1 = 0;
    tim_wdata->sr = 0; /* dummy, drains any pending DMA */

    /* Turn off DMA. */
    dma_rdata.cr &= ~DMA_CR_EN;
    dma_wdata.cr &= ~DMA_CR_EN;
    while ((dma_rdata.cr & DMA_CR_EN) || (dma_wdata.cr & DMA_CR_EN))
        continue;

    /* Disable hard-sector index detection. */
    index_set_hard_sector_detection(0);
}

static void quiesce_drives(void)
{
    int i;

    floppy_flux_end();

    for (i = 0; i < ARRAY_SIZE(unit); i++) {

        struct unit *u = &unit[i];

        if (u->initialised && (u->cyl < 0)) {
            drive_select(i);
            floppy_seek(0);
        }

        if (u->motor)
            drive_motor(i, FALSE);

    }

    drive_deselect();

    watchdog.armed = FALSE;
}

static void _set_bus_type(uint8_t type)
{
    quiesce_drives();
    bus_type = type;
    unit_nr = -1;
    memset(unit, 0, sizeof(unit));
}

static bool_t set_bus_type(uint8_t type)
{
    if (type == bus_type)
        return TRUE;

    if (type > BUS_SHUGART)
        return FALSE;

    _set_bus_type(type);

    return TRUE;
}

static uint8_t get_floppy_pin(unsigned int pin, uint8_t *p_level)
{
    uint8_t rc = ACK_OKAY;
    switch (pin) {
    case 8:
        *p_level = get_index();
        break;
    case 26:
        *p_level = get_trk0();
        break;
    case 28:
        *p_level = get_wrprot();
        break;
    default:
        rc = mcu_get_floppy_pin(pin, p_level);
        if (rc == ACK_BAD_PIN)
            rc = get_user_pin(pin, p_level);
        break;
    }
    return rc;
}

static void floppy_reset(void)
{
    floppy_state = ST_inactive;
    quiesce_drives();
    act_led(FALSE);
}

void floppy_init(void)
{
    floppy_mcu_init();

    gw_info.fw_major = fw_major;
    gw_info.fw_minor = fw_minor;
    gw_info.usb_buf_kb = U_BUF_SZ >> 10;

    /* Output pins, unbuffered. */
    configure_pin(dir,    GPO_bus);
    configure_pin(step,   GPO_bus);
    configure_pin(wgate,  GPO_bus);
    configure_pin(head,   GPO_bus);
    configure_pin(wdata,  GPO_bus);

    /* Input pins. */
    configure_pin(index,  GPI_bus);
    configure_pin(trk0,   GPI_bus);
    configure_pin(wrprot, GPI_bus);

    /* Configure INDEX-changed IRQs and timer. */
    timer_init(&index.timer, index_timer, NULL);
    index_timer(NULL);
    exti->rtsr = 0;
    exti->imr = exti->ftsr = m(pin_index);
    IRQx_set_prio(irq_index, INDEX_IRQ_PRI);
    IRQx_enable(irq_index);

    op_delay.mask = 0;
    timer_init(&op_delay.timer, op_delay_timer, NULL);

    delay_params = factory_delay_params;

    _set_bus_type(BUS_NONE);
}

struct gw_info gw_info = {
    .is_main_firmware = 1,
    .max_cmd = CMD_MAX,
    .sample_freq = 72000000u,
    .hw_model = MCU
};

static void watchdog_kick(void)
{
    watchdog.deadline = time_now() + time_ms(delay_params.watchdog);
}

static void watchdog_arm(void)
{
    watchdog.armed = TRUE;
    watchdog_kick();
}

static void floppy_end_command(void *ack, unsigned int ack_len)
{
    watchdog_arm();
    usb_write(EP_TX, ack, ack_len);
    u_cons = u_prod = 0;
    if (floppy_state == ST_command_wait)
        act_led(FALSE);
    if (ack_len == usb_bulk_mps) {
        ASSERT(floppy_state == ST_command_wait);
        floppy_state = ST_zlp;
    }
}

/*
 * READ PATH
 */

static struct {
    unsigned int nr_index;
    unsigned int max_index;
    uint32_t max_index_linger;
    time_t deadline;
} read;

static void _write_28bit(uint32_t x)
{
    u_buf[U_MASK(u_prod++)] = 1 | (x << 1);
    u_buf[U_MASK(u_prod++)] = 1 | (x >> 6);
    u_buf[U_MASK(u_prod++)] = 1 | (x >> 13);
    u_buf[U_MASK(u_prod++)] = 1 | (x >> 20);
}

static void rdata_encode_flux(void)
{
    const uint16_t buf_mask = ARRAY_SIZE(dma.buf) - 1;
    uint16_t cons = dma.cons, prod;
    timcnt_t prev = dma.prev_sample, curr, next;
    uint32_t ticks;

    /* We don't want to race the Index IRQ handler. */
    IRQ_global_disable();

    /* Find out where the DMA engine's producer index has got to. */
    prod = (ARRAY_SIZE(dma.buf) - dma_rdata.ndtr) & buf_mask;

    if (read.nr_index != index.count) {
        /* We have just passed the index mark: Record information about 
         * the just-completed revolution. */
        read.nr_index = index.count;
        ticks = (timcnt_t)(index.rdata_cnt - prev);
        IRQ_global_enable(); /* we're done reading ISR variables */
        u_buf[U_MASK(u_prod++)] = 0xff;
        u_buf[U_MASK(u_prod++)] = FLUXOP_INDEX;
        _write_28bit(ticks);
        /* Defer watchdog while read is progressing (as measured by index
         * pulses).  */
        watchdog_kick();
    }

    IRQ_global_enable();

    /* Process the flux timings into the raw bitcell buffer. */
    for (; cons != prod; cons = (cons+1) & buf_mask) {
        next = dma.buf[cons];
        curr = next - prev;
        prev = next;

        ticks = curr;

        if (ticks == 0) {
            /* 0: Skip. */
        } else if (ticks < 250) {
            /* 1-249: One byte. */
            u_buf[U_MASK(u_prod++)] = ticks;
        } else {
            unsigned int high = (ticks-250) / 255;
            if (high < 5) {
                /* 250-1524: Two bytes. */
                u_buf[U_MASK(u_prod++)] = 250 + high;
                u_buf[U_MASK(u_prod++)] = 1 + ((ticks-250) % 255);
            } else {
                /* 1525-(2^28-1): Seven bytes. */
                u_buf[U_MASK(u_prod++)] = 0xff;
                u_buf[U_MASK(u_prod++)] = FLUXOP_SPACE;
                _write_28bit(ticks - 249);
                u_buf[U_MASK(u_prod++)] = 249;
            }
        }
    }

    /* If it has been a long time since the last flux timing, transfer some of
     * the accumulated time to the host in a "long gap" sample. This avoids
     * timing overflow and, because we take care to keep @prev well behind the
     * sample clock, we cannot race the next flux timestamp. */
    curr = tim_rdata->cnt - prev;
    if (unlikely(curr > sample_us(400))) {
        ticks = sample_us(200);
        u_buf[U_MASK(u_prod++)] = 0xff;
        u_buf[U_MASK(u_prod++)] = FLUXOP_SPACE;
        _write_28bit(ticks);
        prev += ticks;
    }

    /* Save our progress for next time. */
    dma.cons = cons;
    dma.prev_sample = prev;
}

static uint8_t floppy_read_prep(const struct gw_read_flux *rf)
{
    op_delay_wait(DELAY_read);

    /* Prepare Timer & DMA. */
    dma_rdata.mar = (uint32_t)(unsigned long)dma.buf;
    dma_rdata.ndtr = ARRAY_SIZE(dma.buf);    
    rdata_prep();

    /* DMA soft state. */
    dma.cons = 0;
    dma.prev_sample = tim_rdata->cnt;

    /* Start Timer. */
    tim_rdata->cr1 = TIM_CR1_CEN;

    index.count = 0;
    usb_packet.ready = FALSE;

    floppy_state = ST_read_flux;
    flux_op.start = time_now();
    flux_op.status = ACK_OKAY;
    memset(&read, 0, sizeof(read));
    read.max_index = rf->max_index ?: INT_MAX;
    read.deadline = flux_op.start;
    read.deadline += rf->ticks ? time_from_samples(rf->ticks) : INT_MAX;
    read.max_index_linger = time_from_samples(rf->max_index_linger);

    return ACK_OKAY;
}

static void make_read_packet(unsigned int n)
{
    unsigned int c = U_MASK(u_cons);
    unsigned int l = U_BUF_SZ - c;
    if (l < n) {
        memcpy(usb_packet.data, &u_buf[c], l);
        memcpy(&usb_packet.data[l], u_buf, n-l);
    } else {
        memcpy(usb_packet.data, &u_buf[c], n);
    }
    u_cons += n;
    usb_packet.ready = TRUE;
    usb_packet.len = n;
}

static void floppy_read(void)
{
    unsigned int avail = (uint32_t)(u_prod - u_cons);

    if (floppy_state == ST_read_flux) {

        rdata_encode_flux();
        avail = (uint32_t)(u_prod - u_cons);

        if (avail > U_BUF_SZ) {

            /* Overflow */
            printk("OVERFLOW %u %u %u %u\n", u_cons, u_prod,
                   usb_packet.ready, ep_tx_ready(EP_TX));
            floppy_flux_end();
            flux_op.status = ACK_FLUX_OVERFLOW;
            floppy_state = ST_read_flux_drain;
            u_cons = u_prod = avail = 0;

        } else if (read.nr_index >= read.max_index) {

            /* Index limit is reached: Now linger for the specified time. */
            time_t deadline = time_now() + read.max_index_linger;
            if (time_diff(deadline, read.deadline) > 0)
                read.deadline = deadline;
            /* Disable max_index check: It's now become a linger deadline. */
            read.max_index = INT_MAX;

        }

        else if (time_since(read.deadline) >= 0) {

            /* Deadline is reached: End the read now. */
            floppy_flux_end();
            floppy_state = ST_read_flux_drain;

        } else if ((index.count == 0)
                   && (read.max_index != INT_MAX)
                   && (time_since(flux_op.start) > time_ms(2000))) {

            /* Timeout if no index within two seconds, unless the read is
             * not index terminated. */
            floppy_flux_end();
            flux_op.status = ACK_NO_INDEX;
            floppy_state = ST_read_flux_drain;
            u_cons = u_prod = avail = 0;

        }

    } else if ((avail < usb_bulk_mps)
               && !usb_packet.ready
               && ep_tx_ready(EP_TX)) {

        /* Final packet, including ACK byte (NUL). */
        memset(usb_packet.data, 0, usb_bulk_mps);
        make_read_packet(avail);
        floppy_state = ST_command_wait;
        floppy_end_command(usb_packet.data, avail+1);
        return; /* FINISHED */

    }

    if (!usb_packet.ready && (avail >= usb_bulk_mps))
        make_read_packet(usb_bulk_mps);

    if (usb_packet.ready && ep_tx_ready(EP_TX)) {
        usb_write(EP_TX, usb_packet.data, usb_packet.len);
        usb_packet.ready = FALSE;
    }
}


/*
 * WRITE PATH
 */

static struct {
    bool_t is_finished;
    bool_t cue_at_index;
    bool_t terminate_at_index;
    uint32_t astable_period;
    uint32_t ticks;
    enum {
        FLUXMODE_idle,    /* generating no flux (awaiting next command) */
        FLUXMODE_oneshot, /* generating a single flux */
        FLUXMODE_astable  /* generating a region of oscillating flux */
    } flux_mode;
} write;

static uint32_t _read_28bit(void)
{
    uint32_t x;
    x  = (u_buf[U_MASK(u_cons++)]       ) >>  1;
    x |= (u_buf[U_MASK(u_cons++)] & 0xfe) <<  6;
    x |= (u_buf[U_MASK(u_cons++)] & 0xfe) << 13;
    x |= (u_buf[U_MASK(u_cons++)] & 0xfe) << 20;
    return x;
}

static unsigned int _wdata_decode_flux(timcnt_t *tbuf, unsigned int nr)
{
#define MIN_PULSE sample_ns(800)

    unsigned int todo = nr;
    uint32_t x, ticks = write.ticks;

    if (todo == 0)
        return 0;

    switch (write.flux_mode) {

    case FLUXMODE_astable: {
        /* Produce flux transitions at the specified period. */
        uint32_t pulse = write.astable_period;
        while (ticks >= pulse) {
            *tbuf++ = pulse - 1;
            ticks -= pulse;
            if (!--todo)
                goto out;
        }
        write.flux_mode = FLUXMODE_idle;
        break;
    }

    case FLUXMODE_oneshot:
        /* If ticks to next flux would overflow the hardware counter, insert
         * extra fluxes as necessary to get us to the proper next flux. */
        while (ticks != (timcnt_t)ticks) {
            uint32_t pulse = (timcnt_t)-1 + 1;
            *tbuf++ = pulse - 1;
            ticks -= pulse;
            if (!--todo)
                goto out;
        }

        /* Process the one-shot unless it's too short, in which case
         * it will be merged into the next region. */
        if (ticks > MIN_PULSE) {
            *tbuf++ = ticks - 1;
            ticks = 0;
            if (!--todo)
                goto out;
        }

        write.flux_mode = FLUXMODE_idle;
        break;

    case FLUXMODE_idle:
        /* Nothing to do (waiting for a flux command). */
        break;

    }

    while (u_cons != u_prod) {

        ASSERT(write.flux_mode == FLUXMODE_idle);

        x = u_buf[U_MASK(u_cons)];
        if (x == 0) {
            /* 0: Terminate */
            u_cons++;
            write.is_finished = TRUE;
            goto out;
        } else if (x < 250) {
            /* 1-249: One byte. Time to next flux.*/
            u_cons++;
        } else if (x < 255) {
            /* 250-254: Two bytes. Time to next flux. */
            if ((uint32_t)(u_prod - u_cons) < 2)
                goto out;
            u_cons++;
            x = 250 + (x - 250) * 255;
            x += u_buf[U_MASK(u_cons++)] - 1;
        } else {
            /* 255: Six bytes */
            uint8_t op;
            if ((uint32_t)(u_prod - u_cons) < 6)
                goto out;
            op = u_buf[U_MASK(u_cons+1)];
            u_cons += 2;
            switch (op) {
            case FLUXOP_SPACE:
                ticks += _read_28bit();
                continue;
            case FLUXOP_ASTABLE: {
                uint32_t period = _read_28bit();
                if ((period < MIN_PULSE) || (period != (timcnt_t)period)) {
                    /* Bad period value: underflow or overflow. */
                    goto error;
                }
                write.astable_period = period;
                write.flux_mode = FLUXMODE_astable;
                goto out;
            }
            default:
                /* Invalid opcode */
                u_cons += 4;
                goto error;
            }
        }

        /* We're now implicitly in FLUXMODE_oneshot, but we don't register it 
         * explicitly as we usually switch straight back to FLUXMODE_idle. */
        ticks += x;

        /* This sample too small? Then ignore this flux transition. */
        if (ticks < MIN_PULSE)
            continue;

        /* This sample overflows the hardware timer's counter width?
         * Then bail, and we'll split it into chunks. */
        if (ticks != (timcnt_t)ticks) {
            write.flux_mode = FLUXMODE_oneshot;
            goto out;
        }

        *tbuf++ = ticks - 1;
        ticks = 0;
        if (!--todo)
            goto out;
    }

out:
    write.ticks = ticks;
    return nr - todo;

error:
    floppy_flux_end();
    flux_op.status = ACK_BAD_COMMAND;
    floppy_state = ST_write_flux_drain;
    goto out;
}

static void wdata_decode_flux(void)
{
    const uint16_t buf_mask = ARRAY_SIZE(dma.buf) - 1;
    uint16_t nr_to_wrap, nr_to_cons, nr, dmacons;

    /* Find out where the DMA engine's consumer index has got to. */
    dmacons = (ARRAY_SIZE(dma.buf) - dma_wdata.ndtr) & buf_mask;

    /* Find largest contiguous stretch of ring buffer we can fill. */
    nr_to_wrap = ARRAY_SIZE(dma.buf) - dma.prod;
    nr_to_cons = (dmacons - dma.prod - 1) & buf_mask;
    nr = min(nr_to_wrap, nr_to_cons);

    /* Now attempt to fill the contiguous stretch with flux data calculated 
     * from buffered bitcell data. */
    dma.prod += _wdata_decode_flux(&dma.buf[dma.prod], nr);
    dma.prod &= buf_mask;
}

static void floppy_process_write_packet(void)
{
    int len = ep_rx_ready(EP_RX);

    if ((len >= 0) && !usb_packet.ready) {
        usb_read(EP_RX, usb_packet.data, len);
        usb_packet.ready = TRUE;
        usb_packet.len = len;
    }

    if (usb_packet.ready) {
        unsigned int avail = U_BUF_SZ - (uint32_t)(u_prod - u_cons);
        unsigned int n = usb_packet.len;
        if (avail >= n) {
            unsigned int p = U_MASK(u_prod);
            unsigned int l = U_BUF_SZ - p;
            if (l < n) {
                memcpy(&u_buf[p], usb_packet.data, l);
                memcpy(u_buf, &usb_packet.data[l], n-l);
            } else {
                memcpy(&u_buf[p], usb_packet.data, n);
            }
            u_prod += n;
            usb_packet.ready = FALSE;
        }
    }
}

static uint8_t floppy_write_prep(const struct gw_write_flux *wf)
{
    if (get_wrprot() == LOW)
        return ACK_WRPROT;

    wdata_prep();

    /* WDATA DMA setup: From a circular buffer into the WDATA Timer's ARR. */
    dma_wdata.par = (uint32_t)(unsigned long)&tim_wdata->arr;
    dma_wdata.mar = (uint32_t)(unsigned long)dma.buf;

    /* Initialise DMA ring indexes (consumer index is implicit). */
    dma_wdata.ndtr = ARRAY_SIZE(dma.buf);
    dma.prod = 0;

    usb_packet.ready = FALSE;

    floppy_state = ST_write_flux_wait_data;
    flux_op.status = ACK_OKAY;
    memset(&write, 0, sizeof(write));
    write.flux_mode = FLUXMODE_idle;
    write.cue_at_index = wf->cue_at_index;
    write.terminate_at_index = wf->terminate_at_index;

    index_set_hard_sector_detection(wf->hard_sector_ticks);

    return ACK_OKAY;
}

static void floppy_write_wait_data(void)
{
    bool_t write_finished;
    unsigned int u_buf_threshold;

    floppy_process_write_packet();
    wdata_decode_flux();
    if (flux_op.status != ACK_OKAY)
        return;

    /* We don't wait for the massive F7 u_buf[] to fill at Full Speed. */
    u_buf_threshold = ((U_BUF_SZ > 16384) && !usb_is_highspeed())
        ? 16384 - 512 : U_BUF_SZ - 512;

    /* Wait for DMA and input buffers to fill, or write stream to end. We must
     * take care because, since we are not yet draining the DMA buffer, the
     * write stream may end without us noticing and setting write.is_finished. 
     * Hence we peek for a NUL byte in the input buffer if it's non-empty. */
    write_finished = ((u_prod == u_cons)
                      ? write.is_finished
                      : (u_buf[U_MASK(u_prod-1)] == 0));
    if (((dma.prod != (ARRAY_SIZE(dma.buf)-1)) 
         || ((uint32_t)(u_prod - u_cons) < u_buf_threshold))
        && !write_finished)
        return;

    op_delay_wait(DELAY_write);

    floppy_state = ST_write_flux_wait_index;
    flux_op.start = time_now();

    /* Enable DMA only after flux values are generated. */
    dma_wdata_start();

    /* Preload timer with first flux value. */
    tim_wdata->egr = TIM_EGR_UG;
    tim_wdata->sr = 0; /* dummy write, gives h/w time to process EGR.UG=1 */

    barrier(); /* Trigger timer update /then/ wait for next index pulse */
    index.count = 0;
}

static void floppy_write_wait_index(void)
{
    if (write.cue_at_index && (index.count == 0)) {
        if (time_since(flux_op.start) > time_ms(2000)) {
            /* Timeout */
            floppy_flux_end();
            flux_op.status = ACK_NO_INDEX;
            floppy_state = ST_write_flux_drain;
        }
        return;
    }

    /* Start timer. */
    tim_wdata->cr1 = TIM_CR1_CEN;

    /* Enable output. */
    configure_pin(wdata, AFO_bus);
    write_pin(wgate, TRUE);

    index.count = 0;
    floppy_state = ST_write_flux;
}

static void floppy_write_check_underflow(void)
{
    uint32_t avail = u_prod - u_cons;

    if (/* We've run the input buffer dry. */
        (avail == 0)
        /* The input buffer is nearly dry, and doesn't contain EOStream. */
        || ((avail < 16) && (u_buf[U_MASK(u_prod-1)] != 0))) {

        /* Underflow */
        printk("UNDERFLOW %u %u %u %u\n", u_cons, u_prod,
               usb_packet.ready, ep_rx_ready(EP_RX));
        floppy_flux_end();
        flux_op.status = ACK_FLUX_UNDERFLOW;
        floppy_state = ST_write_flux_drain;

    }
}

static void floppy_write(void)
{
    const uint16_t buf_mask = ARRAY_SIZE(dma.buf) - 1;
    uint16_t dmacons, todo, prev_todo;

    floppy_process_write_packet();
    wdata_decode_flux();
    if (flux_op.status != ACK_OKAY)
        return;

    /* Early termination on index pulse? */
    if (write.terminate_at_index && (index.count != 0))
        goto terminate;

    if (!write.is_finished) {
        floppy_write_check_underflow();
        return;
    }

    /* Wait for DMA ring to drain. */
    todo = ~0;
    do {
        /* Check for early termination on index pulse. */
        if (write.terminate_at_index && (index.count != 0))
            goto terminate;
        /* Check progress of draining the DMA ring. */
        prev_todo = todo;
        dmacons = (ARRAY_SIZE(dma.buf) - dma_wdata.ndtr) & buf_mask;
        todo = (dma.prod - dmacons) & buf_mask;
    } while ((todo != 0) && (todo <= prev_todo));

terminate:
    floppy_flux_end();
    floppy_state = ST_write_flux_drain;
}

static void floppy_write_drain(void)
{
    /* Drain the write stream. */
    if (!write.is_finished) {
        floppy_process_write_packet();
        (void)_wdata_decode_flux(dma.buf, ARRAY_SIZE(dma.buf));
        return;
    }

    /* Wait for space to write ACK usb_packet. */
    if (!ep_tx_ready(EP_TX))
        return;

    /* ACK with Status byte. */
    u_buf[0] = flux_op.status;
    floppy_state = ST_command_wait;
    floppy_end_command(u_buf, 1);
}


/*
 * ERASE PATH
 */

static uint8_t floppy_erase_prep(const struct gw_erase_flux *ef)
{
    op_delay_wait(DELAY_write);

    if (get_wrprot() == LOW)
        return ACK_WRPROT;

    write_pin(wgate, TRUE);

    floppy_state = ST_erase_flux;
    flux_op.status = ACK_OKAY;
    flux_op.end = time_now() + time_from_samples(ef->ticks);

    return ACK_OKAY;
}

static void floppy_erase(void)
{
    if (time_since(flux_op.end) < 0)
        return;

    floppy_flux_end();

    /* ACK with Status byte. */
    u_buf[0] = flux_op.status;
    floppy_state = ST_command_wait;
    floppy_end_command(u_buf, 1);
}


/*
 * SINK/SOURCE
 */

static struct {
    unsigned int todo;
    unsigned int min_delta;
    unsigned int max_delta;
    unsigned int status;
    uint32_t rand;
} ss;

static uint32_t ss_rand_next(uint32_t x)
{
    return (x&1) ? (x>>1) ^ 0x80000062 : x>>1;
}

static void sink_source_prep(const struct gw_sink_source_bytes *ssb)
{
    ss.min_delta = INT_MAX;
    ss.max_delta = 0;
    ss.todo = ssb->nr_bytes;
    ss.rand = ssb->seed;
    ss.status = ACK_OKAY;
    usb_packet.ready = FALSE;
}

static void ss_update_deltas(int len)
{
    uint32_t *u_times = (uint32_t *)u_buf;
    time_t delta, now = time_now();
    unsigned int p = u_prod;

    /* Every four bytes we store a timestamp in a u_buf[]-sized ring buffer.
     * We then record min/max time taken to overwrite a previous timestamp. */
    while (len--) {
        if (p++ & 3)
            continue;
        delta = time_diff(u_times[U_MASK(p)>>2], now);
        u_times[U_MASK(p)>>2] = now;
        if ((delta > ss.max_delta) && (p >= U_BUF_SZ))
            ss.max_delta = delta;
        if ((delta < ss.min_delta) && (p >= U_BUF_SZ))
            ss.min_delta = delta;
    }

    u_prod = p;
}

static void source_bytes(void)
{
    int i;

    if (!usb_packet.ready) {
        for (i = 0; i < usb_bulk_mps; i++) {
            usb_packet.data[i] = (uint8_t)ss.rand;
            ss.rand = ss_rand_next(ss.rand);
        }
        usb_packet.ready = TRUE;
    }

    if (!ep_tx_ready(EP_TX))
        return;

    usb_packet.ready = FALSE;

    if (ss.todo < usb_bulk_mps) {
        floppy_state = ST_command_wait;
        floppy_end_command(usb_packet.data, ss.todo);
        return; /* FINISHED */
    }

    usb_write(EP_TX, usb_packet.data, usb_bulk_mps);
    ss.todo -= usb_bulk_mps;
    ss_update_deltas(usb_bulk_mps);
}

static void sink_bytes(void)
{
    int i, len;

    if (ss.todo == 0) {
        /* We're done: Wait for space to write the ACK byte. */
        if (!ep_tx_ready(EP_TX))
            return;
        u_buf[0] = ss.status;
        floppy_state = ST_command_wait;
        floppy_end_command(u_buf, 1);
        return; /* FINISHED */
    }

    /* Packet ready? */
    len = ep_rx_ready(EP_RX);
    if (len < 0)
        return;

    /* Read it and adjust byte counter. */
    usb_read(EP_RX, usb_packet.data, len);
    ss.todo = (ss.todo <= len) ? 0 : ss.todo - len;
    ss_update_deltas(len);

    /* Check data. */
    for (i = 0; i < len; i++) {
        if (usb_packet.data[i] != (uint8_t)ss.rand)
            ss.status = ACK_BAD_COMMAND;
        ss.rand = ss_rand_next(ss.rand);
    }
}


/*
 * BOOTLOADER UPDATE
 */

#define BL_START 0x08000000
#define BL_END   ((uint32_t)_stext)
#define BL_SIZE  (BL_END - BL_START)

static struct {
    uint32_t len;
} update;

static void erase_old_bootloader(void)
{
    uint32_t p;
    for (p = BL_START; p < BL_END; p += FLASH_PAGE_SIZE)
        fpec_page_erase(p);
}

static uint8_t update_prep(uint32_t len)
{
    /* Just a bad-sized payload. Shouldn't even have got here. Bad command. */
    if ((len & 3) || (len > BL_SIZE))
        return ACK_BAD_COMMAND;

    /* Doesn't fit in our RAM buffer? Return a special error code. */
    if (len > U_BUF_SZ)
        return ACK_OUT_OF_SRAM;

    floppy_state = ST_update_bootloader;
    update.len = len;

    printk("Update Bootloader: %u bytes\n", len);

    return ACK_OKAY;
}

static void update_continue(void)
{
    uint16_t crc;
    int len, retry;

    /* Read entire new bootloader into the u_buf[] ring. */
    if ((len = ep_rx_ready(EP_RX)) >= 0) {
        len = min_t(int, len, update.len - u_prod);
        usb_read(EP_RX, &u_buf[u_prod], len);
        u_prod += len;
    }

    /* Keep going until we have the entire bootloader. */
    if ((u_prod < update.len) || !ep_tx_ready(EP_TX))
        return;

    /* Validate the new bootloader before erasing the existing one! */
    crc = crc16_ccitt(u_buf, update.len, 0xffff);
    if (crc != 0)
        goto done;

    /* We are now committed to overwriting the existing bootloader. 
     * Try really hard to write the new bootloader (including retries). */
    fpec_init();
    for (retry = 0; retry < 3; retry++) {
        erase_old_bootloader();
        fpec_write(u_buf, update.len, BL_START);
        crc = crc16_ccitt((void *)BL_START, update.len, 0xffff);
        if (crc == 0)
            goto done;
    }

done:
    printk("Final CRC: %04x (%s)\n", crc, crc ? "FAIL" : "OK");
    u_buf[0] = !!crc;
    floppy_state = ST_command_wait;
    floppy_end_command(u_buf, 1);
}


static void process_command(void)
{
    uint8_t cmd = u_buf[0];
    uint8_t len = u_buf[1];
    uint8_t resp_sz = 2;

    watchdog_arm();
    act_led(TRUE);

    switch (cmd) {
    case CMD_GET_INFO: {
        uint8_t idx = u_buf[2];
        if (len != 3)
            goto bad_command;
        memset(&u_buf[2], 0, 32);
        switch(idx) {
        case GETINFO_FIRMWARE: /* gw_info */
            memcpy(&u_buf[2], &gw_info, sizeof(gw_info));
            break;
        case GETINFO_BW_STATS: /* gw_bw_stats */ {
            struct gw_bw_stats bw;
            bw.min_bw.bytes = U_BUF_SZ;
            bw.min_bw.usecs = ss.max_delta / time_us(1);
            bw.max_bw.bytes = U_BUF_SZ;
            bw.max_bw.usecs = ss.min_delta / time_us(1);
            memcpy(&u_buf[2], &bw, sizeof(bw));
            break;
        }
        case GETINFO_CURRENT_DRIVE:
        case GETINFO_DRIVE(0) ... GETINFO_DRIVE(2): {
            struct gw_drive_info d;
            int unit_nr = cmd - GETINFO_DRIVE(0);
            u_buf[1] = drive_get_info(unit_nr, &d);
            if (u_buf[1] != ACK_OKAY)
                goto out;
            memcpy(&u_buf[2], &d, sizeof(d));
            break;
        }
        default:
            goto bad_command;
        }
        resp_sz += 32;
        break;
    }
    case CMD_UPDATE: {
        uint32_t u_len = *(uint32_t *)&u_buf[2];
        uint32_t signature = *(uint32_t *)&u_buf[6];
        if (len != 10) goto bad_command;
        if (signature != 0xdeafbee3) goto bad_command;
        u_buf[1] = update_prep(u_len);
        goto out;
    }
    case CMD_SEEK: {
        int cyl;
        if (len == 3) {
            cyl = *(int8_t *)&u_buf[2];
        } else if (len == 4) {
            cyl = *(int16_t *)&u_buf[2];
        } else {
            goto bad_command;
        }
        u_buf[1] = floppy_seek(cyl);
        goto out;
    }
    case CMD_HEAD: {
        uint8_t head = u_buf[2];
        if ((len != 3) || (head > 1))
            goto bad_command;
        if (read_pin(head) != head) {
            op_delay_wait(DELAY_head);
            write_pin(head, head);
            op_delay_async(DELAY_write, delay_params.pre_write);
        }
        break;
    }
    case CMD_SET_PARAMS: {
        uint8_t idx = u_buf[2];
        if ((len < 3) || (idx != PARAMS_DELAYS)
            || (len > (3 + sizeof(delay_params))))
            goto bad_command;
        memcpy(&delay_params, &u_buf[3], len-3);
        break;
    }
    case CMD_GET_PARAMS: {
        uint8_t idx = u_buf[2];
        uint8_t nr = u_buf[3];
        if ((len != 4) || (idx != PARAMS_DELAYS)
            || (nr > sizeof(delay_params)))
            goto bad_command;
        memcpy(&u_buf[2], &delay_params, nr);
        resp_sz += nr;
        break;
    }
    case CMD_MOTOR: {
        uint8_t unit = u_buf[2], on_off = u_buf[3];
        if ((len != 4) || (on_off & ~1))
            goto bad_command;
        u_buf[1] = drive_motor(unit, on_off & 1);
        goto out;
    }
    case CMD_READ_FLUX: {
        struct gw_read_flux rf = {
            .max_index_linger = sample_us(500)
        };
        if ((len < (2 + offsetof(struct gw_read_flux, max_index_linger)))
            || (len > (2 + sizeof(rf))))
            goto bad_command;
        memcpy(&rf, &u_buf[2], len-2);
        u_buf[1] = floppy_read_prep(&rf);
        goto out;
    }
    case CMD_WRITE_FLUX: {
        struct gw_write_flux wf = {};
        if ((len < (2 + offsetof(struct gw_write_flux, hard_sector_ticks)))
            || (len > (2 + sizeof(wf))))
            goto bad_command;
        memcpy(&wf, &u_buf[2], len-2);
        u_buf[1] = floppy_write_prep(&wf);
        goto out;
    }
    case CMD_GET_FLUX_STATUS: {
        if (len != 2)
            goto bad_command;
        u_buf[1] = flux_op.status;
        goto out;
    }
    case CMD_SELECT: {
        uint8_t unit = u_buf[2];
        if (len != 3)
            goto bad_command;
        u_buf[1] = drive_select(unit);
        goto out;
    }
    case CMD_DESELECT: {
        if (len != 2)
            goto bad_command;
        drive_deselect();
        break;
    }
    case CMD_SET_BUS_TYPE: {
        uint8_t type = u_buf[2];
        if ((len != 3) || !set_bus_type(type))
            goto bad_command;
        break;
    }
    case CMD_SET_PIN: {
        uint8_t pin = u_buf[2];
        uint8_t level = u_buf[3];
        if ((len != 4) || (level & ~1))
            goto bad_command;
        u_buf[1] = set_user_pin(pin, level);
        goto out;
    }
    case CMD_GET_PIN: {
        uint8_t pin = u_buf[2];
        if (len != 3)
            goto bad_command;
        u_buf[1] = get_floppy_pin(pin, &u_buf[2]);
        if (u_buf[1] == ACK_OKAY)
            resp_sz += 1;
        goto out;
    }
    case CMD_RESET: {
        if (len != 2)
            goto bad_command;
        delay_params = factory_delay_params;
        _set_bus_type(BUS_NONE);
        reset_user_pins();
        break;
    }
    case CMD_ERASE_FLUX: {
        struct gw_erase_flux ef;
        if (len != (2 + sizeof(ef)))
            goto bad_command;
        memcpy(&ef, &u_buf[2], len-2);
        u_buf[1] = floppy_erase_prep(&ef);
        goto out;
    }
    case CMD_SOURCE_BYTES:
    case CMD_SINK_BYTES: {
        struct gw_sink_source_bytes ssb;
        if (len != (2 + sizeof(ssb)))
            goto bad_command;
        memcpy(&ssb, &u_buf[2], len-2);
        floppy_state = (cmd == CMD_SOURCE_BYTES)
            ? ST_source_bytes : ST_sink_bytes;
        sink_source_prep(&ssb);
        break;
    }
    case CMD_SWITCH_FW_MODE: {
        uint8_t mode = u_buf[2];
        if ((len != 3) || (mode & ~1))
            goto bad_command;
        if (mode == FW_MODE_BOOTLOADER) {
            usb_deinit();
            delay_us(100);
            _reset_flag = 0xdeadbeef;
            dcache_disable();
            system_reset();
        }
        break;
    }
    case CMD_TEST_MODE: {
        uint32_t sig1 = *(uint32_t *)&u_buf[2];
        uint32_t sig2 = *(uint32_t *)&u_buf[6];
        if (len != 10) goto bad_command;
        if (sig1 != 0x6e504b4e) goto bad_command;
        if (sig2 != 0x382910d3) goto bad_command;
        u_buf[1] = testmode_init();
        if (u_buf[1] == ACK_OKAY)
            floppy_state = ST_testmode;
        goto out;
    }
    case CMD_NOCLICK_STEP: {
        if (len != 2)
            goto bad_command;
        u_buf[1] = floppy_noclick_step();
        goto out;
    }
    default:
        goto bad_command;
    }

    u_buf[1] = ACK_OKAY;
out:
    floppy_end_command(u_buf, resp_sz);
    return;

bad_command:
    u_buf[1] = ACK_BAD_COMMAND;
    goto out;
}

static void floppy_configure(void)
{
    watchdog_arm();
    floppy_flux_end();
    floppy_state = ST_command_wait;
    u_cons = u_prod = 0;
    act_led(FALSE);
}

void floppy_process(void)
{
    int len;

    if (watchdog.armed && (time_since(watchdog.deadline) >= 0)) {
        floppy_configure();
        quiesce_drives();
    }

    switch (floppy_state) {

    case ST_command_wait:

        len = ep_rx_ready(EP_RX);
        if ((len >= 0) && (len < (U_BUF_SZ-u_prod))) {
            usb_read(EP_RX, &u_buf[u_prod], len);
            u_prod += len;
        }

        if ((u_prod >= 2) && (u_prod >= u_buf[1]) && ep_tx_ready(EP_TX)) {
            process_command();
        }

        break;

    case ST_zlp:
        if (ep_tx_ready(EP_TX)) {
            usb_write(EP_TX, NULL, 0);
            floppy_state = ST_command_wait;
        }
        break;

    case ST_read_flux:
    case ST_read_flux_drain:
        floppy_read();
        break;

    case ST_write_flux_wait_data:
        floppy_write_wait_data();
        break;

    case ST_write_flux_wait_index:
        floppy_write_wait_index();
        break;

    case ST_write_flux:
        floppy_write();
        break;

    case ST_write_flux_drain:
        floppy_write_drain();
        break;

    case ST_erase_flux:
        floppy_erase();
        break;

    case ST_source_bytes:
        source_bytes();
        break;

    case ST_sink_bytes:
        sink_bytes();
        break;

    case ST_update_bootloader:
        update_continue();
        break;

    case ST_testmode:
        watchdog.armed = FALSE;
        testmode_process();
        break;

    default:
        break;

    }
}

const struct usb_class_ops usb_cdc_acm_ops = {
    .reset = floppy_reset,
    .configure = floppy_configure
};

/*
 * INTERRUPT HANDLERS
 */

static void IRQ_INDEX_changed(void)
{
    unsigned int cnt = tim_rdata->cnt;
    time_t now = time_now();
    int32_t delta;

    /* Clear INDEX-changed flag. */
    exti->pr = m(pin_index);

    delta = time_diff(index.trigger_time, now);
    if (delta < time_us(delay_params.index_mask))
        return;
    index.trigger_time = now;

    if (unlikely(index.hard_sector_thresh != 0)) {
        if (delta > index.hard_sector_thresh) {
            /* Long pulse indicates a subsequent sector hole. Filter it out
             * and unprime the index trigger. */
            index.hard_sector_trigger = 0;
            return;
        }
        /* First short pulse indicates the extra (index) hole. Second
         * consecutive short pulse is the first sector hole: That's the only
         * one we count. */
        index.hard_sector_trigger ^= 1;
        if (index.hard_sector_trigger) {
            /* Filter out the "rising edge" of the trigger. */
            return;
        }
    }

    index.count++;
    index.rdata_cnt = cnt;
}

static void index_timer(void *unused)
{
    time_t now = time_now();
    IRQ_global_disable();
    /* index.trigger_time mustn't get so old that the time_diff() test in
     * IRQ_INDEX_changed() overflows. To prevent this, we ensure that,
     * at all times,
     *   time_diff(index.trigger_time, now) < 2*INDEX_TIMER_PERIOD + delta,
     * where delta is small. */
    if (time_diff(index.trigger_time, now) > INDEX_TIMER_PERIOD)
        index.trigger_time = now - INDEX_TIMER_PERIOD;
    IRQ_global_enable();
    timer_set(&index.timer, now + INDEX_TIMER_PERIOD);
}

static void op_delay_timer(void *unused)
{
    while (time_diff(time_now(), op_delay.timer.deadline) > 0)
        cpu_relax();
    op_delay.mask = 0;
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
