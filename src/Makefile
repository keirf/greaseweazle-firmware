OBJS += board.o
OBJS += build_info.o
OBJS += crc.o
OBJS += vectors.o
OBJS += string.o
OBJS += cortex.o
OBJS += time.o
OBJS += timer.o
OBJS += util.o
OBJS-$(debug) += console.o

ifeq ($(blinky),y)

OBJS += blinky.o

else ifeq ($(bootloader),y)

OBJS += fw_update.o

else

OBJS += main.o
OBJS += floppy.o
OBJS += testmode.o

endif

SUBDIRS += mcu usb

.PHONY: $(SRCDIR)/build_info.c
build_info.o: CFLAGS += -DFW_MAJOR=$(FW_MAJOR) -DFW_MINOR=$(FW_MINOR)
