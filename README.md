# SpacePi Filament Dryer — Mod v01 Mike

A complete ESP32-based controller mod for the Creality Space Pi filament dryer, replacing the stock electronics with a custom build capable of reaching 85 °C, offering full WiFi control, a responsive web app, and safety features the stock firmware lacks.

---

## Live diagrams

> **Enable GitHub Pages** (Settings → Pages → Source: main / root) to make these links work.  
> Or paste any URL into **https://htmlpreview.github.io/** to preview HTML files without Pages.
>
>  https://mehregan59.github.io/Filament-dryer/

| Diagram | Link |
|---|---|
| Complete system wiring | [View diagram](https://mehregan59.github.io/Filament-dryer/wiring/wiring_complete.html) |
| IRLZ44N MOSFET pin detail | [View diagram](https://mehregan59.github.io/Filament-dryer/wiring/mosfet_detail.html) |
| Web control app | [Open app](https://mehregan59.github.io/Filament-dryer/web/dryer_webapp.html) |

**To enable GitHub Pages in 60 seconds:**  
Repo → Settings → Pages → Source → Deploy from branch → Branch: main → Folder: / (root) → Save.  
Wait ~2 min, then the links above will work for anyone.

---

## Why we built this

The stock Creality Space Pi is limited to 70 °C by firmware and develops an E4 error (timeout: setpoint not reached) near its ceiling. The stock board uses a triac with no accessible firmware and a single NTC sensor. For drying Nylon (PA) or PC filament you need 80–85 °C — not achievable with the stock system.

Rather than sensor-offset hacks (which make the display lie), we replaced the entire control side with an ESP32 display board, an SSR, and a precision SHT40 sensor. The result:

- True, displayed, tunable temperature up to 85 °C
- Animated touch UI with presets, screensaver, idle clock with constellation animation
- WiFi with NTP clock, on-screen network picker, and a full mobile/desktop web app
- Three independent safety layers: firmware ceiling (92 °C), thermal runaway detection, physical thermal fuse
- Full remote control from any phone browser on the same WiFi network

---

## Repository structure

```
firmware/
  SpacePi_Dryer.ino        # Main ESP32 firmware (Arduino IDE)
  README_firmware.md       # Library setup, TFT_eSPI User_Setup.h pin block, flash procedure
web/
  dryer_webapp.html        # Responsive web app — open in any browser, no install
wiring/
  wiring_complete.html     # Full system wiring diagram (interactive, open in browser)
  mosfet_detail.html       # IRLZ44N pin detail diagram
docs/
  BOM.md                   # Bill of materials with part numbers and suppliers
  SAFETY.md                # Safety design rationale
media/                     # Photos, videos, 3D files (add your own here)
```

---

## Hardware

See [docs/BOM.md](docs/BOM.md) for the complete part list. Summary:

| Part | Role |
|---|---|
| Elecrow WZ3248R035 (ESP32-WROOM-32, 3.5" ILI9488 touch) | Controller + display |
| SSR-40DA solid state relay | Switches 220V heater |
| Mean Well RS-25-12 or equivalent | 220V → 12V PSU |
| MP1584 or LM2596 buck module | 12V → 5V for ESP32 |
| IRLZ44N MOSFET + 100Ω + 10kΩ | Switches 12V fans |
| Adafruit SHT40 breakout | Temperature + humidity sensor |
| SEFUSE SF133E (133 °C, 10A, 250V) | Thermal fuse — last-resort safety |
| 2A 250V inline fuse + holder | Heater Live wire protection |
| Silicone wire 0.75–1mm², 24–28AWG | All wiring |
| Crimp ferrules, Wago 221 connectors | Mains terminations |
| Non-insulated copper butt splices | Thermal fuse connections |
| 200 °C silicone heat-shrink sleeve | Over thermal fuse body |

---

## Wiring

The wiring diagrams are colour-coded HTML/SVG files — open them in any browser.

**Wire colours in the diagram:**
- 🔴 Red — 220V mains Live path
- 🟡 Yellow — 12V DC
- 🔵 Blue — 5V DC
- 🟢 Green — control signals (IO25 heater, IO32 fan gate)
- ⚫ Dark grey — common ground

**Mains path (220V):**
Inlet Live → 2A fuse → Wago (splits to PSU-L and SSR output-1) → SSR output-2 → thermal fuse → heater wire 1. Heater wire 2 → Neutral Wago → PSU-N → back to inlet Neutral.

**DC path:**
PSU 12V+ → fans+ (direct, always on) and buck IN+. Buck OUT 5V → ESP32 PWR. All grounds common: PSU−, buck−, ESP32 GND, MOSFET source, SSR input−.

**Control signals:**
- ESP32 IO25 → SSR input+ (heater, opto-isolated)
- ESP32 IO32 → 100Ω → MOSFET gate (fans); 10kΩ gate to GND
- ESP32 IO22 SDA, IO21 SCL → SHT40 I2C connector

**IRLZ44N pinout** (label facing you, legs pointing down): Gate = left, Drain = middle/tab, Source = right.

---

## Firmware setup

See [firmware/README_firmware.md](firmware/README_firmware.md) for full instructions. Quick start:

1. Install Arduino IDE 2.x, board: **ESP32 Dev Module** (Espressif core, not Arduino's Nano ESP32).
2. Libraries: **TFT_eSPI** (Bodmer), **Adafruit SHT4x** (+ its dependencies).
3. Edit `Documents/Arduino/libraries/TFT_eSPI/User_Setup.h` — use the pin block in README_firmware.md.
4. Edit `WIFI_SSID` / `WIFI_PASS` in the sketch, or configure WiFi from the device's Settings screen.
5. Upload at **115200 baud** (Tools → Upload Speed). Hold BOOT + tap RESET if upload hangs at `Connecting...`.
6. First boot: touch calibration runs automatically. Repeat via Settings → CALIBRATE after mounting.

---

## Web app

Open `web/dryer_webapp.html` in any browser, or use the [live link](https://mehregan59.github.io/Filament-dryer/web/dryer_webapp.html) if GitHub Pages is enabled. Go to Settings, enter the IP shown on the dryer's screen, tap Connect. The app polls every 2 seconds over local WiFi — no internet required, no install.

Features: live temp/humidity, countdown timer with progress bar, animated heater/fan status, 20-minute history chart, full preset and parameter control, fault reset.

---

## Safety design

Three independent layers — each works even when the others fail:

1. **Firmware:** 92 °C hard cutoff (E2), thermal runaway detection after 4 min no rise (E3), sensor-loss fault (E1). Fault reset gated by confirmed cool-down.
2. **2A inline fuse:** protects the heater Live wire against overcurrent. Independent of all software.
3. **133 °C thermal fuse (SF133E):** clamped to the heater body in series with Live. Trips on physics alone if the SSR welds shut. Installed with non-insulated copper butt crimps and 200 °C silicone sleeve — never soldered.

See [docs/SAFETY.md](docs/SAFETY.md) for the full rationale.

---

## Adding your own media

Add photos, videos, and 3D files to the `media/` folder:

```
media/
  photos/      # Build photos, finished unit
  videos/      # Demo videos
  3d/          # STL/STEP files for LCD cover and case replacement
```

---

## Filament presets

| Filament | Target temp | Duration |
|---|---|---|
| PLA | 50 °C | 6 h |
| PETG | 60 °C | 6 h |
| ABS | 70 °C | 8 h |
| ASA | 70 °C | 8 h |
| PA / Nylon | 80 °C | 12 h |
| PC | 85 °C | 10 h |

All presets are adjustable ±5 °C and ±30 min from the touch screen or web app.

---

## Licence

MIT — see LICENSE file. Build it, modify it, share it. If this helped you, a star on the repo is appreciated.
