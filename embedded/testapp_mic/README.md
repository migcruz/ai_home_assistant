# testapp_mic

Zephyr PDM capture test app for XIAO ESP32S3 Sense:

- `setPinsPdmRx(42, 41)` via devicetree overlay
- `begin(PDM_RX, 16000, 16-bit)` behavior via Zephyr I2S + post-start register patching
- Records `20` seconds of mono PCM WAV to SD card at `/SD:/REC.wav`
- Keeps periodic capture stats + formatted I2S register dumps during recording

## Build

```bash
cd /zephyr-ws/embedded/testapp_mic
make
```

## Flash

```bash
make flash
```
