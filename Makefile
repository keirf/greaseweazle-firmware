
export FW_MAJOR := 1
export FW_MINOR := 4

PROJ = greaseweazle-firmware
VER := $(FW_MAJOR).$(FW_MINOR)

PYTHON := python3

export ROOT := $(CURDIR)

.PHONY: FORCE

.DEFAULT_GOAL := all

prod-%: FORCE
	$(MAKE) target mcu=$* target=bootloader level=prod
	$(MAKE) target mcu=$* target=greaseweazle level=prod

debug-%: FORCE
	$(MAKE) target mcu=$* target=bootloader level=debug
	$(MAKE) target mcu=$* target=greaseweazle level=debug

all-%: FORCE prod-% debug-% ;

all: FORCE all-stm32f1 all-stm32f7 all-at32f4 ;
	$(MAKE) target mcu=stm32f1 target=blinky level=debug

clean: FORCE
	rm -rf out

out: FORCE
	+mkdir -p out/$(mcu)/$(level)/$(target)

target: FORCE out
	$(MAKE) -C out/$(mcu)/$(level)/$(target) -f $(ROOT)/Rules.mk target.bin target.hex target.upd $(mcu)=y $(level)=y $(target)=y

dist: level := prod
dist: t := $(ROOT)/out/$(PROJ)-$(VER)
dist: FORCE all
	rm -rf out/$(PROJ)-*
	mkdir -p $(t)/hex/alt
	cd out/stm32f1/$(level)/greaseweazle; \
	  cp -a target.hex $(t)/hex/$(PROJ)-f1-$(VER).hex; \
	  cp -a ../bootloader/target.upd $(t)/$(PROJ)-$(VER).upd; \
	  $(PYTHON) $(ROOT)/scripts/mk_update.py cat $(t)/$(PROJ)-$(VER).upd \
	    target.upd
	cd out/stm32f1/debug/blinky; \
	  cp -a target.hex $(t)/hex/alt/blinky-test-f1-$(VER).hex
	cd out/stm32f7/$(level)/greaseweazle; \
	  cp -a target.hex $(t)/hex/$(PROJ)-f7-$(VER).hex; \
	  $(PYTHON) $(ROOT)/scripts/mk_update.py cat $(t)/$(PROJ)-$(VER).upd \
	    ../bootloader/target.upd target.upd
	cd out/at32f4/$(level)/greaseweazle; \
	  cp -a target.hex $(t)/hex/$(PROJ)-at32f4-$(VER).hex; \
	  $(PYTHON) $(ROOT)/scripts/mk_update.py cat $(t)/$(PROJ)-$(VER).upd \
	    ../bootloader/target.upd target.upd
	cp -a COPYING $(t)/
	cp -a README $(t)/
	cp -a RELEASE_NOTES $(t)/
	cd out && zip -r $(PROJ)-$(VER).zip $(PROJ)-$(VER)

BAUD=115200
DEV=/dev/ttyUSB0
SUDO=sudo
STM32FLASH=stm32flash
T=out/$(target)/greaseweazle/target.hex

ocd: FORCE all
	$(PYTHON) scripts/telnet.py localhost 4444 \
	"reset init ; flash write_image erase `pwd`/$(T) ; reset"

f1_ocd: FORCE all
	$(PYTHON) scripts/openocd/flash.py `pwd`/$(T)

flash: FORCE all
	$(SUDO) $(STM32FLASH) -b $(BAUD) -w $(T) $(DEV)

start: FORCE
	$(SUDO) $(STM32FLASH) -b $(BAUD) -g 0 $(DEV)

serial: FORCE
	$(SUDO) miniterm.py $(DEV) 3000000
