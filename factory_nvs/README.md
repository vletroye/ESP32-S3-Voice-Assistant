# Configuration mode of the voice assistant

This project is the **configuration mode** of the assistant: a second firmware, in the `ota_0` partition, that
the assistant switches to (first start, "édite la config", Configuration screen, factory reset). The BOX then
shows up on the PC as a USB drive named "ESP-BOX" holding `CONFIG.INI`, `PERSONA.TXT`, `MEMORY.TXT`,
`AGENTS.INI`, `VOICE_FR.TXT`, `VOICE_EN.TXT`, `LOG.TXT` after a model test and `CONFIG.BKP` after a
factory reset. Edit them, save, wait a few seconds, then press **Restart** on the screen.

`components/esp_tinyuf2` is a copy of Espressif's `esp_tinyuf2` 1.2.0 component, extended for these files
(comments kept, files that can grow, files of SPIFFS); firmware updates through the drive are not used.

The default values of a new `CONFIG.INI` come from the assistant's `sdkconfig` (`main/Kconfig.projbuild`): build
the assistant first, then this project.

## Build and flash

```bash
idf.py build
python -m esptool --chip esp32s3 -p COM3 write_flash 0x700000 build/factory_nvs.bin
```

`idf.py flash` in the assistant's folder also writes it. See the [main README](../README.md) for the keys of
`CONFIG.INI` and the files of the drive.
