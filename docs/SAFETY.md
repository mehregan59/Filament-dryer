# Safety design — SpacePi Dryer Mod v01 Mike

This document explains the three-layer safety architecture and the reasoning behind each decision.

---

## Why three layers?

Every layer protects against a failure mode the other layers cannot catch. The layers are deliberately independent — no single component failure disables more than one of them.

### Layer 1 — Firmware (ESP32)

**What it catches:** Normal overtemperature, sensor loss, thermal runaway (heater on but no temperature rise).

**How it works:**
- `MAX_TEMP_C = 92.0` — hard ceiling; any reading at or above this triggers E2 regardless of setpoint.
- E3 — thermal runaway: if the heater has been continuously on for 4 minutes while more than 5 °C below target and the temperature has not risen by 2 °C, something is wrong (heater dead, sensor fell off, SSR intermittent). Latching fault.
- E1 — sensor loss: 8 consecutive failed I2C reads triggers fault. Without a thermometer the machine cannot be trusted to heat safely.
- Fault reset is gated: temperature must be confirmed below 50 °C (or 10 minutes must have elapsed after a sensor fault) before the reset button appears. This forces a cool-down period before any restart.
- All GPIO outputs are driven LOW as the very first lines of `setup()` — even a partial flash or watchdog reset leaves the heater off.

**What it cannot catch:** SSR welded shut. If the SSR fails shorted, IO25 going LOW does nothing downstream — the heater keeps running regardless of what the firmware commands.

### Layer 2 — 2A inline fuse

**What it catches:** Overcurrent in the heater Live wire (wiring fault, SSR internal short to another circuit, etc.)

**What it cannot catch:** The nominal heater load (~1.5A) is below the fuse rating. An SSR welded shut still delivers normal heater current — the fuse does not see an overcurrent condition and does not trip.

### Layer 3 — 133 °C thermal fuse (SEFUSE SF133E)

**What it catches:** Chamber temperature exceeding 133 °C — the physical condition caused by SSR welded shut with the fan dead or inadequate. No software involved. The fuse melts and opens the circuit permanently.

**Why 133 °C and not lower?**
The heater's PTC element body runs considerably hotter than the chamber air during normal operation — that is how it transfers heat. A 99 °C fuse clamped to the heater body will experience temperatures above its rating during normal 85 °C drying and trip nuisance-style. 133 °C provides margin above normal operating conditions while remaining well below the enclosure plastic's deformation temperature (~160–180 °C for most ABS).

**Why clamped to the heater body, not floating in air?**
A fuse floating in the airstream responds to air temperature, not heater temperature. In the failure scenario (SSR welded, fan also failed), the heater body exceeds 133 °C long before the chamber air does. Clamping to the metal heater frame gives the fastest response to the correct failure.

**Why non-insulated crimp, not solder?**
Soldering iron heat travels up the fuse leads and can trip the fuse during installation. A properly crimped non-insulated copper butt splice makes a cold-weld joint without heat. The joint is then sleeved in 200 °C silicone heat-shrink, which is rated above the fuse's own trip temperature.

---

## What happens in the SSR-welded-shut scenario

1. SSR fails shorted. Heater runs continuously regardless of IO25 state.
2. Firmware sees temperature climbing past setpoint → cuts heater command → no effect.
3. Firmware sees temperature pass 92 °C → E2 fault, fans commanded on → fans run but heater does not stop.
4. Temperature continues climbing.
5. At ~133 °C the thermal fuse opens. Circuit is physically broken. Heater stops. Fans continue (fan circuit is separate).

Without the thermal fuse, step 5 does not occur, and the only remaining limits are the enclosure materials and the upstream circuit breaker.

---

## Control loop design — why asymmetric hysteresis?

The heater uses asymmetric hysteresis: fires at (target − 3 °C), cuts at (target + 1 °C). A PTC element overshoots on the warm side because its thermal mass continues releasing heat after the electrical power is cut. A tight symmetric band (±1 °C) causes rapid cycling — the heater fires, overshoots, cuts, undershoots, fires again, every minute or two. The display shows oscillation and the SSR accumulates switch cycles.

With HYST_LOW = 3 and HYST_HIGH = 1, the heater fires when meaningfully below target and cuts early, allowing the PTC's residual heat to bring the chamber to target without overcutting. Switch cycles drop from ~20/hour to ~4/hour at steady state.

---

## Mains wiring rules

- Ferrules on every stranded wire entering a screw terminal. Bare strands in screw terminals loosen under vibration and thermal cycling.
- Wago 221 connectors for L and N junction points — rated 32A/450V, screwless, re-openable.
- Silicone wire throughout the hot zone (PVC insulation softens at 80–105 °C).
- Fuse holder in the heater Live wire — not in Neutral (switching Neutral while Live is connected leaves the downstream circuit energised).
- The SSR's mains output terminals are live when the dryer is plugged in, regardless of the control signal. Treat the entire board as live whenever the mains cable is connected.
