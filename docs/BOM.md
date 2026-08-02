# Bill of Materials

SpacePi Filament Dryer — Mod v01 Mike

All prices approximate at time of build (mid-2025, Europe).

## Essential electronics

| Qty | Part | Specification | Notes |
|---|---|---|---|
| 1 | Elecrow WZ3248R035 | ESP32-WROOM-32-N4, 3.5" ILI9488 480×320, resistive touch XPT2046 | Controller + display. Search: "Elecrow Wizee ESP32 3.5" |
| 1 | Solid state relay | SSR-40DA, input 3–32V DC, output 24–380V AC, 40A | SSR-40DA with plastic terminal cover. DO NOT use mechanical relay — triac-style SSR only |
| 1 | PSU module | 220V AC → 12V DC, ≥2A, enclosed | Mean Well RS-25-12 recommended. HLK-style clones also work |
| 1 | Buck converter | 12V → 5V, ≥1A, adjustable | MP1584EN or LM2596 module. Set to 5.0V before connecting ESP32 |
| 1 | MOSFET | IRLZ44N, TO-220 | Logic-level gate, 55V/47A, fully on at 3.3V. For switching 12V fans |
| 1 | SHT40 breakout | Adafruit SHT40 or equivalent | Rated to 125 °C. I2C. DO NOT use AM2301/DHT21 — max 80 °C, fails at target temp |

## Safety components — non-negotiable

| Qty | Part | Specification | Notes |
|---|---|---|---|
| 1 | Thermal fuse | SEFUSE SF133E, 133 °C, 10A, 250V | Mount clamped against heater body. 99 °C versions will nuisance-trip. Buy SF133E specifically |
| 1 | Inline fuse holder | 250V rated, panel or inline style | For 2A fuse in heater Live wire |
| 2 | Fast-blow fuse | 2A, 250V | One installed, one spare. Heater draws ~1.5A at 220V |
| — | Non-insulated copper butt splices | 0.75–1.5mm² bore | For thermal fuse connections. PVC-insulated versions soften at 85 °C — use bare copper only |
| — | 200 °C silicone heat-shrink | 3–5mm diameter | Over fuse body and bare crimp joints in hot zone |
| — | Crimp tool | Non-insulated terminal type | Pliers-crimped butt joints loosen with thermal cycling — proper tool required |

## Wiring and connectors

| Qty | Part | Specification | Notes |
|---|---|---|---|
| 2m | Silicone wire | 0.75–1mm² (18AWG), various colours | For all mains-side and hot-zone runs. PVC wire max 80–105 °C — do not substitute |
| 1m | Silicone wire | 24–28AWG | For signal and sensor runs |
| 1 pack | Wago 221 connectors | 3-way, ≥3 pieces | For mains L and N junction points — screwless, re-openable, rated 32A/450V |
| 1 pack | Crimp ferrules | 0.5–1.5mm² assortment | For every stranded wire entering a screw terminal |
| — | Kapton tape or silicone mount | High-temp | For mounting SHT40 inside the drying chamber |
| — | Cable ties, heat-shrink assortment | — | General dressing |

## MOSFET gate components

| Qty | Part | Value | Notes |
|---|---|---|---|
| 1 | Resistor | 100 Ω, 1/4W | In series between IO32 and MOSFET gate |
| 1 | Resistor | 10 kΩ, 1/4W | Gate to GND pulldown — keeps fans off during ESP32 boot |

## Tools required

- Multimeter (essential for verifying buck output before connecting ESP32)
- Soldering iron + solder
- Crimping tool for non-insulated ferrules and butt splices
- Heat gun for silicone sleeve
- Screwdrivers (Philips and flat)
- Wire strippers

## What we deliberately did NOT use

| Rejected part | Reason |
|---|---|
| AM2301 / DHT21 sensor | Rated to 80 °C max — fails at target temperature |
| 24V relay module (SRD-24VDC) | Coil requires 24V; only 12V and 5V available |
| 99 °C thermal fuse | Too close to operating temp — nuisance trips during normal 85 °C runs |
| Mechanical relay for heater | Contact arcing under thermostat-rate switching; SSR is superior |
| Stock Creality board | Firmware-locked to 70 °C; no accessible control interface |
| PVC-insulated butt connectors | PVC softens at 80–85 °C — unsafe in the hot zone |
| U-shaped open crimp terminals | Open barrel — strands migrate under thermal cycling; not suitable for mains |
