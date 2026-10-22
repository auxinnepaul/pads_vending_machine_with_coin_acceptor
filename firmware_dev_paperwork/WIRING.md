# Wiring — Pad Dispenser

**As built on 2026-09-30, updated 2026-10-01** (level converter, LCD, new board up). This is the authoritative wiring for the machine. It
supersedes the pin table and diagram in the top-level `README.md` and the older
wiring card artifact, both of which predate the optocoupler and the relay swap.

Every item carries a status:

- ✅ **built and metered.** Fitted and checked with a meter.
- 🔌 **fitted, not yet run.** Fitted, but no live board has exercised it yet.
- ⏳ **not built yet.**

The sketch that goes with this wiring is the one in this repo,
`pads_vending_machine_with_coin_acceptor.ino`. The older copies in
`Documents/Arduino` do **not** match it and must not be flashed.

---

## Rules that protect the hardware

1. **The coin acceptor never connects straight to a GPIO.** COIN goes through
   the PC817 (section 3). Three boards were killed by spikes on that wire.
2. **Prefer to switch the 12 V off before plugging or unplugging the
   acceptor.** The P6KE15CA TVS (section 2) now clamps the 24–36 V hot-plug
   spikes, so this is good practice rather than mandatory; the PC817 already
   keeps those spikes off the ESP32 regardless.
3. **Set the buck to 5.0 V with a meter, with nothing on its output,** before
   any ESP32 is connected. Recheck it whenever the trimpot may have been
   touched.
4. **GPIO 19 and GPIO 26 each have a 10 kΩ pull-down to GND.** Without them the
   pins float during boot, the motor can twitch and the coin slot can open with
   no firmware running.
5. **Relay module VCC comes from the 5 V rail, never from the ESP32's 3V3 pin.**

---

## 1. Pin map

| GPIO | Connects to | Firmware mode | External parts | Status |
|---|---|---|---|---|
| **18** | PC817 **pin 4** (collector) | `INPUT_PULLUP`, interrupt on `FALLING`, 75 ms debounce | 10 kΩ from pin 4 to **3V3** | ✅ all four coins decode exactly |
| **19** | Inhibit relay (relay 2) `IN` | Output, active HIGH | 10 kΩ **pull-down** to GND | ✅ no click through boot; self-test correct. `n1`/`n0` not yet run |
| **26** | Motor relay (relay 1) `IN` | Output, active HIGH | 10 kΩ **pull-down** to GND | ✅ no click through boot |
| **27** | KY-032 IR sensor `OUT` | Input, LOW = pad seen | **1 MΩ pull-down to GND** ✅ (100 kΩ was too strong) | ✅ reads HIGH, chute clear in self-test |
| **25** | Passive buzzer module `I/O` | Tone output; **held HIGH when silent** | Module VCC from **3V3**, not 5 V | ✅ chirps; module cold at idle |
| **21** | Level converter **LV1**, then LCD SDA | I²C SDA | Level converter (section 9) | ✅ LCD found at 0x27 |
| **22** | Level converter **LV2**, then LCD SCL | I²C SCL | Level converter (section 9) | ✅ |
| **23** | Retry / jam-clear button, other side to GND | `INPUT_PULLUP`, held 1 s | — | 🔌 |
| **13** | Pad stock limit switch, other side to GND | `INPUT_PULLUP`, LOW = pads present | — | ✅ self-test: PADS PRESENT |

**Pins deliberately not used:** 0, 2, 5, 12, 15 (strapping pins, which decide
how the chip boots), 1 and 3 (USB serial console), 6–11 (flash chip), 34–39
(input only, no internal pull-up).

**Matching firmware settings** (SECTION 1 and SECTION 4 of the sketch):

