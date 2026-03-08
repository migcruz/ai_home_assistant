# sysbuild.cmake — voice_node AMP build
#
# Builds two images together:
#   voice_node         → PRO CPU (Core 0): WiFi, networking, audio I/O
#   voice_node_appcpu  → APP CPU (Core 1): wake word inference (future)
#
# Build: west build --sysbuild -d ../build -b xiao_esp32s3/esp32s3/procpu/sense .
# Flash: west flash -d ../build   (flashes both images + MCUboot)

ExternalZephyrProject_Add(
    APPLICATION voice_node_appcpu
    SOURCE_DIR  ${APP_DIR}/../appcpu
    BOARD       xiao_esp32s3/esp32s3/appcpu
)

# APP CPU image must be built before PRO CPU so the loader has the binary ready
add_dependencies(procpu voice_node_appcpu)
sysbuild_add_dependencies(CONFIGURE procpu voice_node_appcpu)

if(SB_CONFIG_BOOTLOADER_MCUBOOT)
    # MCUboot must be flashed before either app image
    sysbuild_add_dependencies(FLASH voice_node_appcpu mcuboot)
endif()
