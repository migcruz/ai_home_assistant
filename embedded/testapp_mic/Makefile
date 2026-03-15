BOARD ?= xiao_esp32s3/esp32s3/procpu/sense
BUILD := build

OVERLAY ?= $(CURDIR)/testapp_mic.overlay
OVERLAY_ABS := $(abspath $(OVERLAY))

.PHONY: all clean flash menuconfig

all:
	west build -d $(BUILD) -b $(BOARD) . --pristine \
		-- -DDTC_OVERLAY_FILE=$(OVERLAY_ABS)

clean:
	rm -rf $(BUILD)

flash:
	west flash -d $(BUILD)

menuconfig:
	west build -d $(BUILD) -b $(BOARD) -t menuconfig . \
		-- -DDTC_OVERLAY_FILE=$(OVERLAY_ABS)