```
PIN_COIN = 18   PIN_INHIBIT = 19   PIN_RELAY = 26   PIN_IR = 27
PIN_BUZZER = 25 PIN_SDA = 21       PIN_SCL = 22     PIN_CLEAR = 23   PIN_PADS = 13

RELAY_ACTIVE_HIGH         = true    motor relay
INHIBIT_RELAY_ACTIVE_HIGH = true    inhibit relay (was false before the swap)
INHIBIT_CONTACT_IS_NC     = true    fail-safe contact
IR_DETECTED_IS_LOW        = true
PADS_PRESENT_IS_LOW       = true
PAD_SWITCH_FITTED         = true
LCD_I2C_ADDR              = 0x27
```

---

## 2. Power

Star-wired. Every load has its own pair of wires back to the 12 V terminal
block, rather than being chained from one load to the next. All grounds meet at
that block.

```
                        12 V TERMINAL BLOCK
12 V 4 A adaptor ──────┬───────────────────────────────────────────
                       │  1000 µF 25 V  ✅          (stripe to GND)
                       │  P6KE15CA TVS  ✅ fitted (bidirectional, no band)
                       │
                       ├──► coin acceptor 12 V / GND
                       │      + 100 nF at the acceptor end   ⏳ optional
                       │
                       ├──► relay 1 COM ─ NO ──► motor (section 5)
                       │
                       ├──► PC817 LED feed: 1 kΩ to pin 1 (section 3)
                       │
                       └──► LM2596 IN
                              LM2596 OUT = 5.01 V ✅
                              + 1000 µF 16 V + 100 nF at its output ✅
                                   │
                         5 V RAIL ─┼──► Schottky ──► ESP32 VIN
                                   ├──► relay module VCC (both relays)
                                   │      (~1.3 cm from the buck: its output
                                   │       caps serve the relays)
                                   ├──► level converter HV   ✅
                                   └──► LCD VCC               ✅
                                          (~4 cm from the buck: its output
                                           caps serve the LCD)
                                          + 100 nF at the backpack pins  ✅

ESP32 3V3 pin ──┬──► buzzer module VCC  ✅ (moved from 5 V; see section 6)
                ├──► KY-032 VCC
                ├──► 10 kΩ pull-up to PC817 pin 4
                └──► level converter LV   ✅
```

**Measured:** 12 V in, 5.01 V out of the buck. The diode into VIN is a
**Schottky**, which drops about 0.3 V; an ordinary silicon diode drops about
0.7–1.0 V and would leave the board's own 3.3 V regulator too little headroom.

**Why each part is there:**

| Part | Where | Job |
|---|---|---|
| 1000 µF 25 V | 12 V block | Stops the rail sagging when the motor or solenoid starts, and soaks up spike energy |
| TVS P6KE15CA ✅ | 12 V block | Clamps spikes on the 12 V rail below the LM2596's 40 V input limit. A buck killed by a spike usually fails shorted, which puts 12 V on the 5 V rail |
| 1000 µF + 100 nF | Buck output | The electrolytic holds the 5 V rail up through dips; the ceramic absorbs the fast switching edges the electrolytic is too slow for |
| Schottky | Buck to VIN | Stops the buck and USB fighting when both are connected |
| (none needed) | Relay module VCC | The relays sit ~1.3 cm from the buck's 1000 µF + 100 nF, which serve as their local supply. A 100 nF across the module's VCC/GND is optional insurance |

---

## 3. Coin line through the PC817 ✅

### Identifying the pins

A small dot (or notch) on top marks **pin 1**. Hold the chip with the legs
pointing down, away from you, and the dot at top left. The pins count
anticlockwise:

```
           ●
   pin 1 ┌─────┐ pin 4
         │PC817│
   pin 2 └─────┘ pin 3
```

| Pin | Inside the chip | Side |
|---|---|---|
| 1 | LED anode | acceptor / 12 V |
| 2 | LED cathode | acceptor / 12 V |
| 3 | Phototransistor emitter | ESP32 |
| 4 | Phototransistor collector | ESP32 |

Check with a meter on diode mode: red on 1, black on 2 reads about 1.0–1.2 V;
reversed reads OL. Pins 3–4 read OL both ways.

### As built

