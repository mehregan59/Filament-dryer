# SpacePi Filament Dryer — Mod v01 Mike

A complete ESP32-based controller mod for the Creality Space Pi filament dryer, replacing the stock electronics with a custom build capable of reaching 85 °C, offering full WiFi control, a responsive web app, and safety features the stock firmware lacks.

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

> **Enable GitHub Pages** (Settings → Pages → Source: main / root) to make these links work.
> Or paste any URL into **https://htmlpreview.github.io/** to preview HTML files without Pages.
>
> 🌐 https://mehregan59.github.io/Filament-dryer/

| Diagram | Link |
|---|---|
| Complete system wiring | [View diagram](https://mehregan59.github.io/Filament-dryer/wiring/wiring_complete.html) |
| IRLZ44N MOSFET pin detail | [View diagram](https://mehregan59.github.io/Filament-dryer/wiring/mosfet_detail.html) |
| Web control app | [Open app](https://mehregan59.github.io/Filament-dryer/web/dryer_webapp.html) |

---

## Why we built this

The stock Creality Space Pi is limited to 70 °C by firmware and develops an E4 error near its ceiling. For drying Nylon (PA) or PC filament you need 80–85 °C — not achievable with the stock system. Rather than sensor-offset hacks, we replaced the entire control side with an ESP32 display board, an SSR, and a precision SHT40 sensor.

- True, displayed, tunable temperature up to 85 °C
- Animated touch UI with presets, flip-clock idle screen, constellation animation
- WiFi with NTP clock, on-screen network picker, and a full mobile/desktop web app
- Three independent safety layers: firmware ceiling (92 °C), thermal runaway detection, physical thermal fuse
- Full remote control from any phone browser on the same WiFi network
- Run history and fault log with graphs in the web app

---

## Repository structure

```
firmware/
  SpacePi_Dryer.ino        # Main ESP32 firmware (Arduino IDE)
  README_firmware.md       # Library setup, TFT_eSPI pin block, flash procedure
web/
  dryer_webapp.html        # Responsive web app — open in any browser, no install
wiring/
  wiring_complete.html     # Full system wiring diagram
  mosfet_detail.html       # IRLZ44N pin detail diagram
docs/
  BOM.md                   # Bill of materials with part numbers and rejected parts
  SAFETY.md                # Three-layer safety architecture explained
media/                     # Add your photos, videos, 3D files here
```

---

## Hardware

See [docs/BOM.md](docs/BOM.md) for the complete part list.

| Part | Role |
|---|---|
| Elecrow WZ3248R035 (ESP32-WROOM-32, 3.5" ILI9488 touch) | Controller + display |
| SSR-40DA solid state relay | Switches 220V heater |
| Mean Well RS-25-12 or equivalent | 220V → 12V PSU |
| MP1584 or LM2596 buck module | 12V → 5V for ESP32 |
| IRLZ44N MOSFET + 100Ω + 10kΩ | Switches 12V fans |
| Adafruit SHT40 breakout | Temperature + humidity — rated to 125 °C |
| SEFUSE SF133E (133 °C, 10A, 250V) | Thermal fuse — hardware last resort |
| 2A 250V inline fuse + holder | Heater Live wire protection |
| Silicone wire 0.75–1mm², 24–28AWG | All wiring in hot zone |
| Crimp ferrules, Wago 221 connectors | Mains terminations |
| Non-insulated copper butt splices + 200 °C silicone sleeve | Thermal fuse connections |

---

## Web app — connecting from phone/tablet

Open `http://[dryer-IP]/` directly in your phone browser (same WiFi network). The ESP32 serves the built-in control page — no CORS issues, no file download needed.

For the full web app with history and graphs, download `web/dryer_webapp.html` and open it locally, or serve it from a computer on the same network:
```bash
python3 -m http.server 8080
# then open http://[your-computer-IP]:8080/dryer_webapp.html on your phone
```

---

## Safety design

Three independent layers — each works even when the others fail:

1. **Firmware:** 92 °C hard ceiling (E2), thermal runaway after 4 min no rise (E3), sensor-loss fault (E1). Fault reset gated by confirmed cool-down below 50 °C.
2. **2A inline fuse:** heater Live wire overcurrent protection. Independent of all software.
3. **133 °C thermal fuse (SF133E):** clamped to heater body. Trips on physics alone if SSR welds shut. Never soldered — crimped with non-insulated copper sleeves and 200 °C silicone shrink.

See [docs/SAFETY.md](docs/SAFETY.md) for full rationale.

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

All presets adjustable ±5 °C and ±30 min from touch screen or web app.

---

## Licence

MIT — see LICENSE file. Build it, modify it, share it. **Use entirely at your own risk** — see disclaimer above.
