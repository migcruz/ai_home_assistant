BOARD   ?= xiao_esp32s3/esp32s3/procpu/sense
BUILD   := build

.PHONY: all clean flash menuconfig

all:
	west build -d $(BUILD) -b $(BOARD) .

clean:
	rm -rf $(BUILD)

flash:
	west flash -d $(BUILD)

menuconfig:
	west build -d $(BUILD) -b $(BOARD) -t menuconfig .