```
ACCEPTOR SIDE                                  ESP32 SIDE

12 V ──┬──[1 kΩ]──── pin 1           pin 4 ──┬──── GPIO 18
       │                                     └──[10 kΩ]──── 3V3
    1N4007
  (band to 12 V)
       │
COIN ──┴──────────── pin 2           pin 3 ─────────── ESP32 GND
```

The clamp diode runs from COIN to the **12 V side** of the 1 kΩ. The original
design put it directly across pins 1 and 2. Both protect the LED. In this
position a spike on COIN is held to about 12.6 V, which leaves only about 0.6 V
reverse across the LED (rated 6 V), and the spike's energy goes into the
1000 µF on the rail. A 1N4007 is used because no 1N4148 was bought; any
ordinary diode works here.

**No other connection between COIN and anything on the ESP32 side.** In
particular, the old 10 kΩ pull-up from COIN to 3.3 V is removed. Leaving it
in would join the two sides and bypass the optocoupler.

### Why it works

The PC817 passes light, not voltage. The only supply on the ESP32 side is the
3.3 V pull-up, so GPIO 18 can only ever see 0 V or 3.3 V, whatever happens on
the acceptor side.

| State | LED | Transistor | GPIO 18 |
|---|---|---|---|
| Idle (acceptor output open) | off | off | 3.3 V (HIGH) |
| Coin pulse (acceptor sinks COIN) | on | on | ~0 V (LOW) |

That is the same polarity the firmware already expected, so no firmware change
was needed.

### Measured (meter on pin 4 to GND, GPIO 18 not connected)

| Check | Reading |
|---|---|
| Idle | 3.29 V |
| Coin dropped | dips to ~2.2 V and recovers (the meter averages the pulses) |
| COIN held to GND | **33 mV** (logic low needs below ~0.8 V) |
| Acceptor unplugged and replugged | **no change** |

---

## 4. Coin inhibit relay (relay 2) 🔌

The acceptor's 2-pin inhibit port: the **lower pin is its GND**; shorting the
**upper pin** to it **inhibits** the acceptor. Confirmed by hand on the bench.

```
GPIO 19 ──┬────────────── relay 2 IN
          └──[10 kΩ]───── GND

relay 2 VCC ── 5 V rail        relay 2 GND ── common GND

relay 2 COM ─────────── inhibit port, upper pin
relay 2 NC  ─────────── inhibit port, lower pin (acceptor GND)
relay 2 NO  ─────────── not connected
```

The relay is **active-HIGH, identical to the motor relay.** It replaced an
active-LOW module that a 3.3 V GPIO could switch on but never off.

**The contact is NC, and that is the fail-safe:**

| ESP32 state | GPIO 19 | Coil | NC contact | Acceptor |
|---|---|---|---|---|
| Running, accepting | HIGH | on | open | accepts coins |
| Running, inhibiting | LOW | off | closed | inhibited |
| Booting, reset, crashed, unpowered, dead | LOW (pull-down) | off | closed | **inhibited** |

A dead controller can never leave the slot open. The coil is energised most of
the time in normal use; relays are rated for that, and it costs about 0.35 W.

**Why a relay and not a MOSFET:** the contacts are separated from the coil by
an air gap, so nothing on the acceptor's side can reach the ESP32. A MOSFET
across the port would also work electrically, but it would leave the slot
open whenever the ESP32 dies.

---

## 5. Motor relay (relay 1) 🔌

```
GPIO 26 ──┬────────────── relay 1 IN
          └──[10 kΩ]───── GND

relay 1 VCC ── 5 V rail        relay 1 GND ── common GND

12 V block ── relay 1 COM
relay 1 NO ──────────────┬──────────┬──── motor +
                         │          │
                      1N5408      47 nF
                 (band to motor +)  │
                         │          │
12 V block GND ──────────┴──────────┴──── motor −
```

- **1N5408** across the motor terminals, **band to the positive terminal.**
  Backwards, it shorts the supply the moment the relay closes. Check it on
  diode mode: red on motor −, black on motor + reads about 0.5 V.
