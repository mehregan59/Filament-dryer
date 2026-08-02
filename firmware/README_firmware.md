# Firmware setup — SpacePi Dryer Mod v01 Mike

---

## Which firmware file to use

| File | When to use |
|---|---|
| `SpacePi_Dryer.ino` | Standard version — USB upload only |
| `SpacePi_Dryer_OTA.ino` | **Recommended** — includes wireless OTA update support |

**The recommended workflow:**
1. Flash `SpacePi_Dryer_OTA.ino` **once via USB** (follow steps below)
2. Every future update: upload wirelessly from Arduino IDE over WiFi — no cable ever again
3. Develop and iterate your own changes without opening the enclosure

---

## 1. Arduino IDE setup

- Download Arduino IDE 2.x from arduino.cc
- Tools → Boards Manager → search **esp32** → install **esp32 by Espressif Systems**
  ⚠️ Do NOT install "Arduino ESP32 Boards" — that adds the Nano ESP32 and causes DFU upload errors
- Board: Tools → Board → esp32 → **ESP32 Dev Module**
- Upload speed: Tools → Upload Speed → **115200**
  (higher speeds cause "Serial data stream stopped" errors on the CH340 USB chip)
- Partition scheme: Tools → Partition Scheme → **Minimal SPIFFS (1.9MB APP)**
  This gives the largest possible space for the firmware — required for OTA to work

## 2. Libraries

Sketch → Include Library → Manage Libraries — install:
- **TFT_eSPI** by Bodmer
- **Adafruit SHT4x** (accept all dependency installs: BusIO, Unified Sensor)

`ArduinoOTA` is built into the ESP32 core — no extra install needed.

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

If you see errors about `spiFrequencyToClockDiv` or `Wire.h` precompiled library not found:
delete folders named **Wire** and **SPI** from `Documents/Arduino/libraries/`.
Keep TFT_eSPI, Adafruit SHT4x, etc. Recompile.

## 5. Edit the sketch before flashing

Open `SpacePi_Dryer_OTA.ino`. Near the top, set your credentials:

```cpp
const char* WIFI_SSID    = "YOUR_WIFI_NAME";
const char* WIFI_PASS    = "YOUR_WIFI_PASSWORD";
const char* OTA_HOSTNAME = "spacepi-dryer";   // name shown in Arduino IDE port list
const char* OTA_PASSWORD = "dryer2024";       // ← CHANGE THIS before first flash
```

You can also leave WIFI_SSID/PASS blank and configure WiFi from the device Settings screen
after the first boot — but OTA requires WiFi to be connected to work.

---

## 6. First flash — USB only (one time)

> This is the only time you need a USB cable. After this, all updates are wireless.

1. Connect the board via the **UART0** USB-C port (data cable, not a charge-only cable)
2. Tools → Port → select the COMx / ttyUSB0 port that appears when you plug in
3. Click Upload (→ arrow)
4. If upload hangs at `Connecting........`:
   hold **BOOT**, tap **RESET**, release BOOT — then it flashes
5. **First boot:** touch calibration screen appears automatically — touch all four corner
   arrows firmly with the stylus. Stored in flash, never asked again.
6. Home screen appears. Go to Settings — confirm WiFi connected and IP shown.
7. Settings screen shows: `OTA: spacepi-dryer  pwd: dryer2024` in green = ready

---

## 7. All future updates — wireless OTA

Once the OTA firmware is running and the dryer is on your WiFi:

1. Open Arduino IDE on a computer on **the same WiFi network**
2. Tools → Port → look in the **Network ports** section for **spacepi-dryer**
   (may take 10–15 seconds to appear after the dryer boots)
3. Select **spacepi-dryer**
4. Click Upload — Arduino IDE asks for the OTA password: **`dryer2024`**
   (or whatever you set as `OTA_PASSWORD`)
5. The dryer display shows a progress bar while uploading
6. Device reboots automatically when complete — calibration and settings are preserved

**If spacepi-dryer does not appear in the port list:**
- Confirm dryer is on WiFi (Settings screen shows green IP)
- Confirm your computer is on the same WiFi network (not ethernet + WiFi separately routed)
- Restart Arduino IDE
- Try uploading once more — mDNS discovery occasionally needs a second attempt

**OTA safety:** the heater and fan are forced OFF as the very first action when an OTA
upload begins — before any other code runs. Do not start an OTA upload during a drying
run. Finish or stop the run first.

---

## 8. Pin mapping

| Signal | ESP32 pin |
|---|---|
| SSR input + (heater) | IO25 |
| IRLZ44N gate (fans) | IO32 |
| SHT40 SDA | IO22 |
| SHT40 SCL | IO21 |
| TFT backlight | IO27 |

## 9. First powered hardware test (heater DISCONNECTED from SSR)

1. Set buck converter to exactly 5.0V with a multimeter before connecting the ESP32
2. Flash firmware via USB
3. Verify: home screen appears, sensor reads temp/humidity, touch works
4. Settings screen → IP shown in green = WiFi connected, OTA ready
5. Press START on a short 30-min timer:
   - SSR input LED must light (heater control signal active)
   - Fans must spin
6. Only after all checks pass: connect heater wire to SSR output terminal
7. Supervise the entire first heated run with an independent thermometer in the chamber

## 10. Touch calibration

Settings → CALIBRATE. Press firmly at each corner arrow with the stylus.
Calibration is stored in flash. Redo after physically re-mounting the board
(resistive touch panels shift slightly under new mounting pressure).

## 11. Screen rotation

Default is `setRotation(3)`. If image appears upside-down, change to `1` in `setup()`.

## 12. Developing your own changes

After the first USB flash you can iterate entirely wirelessly:

1. Make your code changes in Arduino IDE
2. Tools → Port → select **spacepi-dryer** (network port)
3. Upload — enter password `dryer2024` when prompted
4. Test — the device reboots with your new code in ~30 seconds

No screwdriver, no USB cable, no opening the enclosure. The dryer becomes a wireless
development target you can update from anywhere in the house.
