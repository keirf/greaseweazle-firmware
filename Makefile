
export FW_MAJOR := 1
export FW_MINOR := 0

TARGETS := all blinky clean dist mrproper f1_ocd ocd flash start serial
.PHONY: $(TARGETS)

ifneq ($(RULES_MK),y)

export ROOT := $(CURDIR)

$(TARGETS):
	$(MAKE) -f $(ROOT)/Rules.mk $@

else

PROJ = greaseweazle-firmware
VER := v$(FW_MAJOR).$(FW_MINOR)

SUBDIRS += src bootloader blinky_test

all:
	$(MAKE) -C src -f $(ROOT)/Rules.mk \
		greaseweazle.elf greaseweazle.bin greaseweazle.hex
	$(MAKE) bootloader=y -C bootloader -f $(ROOT)/Rules.mk \
		bootloader.elf bootloader.bin bootloader.hex
	srec_cat bootloader/bootloader.hex -Intel src/greaseweazle.hex -Intel \
	-o $(PROJ)-$(VER).hex -Intel
	$(PYTHON) ./scripts/mk_update.py new $(PROJ)-$(VER).upd \
		bootloader/bootloader.bin src/greaseweazle.bin $(mcu)

blinky:
	$(MAKE) debug=y mcu=stm32f1 -C blinky_test -f $(ROOT)/Rules.mk \
		blinky.elf blinky.bin blinky.hex

clean::
	rm -f *.hex *.upd
	find . -name __pycache__ | xargs rm -rf

dist:
	rm -rf $(PROJ)-*
	mkdir -p $(PROJ)-$(VER)/hex/alt
	$(MAKE) clean
	$(MAKE) mcu=stm32f1 all blinky
	cp -a $(PROJ)-$(VER).hex $(PROJ)-$(VER)/hex/$(PROJ)-f1-$(VER).hex
	cp -a $(PROJ)-$(VER).upd $(PROJ)-$(VER)/$(PROJ)-$(VER).upd
	cp -a blinky_test/blinky.hex $(PROJ)-$(VER)/hex/alt/blinky-test-f1-$(VER).hex
	cp -a COPYING $(PROJ)-$(VER)/
	cp -a README $(PROJ)-$(VER)/
	cp -a RELEASE_NOTES $(PROJ)-$(VER)/
	$(MAKE) clean
	$(MAKE) mcu=stm32f7 all
	cp -a $(PROJ)-$(VER).hex $(PROJ)-$(VER)/hex/$(PROJ)-f7-$(VER).hex
	$(PYTHON) ./scripts/mk_update.py cat $(PROJ)-$(VER)/$(PROJ)-$(VER).upd \
		$(PROJ)-$(VER)/$(PROJ)-$(VER).upd $(PROJ)-$(VER).upd
	$(MAKE) clean
	$(MAKE) mcu=at32f4 all
	cp -a $(PROJ)-$(VER).hex $(PROJ)-$(VER)/hex/$(PROJ)-at32f4-$(VER).hex
	$(PYTHON) ./scripts/mk_update.py cat $(PROJ)-$(VER)/$(PROJ)-$(VER).upd \
		$(PROJ)-$(VER)/$(PROJ)-$(VER).upd $(PROJ)-$(VER).upd
	$(MAKE) clean
	$(ZIP) $(PROJ)-$(VER).zip $(PROJ)-$(VER)

mrproper: clean
	rm -rf $(PROJ)-*

BAUD=115200
DEV=/dev/ttyUSB0

ocd: all
	$(PYTHON) scripts/telnet.py localhost 4444 \
	"reset init ; flash write_image erase `pwd`/$(PROJ)-$(VER).hex ; reset"

f1_ocd: all
	python3 scripts/openocd/flash.py `pwd`/$(PROJ)-$(VER).hex

flash: all
	sudo stm32flash -b $(BAUD) -w $(PROJ)-$(VER).hex $(DEV)

start:
	sudo stm32flash -b $(BAUD) -g 0 $(DEV)

serial:
	sudo miniterm.py $(DEV) 3000000

endif