- **47 nF ceramic** across the same terminals, for brush noise. No polarity.
- Motor tested running directly: ✅.

---

## 6. IR sensor, buzzer

**KY-032 IR sensor**

```
KY-032 VCC ── ESP32 3V3
KY-032 GND ── common GND
KY-032 OUT ──┬── GPIO 27
             └──[1 MΩ]──── GND      ✅ fitted 2026-10-22 (100 kΩ removed, too strong)
```

**Why a pull-down.** Without it, an unplugged sensor leaves GPIO 27
floating. It usually drifts LOW, which reads as "something in the beam", but
not always; floating HIGH reads as "clear", and the machine would run with no
sensor and fault every vend as JAMMED. With the pull-down, an unplugged sensor
**always** reads BLOCKED: the self-test halts at boot, and on a running
machine the beam reads blocked at rest for 5 s and the slot shuts with
`CHECK SENSOR`. Plugging it back in clears the fault by itself after 3 s.

**Why 1 MΩ: measured, not assumed.** The pull-down forms a divider with the
sensor's own output pull-up when the beam is clear, and the ESP32 only
guarantees reading HIGH above about **2.5 V**. A ~10 kΩ pull-up was assumed,
which made 100 kΩ look right (~3.0 V). **Measured on this KY-032:** with no
pull-down the output sits at **3.02 V** with the beam clear; with 100 kΩ it
fell to **2.01 V**. So its output is about 3.02 V behind roughly **50 kΩ**.
Against that:

| Pull-down | GPIO 27, beam clear | |
|---|---|---|
| none | 3.02 V (measured) | ✅ but an unplugged sensor floats |
| 100 kΩ | 2.01 V (measured) | ❌ below the guaranteed HIGH: a dip could read as a pad falling |
| 470 kΩ | ~2.73 V | marginal |
| **1 MΩ** | **~2.88 V** | ✅ |
| 2.2 MΩ | ~2.95 V | ✅ |

**What the 1 MΩ actually covers** (measured with no pull-down): with the
sensor's **VCC** disconnected, GPIO 27 still reads **0.000 V** and the machine
goes out of order correctly, because the module's own ~50 kΩ pull-up now ties
the pin to its unpowered VCC. So the pull-down is not needed for a lost VCC.
It is needed for a lost **OUT** wire, or the whole sensor unplugged, where the
pin really floats, and probably a lost GND (the module's output is then not
driven at all). A broken signal wire is the likeliest connector failure in a
machine with a motor in it.

1 MΩ still holds an unplugged pin at 0 V: the pin's leakage is far too small
to lift it. If a 1 MΩ is not to hand, remove the 100 kΩ rather than run with
it; floating only matters when the sensor is unplugged.

**Check after fitting:** sensor connected and the chute empty, meter on
GPIO 27 to GND: **at least 2.8 V**. Then unplug the sensor (or its VCC): it
should read **0 V**.

**Measure volts with the power on, not ohms.** A resistance reading taken
across the fitted resistor read 33 kΩ on the bench: the meter also measures
the parallel path through the sensor's pull-up and the unpowered 3V3 rail
(100 kΩ ∥ ~50 kΩ ≈ 33 kΩ). The resistor was fine. To check a resistor's value,
read its bands (brown-black-yellow = 100 kΩ) or lift one leg.

Output goes LOW when it sees a pad (`IR_DETECTED_IS_LOW = true`). Powered from
3V3 so its output can never exceed 3.3 V. Console command `i` shows the live
reading.

**Passive buzzer module** (9012 PNP driver, "low level trigger")

```
buzzer VCC ── ESP32 3V3   buzzer GND ── common GND    buzzer I/O ── GPIO 25
```

