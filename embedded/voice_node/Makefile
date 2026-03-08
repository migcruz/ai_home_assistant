BOARD   ?= xiao_esp32s3/esp32s3/procpu/sense
BUILD   := build

.PHONY: all clean flash menuconfig

all:
	west build --sysbuild -d $(BUILD) -b $(BOARD) procpu

clean:
	rm -rf $(BUILD)

flash:
	west flash -d $(BUILD)

menuconfig:
	west build --sysbuild -d $(BUILD) -b $(BOARD) -t menuconfig procpu
