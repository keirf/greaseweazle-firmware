OBJS += config.o
OBJS += core.o
OBJS += cdc_acm.o

OBJS-$(stm32f1) += hw_usbd.o
OBJS-$(stm32f1) += hw_f1.o

OBJS-$(stm32f7) += hw_dwc_otg.o
OBJS-$(stm32f7) += hw_f7.o

OBJS-$(at32f4) += hw_dwc_otg.o
OBJS-$(at32f4) += hw_usbd_at32f4.o
OBJS-$(at32f4) += hw_at32f4.o

$(OBJS) $(OBJS-y): CFLAGS += -include $(SRCDIR)/defs.h