**Why 3V3, not 5 V.** The module's 9012 is a PNP transistor: the coil is on
while the input is LOW. The firmware holds GPIO 25 **HIGH** between beeps so
the coil carries no current (it used to sit LOW, silent but drawing steady
current through the coil, and the module ran warm). A HIGH only turns the PNP
fully off when the module's VCC is the same 3.3 V: from a 5 V VCC, 3.3 V on
the input still leaves ~1.7 V across emitter and base, and the transistor
stays partly on. It is the same reason a 3.3 V pin cannot release a 5 V
active-LOW relay (`LESSONS_LEARNED.md` 1.4). The module is rated 3.3-5 V; at
3.3 V it is a little quieter.

**Check:** with the machine idle, the module should be cold to the touch, and
GPIO 25 should read about 3.3 V to GND.

---

## 7. Retry button and pad stock switch

```
GPIO 23 ── momentary button ── GND        (front panel, customer can reach it;
                                           any press also wakes a sleeping screen)
GPIO 13 ── limit switch    ── GND         (closed = pads present)
```

Both use the ESP32's internal pull-up, so no resistors.

**Stock switch direction:** closed (LOW) means pads present. A broken wire or
pulled connector reads as EMPTY, so the machine stops taking money rather than
selling pads it does not have.

**Keep the stock switch wiring away from the relay wiring.** On the bench the
inhibit relay switching made the stock line flicker between EMPTY and PRESENT
several times a second, and that loop reset the board twice. The firmware
guard (`INHIBIT_SETTLE_MS` and the flap detector, committed and verified)
stops the firmware reacting to it: no stock-line change through 24 relay
clicks and every vend since. Good wiring practice still applies:

- route the stock switch pair separately from the relay and motor wires;
- keep the relay modules close to the buck's output caps (they are, ~1.3 cm,
  so no separate relay decoupling is needed);
- make every ground meet at the one terminal block.

---

## 8. Grounds

Every ground returns to the 12 V terminal block:

- 12 V adaptor negative, coin acceptor GND, motor −, LM2596 IN−
- LM2596 OUT− (5 V rail ground), ESP32 GND, relay module GND
- PC817 pin 3, KY-032 GND, buzzer GND
- level converter GND, LCD GND

The inhibit port's lower pin is the acceptor's own ground. It connects only to
the relay's NC contact, which makes that ground connection only while the relay
is off.

---

## 9. LCD through the I²C level converter ✅

**Solder it, don't use jumper wires.** On 2026-10-07 the display garbled once
while it was on Dupont jumpers, and cleared itself on the next rebuild. A
push-on contact that breaks for a fraction of a millisecond under motor and
relay vibration has the same effect as a supply dip. It has been soldered
since, with the I²C lines short and crossing nothing.

**Why:** the LCD backpack runs on 5 V and pulls SDA and SCL up to 5 V. ESP32
pins are rated for 3.6 V. The converter translates between the two, so the
ESP32 side only ever sees 3.3 V.

**Module:** 4-channel bi-directional I²C logic level converter (ASK
Electronics, KSh 200). The listing doesn't say which transistor it uses or
whether it has pull-up resistors. Most of these boards use four BSS138 MOSFETs
with 10 kΩ pull-ups on both sides. Look at the board to confirm.

**Its pins:** `HV` and `LV` (the two supply voltages), `GND` (usually one on
each side, both the same), and four channel pairs `HV1`–`HV4` / `LV1`–`LV4`.
A signal on `LV1` appears on `HV1`, and so on.

```
                LEVEL CONVERTER
ESP32 3V3 ───── LV          HV ───── 5 V rail
common GND ──── GND        GND ───── common GND

GPIO 21 ─────── LV1        HV1 ───── LCD SDA
GPIO 22 ─────── LV2        HV2 ───── LCD SCL
                LV3, LV4 / HV3, HV4: not used

LCD VCC ── 5 V rail        LCD GND ── common GND
           + 100 nF soldered across VCC/GND at the backpack pins ✅
           (no separate bulk cap needed when the LCD is a few cm from the
            buck's output caps)
```

The backpack's own pull-ups to 5 V can stay: they are on the HV side now,
where 5 V is correct. No firmware change is needed.

