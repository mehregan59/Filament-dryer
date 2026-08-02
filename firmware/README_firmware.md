# Firmware setup — SpacePi Dryer Mod v01 Mike

## 1. Arduino IDE setup

- Download Arduino IDE 2.x from arduino.cc
- Tools → Boards Manager → search **esp32** → install **esp32 by Espressif Systems** (NOT "Arduino ESP32 Boards" — that installs the Nano ESP32 and causes DFU upload errors)
- Board: Tools → Board → esp32 → **ESP32 Dev Module**
- Upload speed: Tools → Upload Speed → **115200** (higher speeds cause Serial data stream errors on the CH340 USB chip)

## 2. Libraries

Sketch → Include Library → Manage Libraries:
- **TFT_eSPI** by Bodmer
- **Adafruit SHT4x** (accept all dependency installs: BusIO, Unified Sensor)

## 3. TFT_eSPI User_Setup.h — REQUIRED

Open `Documents/Arduino/libraries/TFT_eSPI/User_Setup.h` in any text editor.
Delete or comment everything, replace with:

```cpp
#define ILI9488_DRIVER
#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST  -1
#define TFT_BL   27
#define TFT_BACKLIGHT_ON HIGH
#define TOUCH_CS 33
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define SPI_FREQUENCY       27000000
#define SPI_TOUCH_FREQUENCY  2500000
```

Save. If you skip this step the screen stays white or black with no error message.

## 4. Common library conflict

If you see errors about `spiFrequencyToClockDiv` or `Wire.h` precompiled library not found, you have old core Wire/SPI libraries in `Documents/Arduino/libraries/`. Delete folders named **Wire** and **SPI** from there (keep TFT_eSPI, Adafruit SHT4x, etc.). Recompile.

## 5. Edit the sketch

Open `SpacePi_Dryer.ino`. At the top, optionally set:
```cpp
const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
```
Or leave them and configure WiFi from the device's Settings screen after flashing.

## 6. Flash procedure

1. Connect the board via the **UART0** USB-C port (data cable, not charge-only)
2. Tools → Port → select the new COMx / ttyUSB0 that appears
3. Click upload (→ arrow)
4. If it hangs at `Connecting........`: hold **BOOT**, tap **RESET**, release BOOT
5. First boot: the screen shows a touch calibration sequence — touch all four corner arrows with the stylus
6. After calibration the home screen appears

## 7. Pin mapping

| Signal | ESP32 pin |
|---|---|
| SSR input + (heater) | IO25 |
| IRLZ44N gate (fans) | IO32 |
| SHT40 SDA | IO22 |
| SHT40 SCL | IO21 |
| TFT backlight | IO27 |

## 8. First powered test (heater DISCONNECTED)

1. Set buck to 5.0V with multimeter before connecting ESP32
2. Flash firmware
3. Home screen appears, sensor reads, touch calibration done
4. Settings screen → verify IP shown if WiFi connected
5. Press START on a short timer; SSR input LED should light, fans should spin
6. Only after all checks pass: connect heater to SSR output, supervise first run

## 9. Touch calibration

If touch coordinates are offset: Settings → CALIBRATE. Press firmly at each corner arrow. Calibration is stored in flash and survives power cycles. Redo after re-mounting the board (resistive panels shift under mounting pressure).

## 10. Rotation

Default is `setRotation(3)`. If the image is upside-down, change to `1` in the sketch setup() function.
