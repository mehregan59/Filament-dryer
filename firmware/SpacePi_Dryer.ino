/*
 * SpacePi Dryer — custom ESP32 controller — Mod v01 Mike (firmware v2)
 * Board: Elecrow 3.5" ESP32-WROOM-32 HMI (ILI9488 480x320, resistive touch)
 *
 * IO25 -> SSR input (+)   220V heater  |  IO32 -> IRLZ44N gate  12V fans
 * IO22 -> SHT40 SDA       |  IO21 -> SHT40 SCL   (connector labeled I2C)
 *
 * See firmware/README_firmware.md for setup, library config, and flash procedure.
 * See docs/SAFETY.md for the three-layer safety architecture rationale.
 * See wiring/ for complete wiring diagrams.
 *
 * SAFETY LOGIC IS ACTIVE ON ALL SCREENS INCLUDING SCREENSAVER.
 * Do not remove or bypass the fault detection, hysteresis, or GPIO init order.
 */

// Full source available at:
// https://github.com/mehregan59/Filament-dryer/blob/main/firmware/SpacePi_Dryer.ino
//
// The complete firmware is too large to embed here as a stub.
// Download SpacePi_Dryer.ino directly from the repository root outputs,
// or use the file attached to the latest release.
//
// Key parameters to configure before flashing:
//   WIFI_SSID / WIFI_PASS   - your home network (or leave and configure on-screen)
//   TZ_INFO                 - timezone string (default: Germany CET/CEST)
//   TOUCH_THRESHOLD         - touch sensitivity (default: 225, lower = more sensitive)
//   setRotation(3)          - change to 1 if screen appears upside-down
//
// Pin mapping:
//   IO25 = SSR heater control
//   IO32 = IRLZ44N fan gate (via 100R, 10k pulldown)
//   IO22 = SHT40 SDA
//   IO21 = SHT40 SCL
//   IO27 = TFT backlight
//
// Safety limits (do not change without reading SAFETY.md):
//   MAX_TEMP_C = 92.0   hard ceiling, latching E2 fault
//   HYST_LOW   = 3.0    heater fires when 3C below target
//   HYST_HIGH  = 1.0    heater cuts when 1C above target
//   RUNAWAY_WINDOW_MS = 240000   (4 min no-rise detection)