**Checks before the ESP32 is connected to the converter:**

1. Converter powered (HV = 5 V, LV = 3.3 V), GPIO 21/22 not connected.
2. Meter on **LV1** and **LV2** to GND: **3.3 V**, never 5 V.
3. Meter on **HV1** and **HV2** to GND: about **5 V**.

If LV1 or LV2 reads 5 V, stop: HV and LV are swapped, or an LCD wire is on
the wrong side.

**After connecting:** console command `z` scans the I²C bus and should find
the LCD at **0x27** (sometimes 0x3F).

**Measured 2026-10-01:** LV1/LV2 3.29 V, HV1/HV2 5.02 V before connecting;
the boot self-test then found the LCD at 0x27 and it showed the idle screen.
Each side idling at its own rail shows this board has pull-ups on both sides.

**The backlight goes off after 2 minutes idle** (`BACKLIGHT_TIMEOUT_MS`). The
text is still readable in the dark. That is intended; a coin or the button
wakes it.

---

## 10. First power-up of a new board

Done for the current board on 2026-10-01: steps 1–4 passed (3V3 at 3.28 V,
nothing hot, no click before the chirp, `SELF-TEST: PASS`). Steps 5–8 are next.
Close the Arduino IDE's serial monitor before flashing from the command line;
it holds COM3. In order:

1. Power on with the USB unplugged. Nothing should get hot. No relay should
   click.
2. Meter on the ESP32's **3V3 pin**: 3.3 V, and it should stay above 3.2 V
   while the motor runs.
3. USB in, flash the sketch from this repo, open the console at 115200.
4. Read the boot self-test. Section [7] should report the inhibit relay as
   active HIGH, NC, with a pull-down.
5. `n1` then `n0`: the inhibit relay energises on `n1` and releases on `n0`.
6. Press EN a few times with the acceptor connected: **no relay may click**
   during boot.
7. Drop each coin: 1 / 5 / 10 / 20 pulses decode to KSh 5 / 10 / 20 / 40,
   the same count every time, every gap ~155 ms. ✅ 2026-10-01
   Then accumulation (4 × 5 and 2 × 10) and a KSh 40 double vend.
8. `s`: check `motor noise` with the 47 nF and 1N5408 fitted.

---

## Status summary

| Item | Status |
|---|---|
| 12 V block 1000 µF | ✅ |
| Buck 5.01 V, output 1000 µF + 100 nF, Schottky to VIN | ✅ |
| Motor 47 nF + 1N5408, motor runs | ✅ |
| PC817 coin line, metered | ✅ |
| Pull-downs on GPIO 19 and 26 | ✅ fitted |
| Active-HIGH inhibit relay on NC | ✅ holds through boot; verified over many vends |
| New ESP32 | ✅ flashed, self-test PASS |
| Coins through the PC817 | ✅ all four coins exact after the double-count fix; five vends |
| TVS on 12 V block | ✅ P6KE15CA fitted 2026-10-22 |
| Relay module decoupling | ✅ not needed: ~1.3 cm from the buck's output caps; the relay/stock-switch coupling is handled and verified |
| 100 nF at acceptor end | ⏳ optional |
| Level converter + LCD | ✅ LCD at 0x27, idle screen showing |
| Assembled in the enclosure | ✅ 2026-10-22; 6-day run, flat heap |
| Coil feeds real pads | ⏳ 2 of 2 so far; full 25–50 pad soak pending |
| LCD decoupling | ✅ 100 nF soldered at the backpack's VCC/GND; no bulk cap needed (~4 cm from the buck's 1000 µF + 100 nF) |
| Pull-down on GPIO 27 | ✅ 1 MΩ fitted 2026-10-22 (100 kΩ gave 2.01 V, too low; 3.02 V with none). Self-test reads chute clear |

The measurements and reasoning behind all of this are in
`FIRMWARE_PROGRESS.md`, in the 2026-09-28 to 2026-09-30 sections.
