# SpacePi Filament Dryer — Mod v01 Mike

A complete ESP32-based controller mod for the Creality Space Pi filament dryer, replacing the stock electronics with a custom build capable of reaching 85 °C, offering full WiFi control, a responsive web app, and wireless OTA firmware updates.

---

## ⚠️ Disclaimer

**This project involves mains voltage (220V/110V AC) which can cause serious injury or death if handled incorrectly. By using, building, or modifying anything in this repository you accept full responsibility for your own safety and the safety of others.**

- This is a hobbyist project shared for educational purposes only
- The author(s) accept no liability for damage to property, injury, or death resulting from use of this information
- This modification voids the warranty of your Creality Space Pi dryer
- Always disconnect mains power before working on the electronics
- Have your wiring inspected by a qualified electrician if you are not confident
- Never leave a modified appliance unattended while operating
- The thermal fuse and firmware safety limits are not a substitute for safe working practices

**Build and use entirely at your own risk.**

---

## Live diagrams

> Enable **GitHub Pages** (Settings → Pages → Source: main / root) to make these links work.
> Or paste any URL into **https://htmlpreview.github.io/** to preview without Pages.
>
> 🌐 https://mehregan59.github.io/Filament-dryer/

| Resource | Link |
|---|---|
| Complete system wiring | [View diagram](https://mehregan59.github.io/Filament-dryer/wiring/wiring_complete.html) |
| IRLZ44N MOSFET pin detail | [View diagram](https://mehregan59.github.io/Filament-dryer/wiring/mosfet_detail.html) |
| Web control app | [Open app](https://mehregan59.github.io/Filament-dryer/web/dryer_webapp.html) |

---

## Quick start — flash order

### Step 1 — USB flash (one time only)

Flash `firmware/SpacePi_Dryer_OTA.ino` via USB cable following the full guide in
[firmware/README_firmware.md](firmware/README_firmware.md).

This is the **only time you ever need a USB cable.**

Before flashing, edit these lines at the top of the sketch:
```cpp
const char* WIFI_SSID    = "your-network-name";
const char* WIFI_PASS    = "your-network-password";
const char* OTA_PASSWORD = "dryer2024";   // change to something unique
```

### Step 2 — all future updates via WiFi (OTA)

Once the OTA firmware is running and the dryer is connected to your WiFi:

1. Open Arduino IDE on any computer on the **same WiFi network**
2. Tools → Port → select **spacepi-dryer** under Network ports
3. Click Upload — enter your OTA password when prompted (`dryer2024` by default)
4. The dryer screen shows a progress bar, then reboots automatically

No screwdriver. No USB cable. No opening the enclosure.
Develop and iterate your own code changes entirely wirelessly.

> **OTA safety:** the heater and fan are hardware-forced OFF as the very first action
> of any OTA upload — before any other code runs. Still, finish or stop any active
> drying run before starting an OTA update.

See the full OTA workflow in [firmware/README_firmware.md](firmware/README_firmware.md).

---

## Why we built this

The stock Creality Space Pi tops out at 70 °C and throws an E4 error near its ceiling.
PA/Nylon needs 80 °C, PC needs 85 °C — neither achievable stock.
Rather than sensor-offset hacks that make the display lie, we replaced the entire control
side with an ESP32, an SSR-40DA, and a precision SHT40 sensor.

- True, displayed, tunable temperature up to 85 °C
- Flip-clock idle screen with constellation animation
- Animated touch UI — flame heater icon, dual spinning fans, status badges
- WiFi with NTP clock, on-screen network picker, web app for phone/tablet/desktop
- Three independent safety layers: 92 °C firmware ceiling, thermal runaway detection, physical thermal fuse
- Run history and fault log with bar chart in the web app
- Wireless OTA updates — develop from anywhere in the house

---

## Repository structure

```
firmware/
  SpacePi_Dryer_OTA.ino   # ← USE THIS — includes wireless OTA update support
  SpacePi_Dryer.ino       # Standard version (USB only, no OTA)
  README_firmware.md      # Full setup guide, pin config, OTA workflow
web/
  dryer_webapp.html       # Responsive web app — phone, tablet, desktop
wiring/
  wiring_complete.html    # Full colour-coded system wiring diagram
  mosfet_detail.html      # IRLZ44N pin detail
docs/
  BOM.md                  # Complete bill of materials
  SAFETY.md               # Three-layer safety architecture rationale
index.html                # GitHub Pages landing page
media/                    # Add your photos, videos, 3D files here
```

---

## Hardware

See [docs/BOM.md](docs/BOM.md) for the complete list including rejected parts and reasoning.

| Part | Role |
|---|---|
| Elecrow WZ3248R035 (ESP32-WROOM-32, 3.5" ILI9488 touch) | Controller + display |
| SSR-40DA solid state relay | Switches 220V heater — opto-isolated |
| Mean Well RS-25-12 or equivalent | 220V → 12V PSU |
| MP1584 or LM2596 buck module | 12V → 5V for ESP32 |
| IRLZ44N MOSFET + 100Ω + 10kΩ | Switches 12V fans from IO32 |
| Adafruit SHT40 breakout | Temp + humidity, rated to 125 °C |
| SEFUSE SF133E (133 °C, 10A, 250V) | Thermal fuse — hardware last resort |
| 2A 250V inline fuse + holder | Heater Live wire protection |
| Silicone wire 0.75–1mm², 24–28AWG | All wiring in hot zone |
| Crimp ferrules + Wago 221 connectors | Mains terminations |
| Non-insulated copper butt splices + 200 °C silicone sleeve | Thermal fuse connections |

---

## Web app — connecting from phone/tablet

Open `http://[dryer-IP]/` directly in your phone browser (same WiFi).
The ESP32 serves the built-in control page — no CORS issues, no file needed.

For the full web app with history graphs, serve it locally:
```bash
python3 -m http.server 8080
# then open http://[computer-IP]:8080/dryer_webapp.html on your phone
```

---

## Safety

Three independent layers — each works when the others fail:

1. **Firmware** — 92 °C hard ceiling (E2), thermal runaway after 4 min (E3), sensor loss (E1).
   Fault reset gated by confirmed cool-down below 50 °C.
2. **2A inline fuse** — heater Live wire. Independent of all software.
3. **133 °C thermal fuse (SF133E)** — clamped to heater body, physics-only trip.
   Crimped with bare copper sleeves and 200 °C silicone sleeve — never soldered.

See [docs/SAFETY.md](docs/SAFETY.md) for full rationale and the SSR-welded-shut failure scenario.

---

## Filament presets

| Filament | Target | Duration |
|---|---|---|
| PLA | 50 °C | 6 h |
| PETG | 60 °C | 6 h |
| ABS | 70 °C | 8 h |
| ASA | 70 °C | 8 h |
| PA / Nylon | 80 °C | 12 h |
| PC | 85 °C | 10 h |

All adjustable ±5 °C and ±30 min from touch screen or web app.

---

## Licence

MIT — see LICENSE. Build it, modify it, share it.
**Use entirely at your own risk — see disclaimer above.**
