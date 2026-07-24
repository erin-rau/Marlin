# Machine Firmware — Ender 3 V2 + Sprite Pro + CR Touch

**This branch (`codex/modify-marlin-firmware-for-ender-3-v2`) is THE authoritative
Marlin firmware configuration for this printer.** It is the single source of truth
for the hardware described below. Any other config branch is obsolete — build and
flash from here.

The values in this document are pulled directly from `Marlin/Configuration.h` and
`Marlin/Configuration_adv.h` on this branch. Do not edit those files to match this
document — this document is generated from them.

## Hardware summary

| Area | Value | Config source |
|------|-------|---------------|
| Printer | Creality Ender 3 V2 | — |
| Extruder | Creality Sprite Pro (direct drive) | — |
| Probe | CR Touch (BLTOUCH-compatible) | `BLTOUCH` |
| Mainboard | `BOARD_CREALITY_V422` (Creality 4.2.2, 32-bit) | `MOTHERBOARD` |
| PlatformIO build env | `STM32F103RC_creality` | `platformio.ini` → `default_envs` |
| Stepper drivers | TMC2208 (standalone / no UART) on X, Y, Z, E0 | `*_DRIVER_TYPE` |

## Motion & extruder

| Setting | Value | Config source |
|---------|-------|---------------|
| Steps/mm (X, Y, Z, E) | `{ 80, 80, 400, 424.9 }` | `DEFAULT_AXIS_STEPS_PER_UNIT` |
| Extruder steps/mm (E) | **424.9** (Sprite Pro) | same |
| Bed size (X × Y) | 230 × 230 mm | `X_BED_SIZE` / `Y_BED_SIZE` |
| Max Z height | 240 mm | `Z_MAX_POS` |

## CR Touch probe & bed leveling

| Setting | Value | Config source |
|---------|-------|---------------|
| Probe type | CR Touch via `BLTOUCH` | `BLTOUCH` |
| Nozzle-to-probe offset (X, Y, Z) | `{ -37, -39, -3.25 }` | `NOZZLE_TO_PROBE_OFFSET` |
| Bed-leveling type | Bilinear ABL | `AUTO_BED_LEVELING_BILINEAR` |
| Probe grid | 9 × 9 points | `GRID_MAX_POINTS_X` / `_Y` |

The `-3.25` Z entry in `NOZZLE_TO_PROBE_OFFSET` is a **placeholder** — see
"Calibrate on the machine" below.

## Display / UI

| Setting | Value | Config source |
|---------|-------|---------------|
| UI | MarlinUI on the DWIN LCD | `DWIN_MARLINUI_PORTRAIT` |
| Orientation | Portrait | `DWIN_MARLINUI_PORTRAIT` |

## Enabled conveniences

| Feature | Enabled by | Notes |
|---------|-----------|-------|
| Babystepping | `BABYSTEPPING` | Live Z-offset nudging while printing |
| Filament change (M600) | `ADVANCED_PAUSE_FEATURE` + `NOZZLE_PARK_FEATURE` | Guided load/unload & mid-print swap |
| Persistent settings | `EEPROM_SETTINGS` | `M500` to save, `M501` to load |

## Committed temperature PID values

These are the values currently compiled into the firmware:

| Loop | Kp | Ki | Kd | Config source |
|------|----|----|----|---------------|
| Bed  | 462.10 | 85.47 | 624.59 | `DEFAULT_bedKp/Ki/Kd` |
| Hotend | 22.20 | 1.08 | 114.00 | `DEFAULT_Kp/Ki/Kd` |

> ⚠️ **The committed bed PID values are NOT a real autotune result for this
> machine.** They are placeholders. Re-tune on the actual printer (see below)
> before trusting bed temperature stability.

## Calibrate on the machine

Two things in this firmware **must be verified/re-tuned on the physical printer**
before it is trustworthy. Do not treat the committed values as final.

1. **Probe Z-offset.** The `-3.25` mm Z value in `NOZZLE_TO_PROBE_OFFSET` is a
   placeholder, not a measured value for this CR Touch mounting. Measure the real
   nozzle-to-probe Z-offset on the machine (e.g. paper/feeler-gauge method or the
   probe Z-offset wizard), set it, then save with `M500`. Use babystepping during
   a first-layer test to fine-tune, then persist with `M500`.

2. **Bed & hotend PID.** The committed PID values are placeholders, not a real
   autotune for this hardware. Run PID autotune on the machine and save the
   results:

   ```gcode
   ; Bed PID autotune (example: 60 °C, 8 cycles)
   M303 E-1 S60 C8 U1
   ; Hotend PID autotune (example: 200 °C, 8 cycles)
   M303 E0 S200 C8 U1
   ; Persist the tuned values to EEPROM
   M500
   ```

   `U1` applies the tuned values immediately; `M500` writes them to EEPROM.
   Re-verify after any change to the bed surface, thermistor, or hotend hardware.
