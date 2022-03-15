TOOL_PREFIX = arm-none-eabi-
CC = $(TOOL_PREFIX)gcc
OBJCOPY = $(TOOL_PREFIX)objcopy
LD = $(TOOL_PREFIX)ld

PYTHON = python3
ZIP = zip -r
UNZIP = unzip

ifneq ($(VERBOSE),1)
TOOL_PREFIX := @$(TOOL_PREFIX)
endif

FLAGS  = -g -Os -nostdlib -std=gnu99 -iquote $(ROOT)/inc
FLAGS += -Wall -Werror -Wno-format -Wdeclaration-after-statement
FLAGS += -Wstrict-prototypes -Wredundant-decls -Wnested-externs
FLAGS += -fno-common -fno-exceptions -fno-strict-aliasing
FLAGS += -mlittle-endian -mthumb -mfloat-abi=soft
FLAGS += -Wno-unused-value -ffunction-sections

ifeq ($(mcu),stm32f1)
FLAGS += -mcpu=cortex-m3 -DSTM32F1=1 -DMCU=1
stm32f1=y
else ifeq ($(mcu),stm32f7)
FLAGS += -mcpu=cortex-m7 -DSTM32F7=7 -DMCU=7
stm32f7=y
else ifeq ($(mcu),at32f4)
FLAGS += -mcpu=cortex-m4 -DAT32F4=4 -DMCU=4
at32f4=y
endif

ifneq ($(debug),y)
FLAGS += -DNDEBUG
endif

ifeq ($(bootloader),y)
FLAGS += -DBOOTLOADER=1
endif

FLAGS += -MMD -MF .$(@F).d
DEPS = .*.d

FLAGS += $(FLAGS-y)

CFLAGS += $(CFLAGS-y) $(FLAGS) -include decls.h
AFLAGS += $(AFLAGS-y) $(FLAGS) -D__ASSEMBLY__
LDFLAGS += $(LDFLAGS-y) $(FLAGS) -Wl,--gc-sections

SRCDIR := $(shell $(PYTHON) $(ROOT)/scripts/srcdir.py $(CURDIR))
include $(SRCDIR)/Makefile

SUBDIRS += $(SUBDIRS-y)
OBJS += $(OBJS-y) $(patsubst %,%/build.o,$(SUBDIRS))

# Force execution of pattern rules (for which PHONY cannot be directly used).
.PHONY: FORCE
FORCE:

.PHONY: clean

.SECONDARY:

build.o: $(OBJS)
	$(LD) -r -o $@ $^

%/build.o: FORCE
	$(MAKE) -f $(ROOT)/Rules.mk -C $* build.o

%.ld: $(SRCDIR)/%.ld.S $(SRCDIR)/Makefile
	@echo CPP $@
	$(CC) -P -E $(AFLAGS) $< -o $@

%.elf: $(OBJS) %.ld $(SRCDIR)/Makefile
	@echo LD $@
	$(CC) $(LDFLAGS) -T$(*F).ld $(OBJS) -o $@
	chmod a-x $@

%.hex: %.elf
	@echo OBJCOPY $@
	$(OBJCOPY) -O ihex $< $@
	chmod a-x $@
ifneq ($(bootloader),y)
	srec_cat ../bootloader/target.hex -Intel $@ -Intel -o $@ -Intel
endif

%.bin: %.elf
	@echo OBJCOPY $@
	$(OBJCOPY) -O binary $< $@
	chmod a-x $@

%.upd: %.bin
	$(PYTHON) $(ROOT)/scripts/mk_update.py new $@ \
	$< $(mcu)-$(FW_MAJOR).$(FW_MINOR)-$(bootloader)

%.o: $(SRCDIR)/%.c $(SRCDIR)/Makefile
	@echo CC $@
	$(CC) $(CFLAGS) -c $< -o $@

%.o: $(SRCDIR)/%.S $(SRCDIR)/Makefile
	@echo AS $@
	$(CC) $(AFLAGS) -c $< -o $@

-include $(DEPS)
