BOARD   ?= xiao_esp32s3/esp32s3/procpu/sense
BUILD   := build

PROCPU_OVERLAY := $(CURDIR)/overlays/procpu.overlay
APPCPU_OVERLAY := $(CURDIR)/overlays/appcpu.overlay

.PHONY: all clean flash menuconfig

all:
	west build --sysbuild -d $(BUILD) -b $(BOARD) procpu --pristine \
		-- -DDTC_OVERLAY_FILE=$(PROCPU_OVERLAY) \
		   -Dvoice_node_appcpu_DTC_OVERLAY_FILE=$(APPCPU_OVERLAY)

clean:
	rm -rf $(BUILD)

flash:
	west flash -d $(BUILD)

menuconfig:
	west build --sysbuild -d $(BUILD) -b $(BOARD) -t menuconfig procpu \
		-- -DDTC_OVERLAY_FILE=$(PROCPU_OVERLAY) \
		   -Dvoice_node_appcpu_DTC_OVERLAY_FILE=$(APPCPU_OVERLAY)
