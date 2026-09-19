# Pads Vending Machine — Coin Acceptor Prototype

Bricapa Technologies · Client: Rebecca Mpiima · Ref `BT-PADVEND-2026-001`

Proof-of-concept firmware. Its only job is to demonstrate that **a coin goes in
and a pad comes out.** Battery, sleep mode, cabinet and M-Pesa are deliberately
out of scope for this build.

---

## Hardware

| Part | Source | KSh |
|---|---|---|
| Multi-coin acceptor, 12 V, 6 coin types | ASK Electronics | 3,000 |
| ESP-32S ESP-WROOM-32 (Board Yellow) | ASK Electronics | 1,500 |
| I2C 1602 LCD, blue | ASK Electronics | 450 |
| KY-032 IR sensor | ASK Electronics | 350 |
| Passive buzzer module | ASK Electronics | 150 |
| LM2596 buck converter | ASK Electronics | 200 |
| Relay module, 1 channel, active-high | ASK Electronics | 200 |
| Jumper wire | ASK Electronics | 100 |
| 12 V 4 A adaptor | Town | ~1,000–1,500 |
| Geared motor, target 30 rpm | Town | ~1,000–1,500 |
| Schottky, flyback diode, 1000 µF | Stock | — |
| Coil, wound to pad pitch | Spring shop | TBC |

---

## Wiring

```
12 V adaptor ─┬─ LM2596 → 5.0 V → Schottky → ESP32 VIN
              │                        └─ ESP32 3V3 → KY-032 VCC
              ├─ coin acceptor (12 V)
              └─ relay COM/NO → motor  ── flyback diode across motor terminals
```

| Signal | GPIO | Notes |
|---|---|---|
| LCD SDA | 21 | |
| LCD SCL | 22 | |
| Coin pulse | 18 | `INPUT_PULLUP`, acceptor pulls LOW, interrupt on FALLING |
| Relay | 26 | active HIGH, **10 kΩ pull-down to GND** |
| IR sensor | 27 | powered from 3V3 |
| Buzzer | 25 | passive, driven by `tone()` |

Avoided: GPIO 0, 2, 5, 12, 15 (strapping), 1 and 3 (UART), 6–11 (flash).

### Three things that kill hardware

1. **Set the LM2596 to 5.0 V with a meter before the ESP32 is connected.**
   It ships at whatever the pot happens to be.
2. **Measure the coin acceptor's pulse output before it touches GPIO 18.**
   If it swings to 12 V, fit a divider or optocoupler. ESP32 pins are not 5 V
   tolerant, let alone 12 V.
3. **Fit the 10 kΩ pull-down on GPIO 26** so the motor stays still through boot
   and through every reset while flashing.

---

## Library

Install **LiquidCrystal I2C** (Frank de Brabander) via Library Manager.

If `tone()` fails to compile, your ESP32 core is old — either update it or
swap the `beep()` body for `ledcWriteTone()`.

---

## Bring-up order

Do these in sequence. Each one isolates a single failure.

**1. LCD.** Flash as-is. You should see `PAD DISPENSER / VENDS: 0`, then
`INSERT KSH 20`. Blank screen → change `LCD_I2C_ADDR` to `0x3F`, or run an I2C
scanner sketch to find it.

**2. Coin pulses.** Open Serial at 115200 and drop a KSh 20 coin. You want
exactly one `Coin pulse x1` per coin. If you get several, either raise
`COIN_DEBOUNCE_MS` or re-program the acceptor for one pulse per coin.

While you are here, put a **KSh 1, a KSh 5, a KSh 10 and a 26 mm hardware
washer** through it and confirm each is refused. That test is half the reason
this machine exists.

**3. IR sensor.** With the motor disconnected, watch whether a hand passing the
sensor is detected. If the logic is backwards, flip `IR_DETECTED_IS_LOW`.
If the machine faults straight to `CHECK SENSOR`, the sensor is reading "pad"
while idle — wrong polarity, wrong aim, or something in the chute.

*The KY-032 uses an NE555 and may be unreliable at 3.3 V. If it misbehaves,
move it to 5 V and put a 10 k / 20 k divider on its output — do not feed 5 V
into a GPIO.*

**4. Relay, motor disconnected.** Confirm it clicks once when credit is met and
again when the IR is triggered. Confirm it does **not** click on boot or reset.

**5. Motor, no coil.** Time one revolution. Set `DISPENSE_TIMEOUT_MS` to about
double that. Watch the 12 V rail on a meter while it starts — if it dips enough
to reset the ESP32, that is what the 1000 µF is for.

**6. Whole thing, with pads.** Fifty cycles minimum. Count how many actually
drop cleanly. That number is the answer to the only question that matters.

---

## Tunables

All at the top of the `.ino`.

| Constant | Default | Set it from |
|---|---|---|
| `PULSES_PER_VEND` | `1` | how you programmed the acceptor in learn mode |
| `IR_DETECTED_IS_LOW` | `true` | step 3 above |
| `RELAY_ACTIVE_HIGH` | `true` | your relay module |
| `LCD_I2C_ADDR` | `0x27` | step 1 above |
| `DISPENSE_TIMEOUT_MS` | `3000` | measured revolution time × 2 |
| `COIN_DEBOUNCE_MS` | `50` | step 2 above |

---

## Behaviour

```
IDLE ──coin, credit met──► check beam is clear
                            │
              not clear ────┴──► FAULT "CHECK SENSOR"
                            │
                         clear
                            ▼
                      DISPENSING ──pad breaks beam──► DELIVERED ──2.5 s──► IDLE
                            │
                      timeout (3 s)
                            ▼
                    FAULT "EMPTY OR JAMMED"
```

**Faults latch.** The machine stops and reports rather than reversing or
retrying — the relay drives one direction only, which is a deliberate decision
agreed with the client. Power-cycle to clear.

Coins inserted while dispensing or faulted are **logged but not banked**, so
the machine never owes a pad it cannot deliver.

Vend count is written to NVS on every successful vend and survives power loss.
It shows on the LCD at startup and on Serial.

---

## Known limits of this build

Stated here so nothing is claimed that has not been demonstrated.

- **No speed control.** The relay has no PWM. Dispensing speed is fixed by the
  motor you buy and can only be changed by changing the motor.
- **No reverse**, so no automatic jam recovery.
- **Mains only.** Battery, sleep mode and the wake button are stage two. The
  3–4 week battery figure in the client study is still arithmetic, not a
  measurement.
- **One pad product.** The coil is wound to one pitch.
- **A faulted machine still swallows coins.** The acceptor is wired permanently
  to 12 V, so once the machine latches `OUT OF ORDER` it keeps taking coins
  mechanically. Firmware logs them and refuses to bank them — it never owes a
  pad it cannot deliver — but it cannot hand the money back. Cutting power to
  the acceptor needs the high-side switch (MOSFET or relay) from the full
  design, which was dropped from this build. Tape the slot if the demo faults.
- **95% coin recognition** is the acceptor's rating. Roughly one good coin in
  twenty is refused and must be re-inserted. Expect it at the demo; it is not
  a fault.
