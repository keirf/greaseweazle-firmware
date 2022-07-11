/*
 * hw_f1.c
 * 
 * STM32F103 USBD.
 * 
 * Written & released by Keir Fraser <keir.xen@gmail.com>
 * 
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

static const struct usb_driver *drv = &usbd;

void hw_usb_init(void)
{
#if !defined(BOOTLOADER)
    /* We detect an Artery MCU by presence of Cortex-M4 CPUID. Cortex-M4:
     * 41xfc24x ; Cortex-M3: 41xfc23x */
    bool_t is_artery_mcu = ((scb->cpuid >> 4) & 0xf) == 4;
    if (is_artery_mcu)
        drv = &usbd_at32f4;
#endif

    drv->init();

    /* Indicate we are connected by pulling up D+. */
    gpio_configure_pin(gpioa, 0, GPO_pushpull(_2MHz, HIGH));
}

void hw_usb_deinit(void)
{
    gpio_configure_pin(gpioa, 0, GPI_floating);

    drv->deinit();
}

bool_t hw_has_highspeed(void)
{
    return drv->has_highspeed();
}

bool_t usb_is_highspeed(void)
{
    return drv->is_highspeed();
}

int ep_rx_ready(uint8_t epnr)
{
    return drv->ep_rx_ready(epnr);
}

bool_t ep_tx_ready(uint8_t epnr)
{
    return drv->ep_tx_ready(epnr);
}
 
void usb_read(uint8_t epnr, void *buf, uint32_t len)
{
    drv->read(epnr, buf, len);
}

void usb_write(uint8_t epnr, const void *buf, uint32_t len)
{
    drv->write(epnr, buf, len);
}
 
void usb_stall(uint8_t epnr)
{
    drv->stall(epnr);
}

void usb_configure_ep(uint8_t epnr, uint8_t type, uint32_t size)
{
    drv->configure_ep(epnr, type, size);
}

void usb_setaddr(uint8_t addr)
{
    drv->setaddr(addr);
}

void usb_process(void)
{
    drv->process();
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
