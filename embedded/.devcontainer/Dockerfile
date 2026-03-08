FROM ghcr.io/zephyrproject-rtos/zephyr-build:latest

USER root

# Flashing tools for every supported MCU vendor.
# openocd:   ARM Cortex-M (STM32, nRF, SAM, etc.) via JTAG/SWD
# dfu-util:  DFU-mode flashing (STM32, nRF52840 bootloader, etc.)
# pyocd:     ARM Cortex-M debug/flash via CMSIS-DAP
# esptool:   ESP32 family (serial + USB DFU)
# All tools reach the board through the host USB bus exposed via --privileged.
RUN pip3 install --no-cache-dir esptool pyocd

# xxd: inspect binary build artifacts (ELF, WAV headers, etc.)
RUN apt-get update -q && apt-get install -y --no-install-recommends \
    xxd wget xz-utils \
    openocd dfu-util \
    picocom \
    sudo \
    && rm -rf /var/lib/apt/lists/*

# Allow user to chmod serial devices without a password.
# Needed because the host (Arch) uses the uucp group for /dev/ttyACM*,
# which has a different GID than the container's dialout group.
RUN echo "user ALL=(root) NOPASSWD: /bin/chmod" >> /etc/sudoers

# Install Zephyr SDK 0.17.4 with ALL toolchains (full tarball ~4 GB).
# zephyr-build:latest ships SDK 1.0.0-rc1 which restructured its cmake layout
# and is incompatible with Zephyr 4.3.0. SDK 0.17.4 is the last stable release
# that works with Zephyr 4.x.
RUN wget -q https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v0.17.4/zephyr-sdk-0.17.4_linux-x86_64.tar.xz \
    && tar -xf zephyr-sdk-0.17.4_linux-x86_64.tar.xz -C /opt/toolchains \
    && /opt/toolchains/zephyr-sdk-0.17.4/setup.sh -c \
    && rm zephyr-sdk-0.17.4_linux-x86_64.tar.xz

# Add user to dialout so esptool/west flash can access /dev/ttyACM* and
# /dev/ttyUSB* without permission errors when --privileged is set.
RUN usermod -aG dialout user

# Create the zephyr west workspace directory with correct ownership.
# Docker copies this into the named volume on first mount.
RUN mkdir -p /zephyr-ws && chown user:user /zephyr-ws

USER user

# Point Zephyr cmake at the compatible SDK.
ENV ZEPHYR_SDK_INSTALL_DIR=/opt/toolchains/zephyr-sdk-0.17.4
