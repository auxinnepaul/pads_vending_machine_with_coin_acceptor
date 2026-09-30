# Firmware progress

The living memory for this project. **Read the RESUME block first.** Update it
every time the work moves on, and add a dated section saying what changed, why,
what the behaviour change is, and whether it is verified on hardware.

Mark **✅ verified on hardware** vs **⏳ pending** explicitly. A thing that
compiles is not a thing that works.

---

# ▶ RESUME HERE

**State as of 2026-10-01.**

### ⚡ Switch the 12 V off before plugging or unplugging the coin acceptor

The PC817 now protects the ESP32 from the coin line (metered, see below). The
TVS for the 12 V rail is **not bought yet**, so the buck is still exposed to
the worst spikes. Until it is fitted, never connect or disconnect the acceptor
with power on.

**Wiring as built: `WIRING.md`.** That is the authoritative pin map and
diagram set.

**A new board is up and passing its self-test** (2026-10-01): flashed, LCD
working through the level converter, coin line reading correctly through the
PC817, no relay click during boot.

The firmware is functionally complete and every coin path was verified on real
hardware, **with the coin line wired directly to GPIO 18**. The PC817 changes
that input path, so the coin tests must be repeated on the new board. That is
the next step.

### Verified on hardware ✅

- All four coins decode: **1 / 5 / 10 / 20** pulses → KSh 5 / 10 / 20 / 40.
  Re-verified 2026-10-01 through the PC817 after the double-count fix. The
  earlier 2 / 10 / 20 / 40 figures were every pulse counted twice
- Accumulation: 4 × KSh 5 and 2 × KSh 10 both reach KSh 20 and vend
- KSh 40 vends two pads back to back
- Change carries across a vend and lands on the correct screen
- Meter survives a power cut; recovered pad is held and released by button or coin
- Jam retry, the three-attempt cap, and the customer button
- Coin-noise muting (the machine no longer spends induced pulses)
- Shorting the inhibit port by hand inhibits the acceptor, so NC is fail-safe
- **PC817 coin line (meter only):** 3.29 V idle, 33 mV when held low, no change
  when the acceptor is unplugged and replugged
- **Power:** 5.01 V from the buck, Schottky into VIN, bulk caps at the 12 V
  block and the buck output, 47 nF + 1N5408 at the motor
- **I²C level converter (meter):** LV1/LV2 3.29 V, HV1/HV2 5.02 V
- **New ESP32 first boot:** 3V3 at 3.28 V, nothing hot, flashed first time,
  `SELF-TEST: PASS`. LCD found at 0x27 through the converter and showing the
  idle screen; GPIO 18 idle HIGH through the PC817; inhibit relay reported
  active HIGH / NC; stock switch PADS PRESENT; buzzer chirps
- **Boot window:** no relay click before the self-test chirp, so the
  pull-downs on GPIO 19 and 26 hold through reset
- **Backlight sleep:** goes dark after 2 min idle
  (`BACKLIGHT_TIMEOUT_MS`), characters still readable

### Fitted, not yet run on a board 🔌

| | |
|---|---|
| **Active-HIGH inhibit relay** on NC | Self-test reports it correctly and the firmware drives it HIGH after boot. `n1` / `n0` not yet run |
| **Inhibit-relay / stock-switch guard** (`INHIBIT_SETTLE_MS`, flap detector) | Running since the first boot, still uncommitted. No stock-line change through 24 relay clicks and five vends |
| **Double-count fix + `P` probe** | Verified on the bench, still uncommitted (see the 2026-10-01 test log) |

### Pending ⏳

| | Blocked on |
|---|---|
| **TVS on the 12 V block** | Not bought. P6KE15A, band to +12 V |
| **100 µF + 100 nF at the relay module VCC** | Not fitted. Part of the physical fix for the relay disturbing the stock switch |
| **Motor noise → zero** | Re-measure with `s` now the 47 nF and 1N5408 are fitted |
| **Real drop times** | No chute or panel. Every timing so far is a finger at the IR sensor |
| **Service phone number** | Still the placeholder `07XX XXX XXX`. The fault screen tells a stuck customer to call it. Set it before the machine goes to a site |
| **`DISPENSE_TIMEOUT_MS`** | **7000 ms placeholder.** Run `T25` down a real chute, set twice the slowest |
| **Do pads feed off the coil without jamming** | The number one risk in the whole project, still entirely open |

### Next session should

1. ~~Inhibit relay click test and EN resets~~ ✅ 2026-10-01
2. ~~Coin tests through the PC817~~ ✅ 2026-10-01, after finding and fixing
   the double count. Still to do: check the 3V3 pin holds above 3.2 V while
   the motor runs, and two coins inserted fast still read as two.
3. **`s` for motor noise** with the 47 nF and 1N5408 fitted.
4. **Watch the stock line while the inhibit relay switches.** If the guard
   holds and the log shows no flapping, **commit the guard**.
5. Buy and fit the TVS; fit the relay module decoupling.
6. Set the real service number.
7. Build the chute, load the coil, run `T25`, bake the timeout in.
8. The 50-cycle soak — that number is what the client actually asked for.

---

## 2026-10-01 — New board up: level converter, LCD and a clean first boot

**Status: ✅ verified on hardware** for everything listed below. No firmware
change; the sketch flashed is the repo's working tree (the active-HIGH relay
setting from `4bb1319` plus the still-uncommitted inhibit-relay/stock-switch
guard).

### Level converter

Wired per `WIRING.md` §9, with the channels as drawn (GPIO 21 → LV1 → HV1 →
LCD SDA, GPIO 22 → LV2 → HV2 → LCD SCL). Metered before the ESP32 was
connected:

| Pin | Reading |
|---|---|
| LV1, LV2 | 3.29 V |
| HV1, HV2 | 5.02 V |

Each side idles at its own rail, so the board has pull-ups on both sides and
the ESP32 can only ever see 3.3 V on SDA and SCL.

### First power-up of the new ESP32

- 12 V on, USB unplugged: nothing warm, no relay click. 3V3 pin 3.28 V.
- USB in: esptool connected first time with automatic reset (ESP32-D0WD-V3,
  rev 3.1, 40 MHz crystal, MAC `f4:2d:c9:89:bf:d4`). Flashed at 460800, all
  four images hash-verified.
- **COM3 was first refused with "Access denied":** the Arduino IDE's serial
  monitor was holding the port. Close it before flashing from the command
  line.

### Boot self-test: PASS

| Section | Reported |
|---|---|
| [3] Coin line | GPIO 18 reads HIGH (idle, correct), through the PC817 |
| [4] Screen | I²C scan finds 0x27; LCD present |
| [5] IR sensor | chute clear |
| [7] Inhibit | GPIO 19, active HIGH, NC, inhibited during the test |
| [8] Stock switch | PADS PRESENT |
| [8b] Stored balance | store open and writing, KSh 0. **Service number is still the placeholder** |
| [9] Buzzer | chirped |

After the self-test the firmware drove GPIO 19 HIGH and enabled the acceptor.

### Observed at the bench

- **No relay click before the chirp.** The pull-downs hold GPIO 19 and 26 low
  through reset, so the slot cannot open and the motor cannot twitch before
  the firmware is running.
- The LCD shows the idle screen and pages "PAD IS 20 / THIS MACHINE TAKES
  5 10 20 OR 40".
- After 2 minutes idle the backlight goes off (`BACKLIGHT_TIMEOUT_MS`); the
  text is still readable in the dark. Intended: a coin or the button wakes it.

### Bench test log

**Test 1 — inhibit relay ✅**

- `n` click test run four times. Each run drove GPIO 19 LOW (released, slot
  blocked) and HIGH (energised, slot live) three times; the relay clicked
  every time. `n0`/`n1` were not run separately: `n` drives both levels.
- Three EN resets: identical `SELF-TEST: PASS`, the inhibit pin LOW and coins
  inhibited during the test, GPIO 19 driven HIGH only after `Ready`. No click
  before the chirp.
- **No stock-switch change during 24 relay switches.** Before the guard and
  the new relay, relay switching flipped the stock line and reset the board
  twice. First evidence the guard (flashed, uncommitted) is holding. Idle
  only; not yet seen during a vend.

**Test 2 — coins through the PC817** ⏳ in progress

- **Step 1, one KSh 5 ✅.** `2 pulses`, `50 ms burst, gaps 50-50 ms`,
  recognised KSh 5, credit KSh 5 of 20, no vend. The first coin decoded
  through the optocoupler: clean, bounce-free pulse edges.
- **Observed:** ~60 garbled characters on the serial monitor just before the
  coin report, where its separator line starts. Suspected cause: the
  acceptor's gate solenoid fires on every coin, and its ground disturbance
  reaches the USB serial link, since the ESP32 ground is joined to the PC's
  through the USB cable. The coin decoded correctly regardless. **It did not
  repeat on any later coin**, so the per-coin solenoid explanation is
  withdrawn; most likely a one-off such as the serial monitor connecting.
  LCD showed KSh 5 credit and the countdown.
- **Step 2, three more KSh 5 ✅ for the vend.** Credit 10 → 15 → 20 (gaps
  50–51 ms), `Motor on — credit met`, `PAD DELIVERED in 1345 ms` (finger at
  the sensor). No phantom coins after the motor ran; no stock-line change
  during the vend.
- **❌ DEFECT under investigation: a KSh 5 coin swallowed twice.** The
  builder's account: the step-1 coin was then refused twice (came back out;
  no pulses logged, so a refused coin produces nothing, which is correct).
  A second KSh 5 was inserted five times: three times it sent 2 pulses and
  was credited, **twice it sent 1 pulse, was reported as not in the table,
  and went into the cash box.** KSh 10 lost. Both events passed the 20 ms
  debounce, so they were real low periods, not spikes.

  The ISR (falling edge, 20 ms debounce, pulses 50 ms apart) counts both
  pulses as long as the line returns high between them. Same coin, different
  counts, so it is not a coin that simply maps to an unused channel. Two
  candidate causes:

  1. **Acceptor programming.** It has 6 channels; COIN_TABLE uses 4. The
     table's own instructions suggest 1 pulse for the cheapest coin. If KSh 5
     was ever taught into a 1-pulse channel as well as the 2-pulse one, a KSh 5
     can match either. Fix on the acceptor.
  2. **A pulse lost in the new signal path** (the line not returning high
     between pulses). New wiring, so a fair suspect, but the PC817 switches
     in microseconds against a ~25 ms gap. Fix in the circuit.

  **Discriminating test:** a lost pulse would hit every coin (10 → 9,
  20 → 19); a channel mismatch only produces specific counts. Running KSh 10
  ×5, KSh 20 ×5, KSh 40 ×2, and asking whether KSh 5 was taught into more
  than one channel.

  **Result: it is pulse loss, on every coin. Cause 1 ruled out.** Every coin
  went into the cash box; none was rejected.

  | Coin | Expected | Counted |
  |---|---|---|
  | KSh 5 | 2 | 2, 2, 1, 2, 1, 2 (second run) |
  | KSh 10 | 10 | 10, 9, 8, 7, 8, 7, 8, 8, 7, 8, 9 |
  | KSh 20 | 20 | 16, 17, 19, 16, 15 |
  | KSh 40 | 40 | 33, 37 |

  Always short, never over; larger coins lose more. Gaps of 155 ms now appear
  where the acceptor's normal spacing is 50–105 ms, i.e. a pulse missing in
  between. **Retuning COIN_TABLE cannot fix this**: the same coin arrives with
  different counts. Nothing like this was recorded with the direct wiring, so
  the new signal path is the prime suspect.

  Not yet known: whether the missing pulses never pull GPIO 18 low, or the pin
  fails to return high between pulses. Those need opposite resistor changes.
  **Added console command `P`** (uncommitted, compiles clean): samples GPIO 18
  every ~20 µs for one coin and prints each LOW/HIGH period beside what the
  interrupt counted. Reads the pin only; counting and banking unchanged.

  **Separate finding:** the help lists `k`, `l` and `L`, but none is
  implemented, in HEAD or the working tree. Typing them gives "Unknown
  command". Backlog.

  **✅ ROOT CAUSE FOUND with `P` — the pulses were never lost; the
  firmware was counting extra ones.** One KSh 10 under the probe:

  ```
    #   LOW ms   then HIGH ms
     1  50.4     104.6
     2  50.4     104.7
     3  50.4     104.6
     4  50.4     104.8
     5  50.4     -  (end of coin)
    raw LOW periods seen : 5
    interrupt counted    : 9
  ```

  Five clean pulses on the wire; nine counts from the interrupt. The
  interrupt is FALLING, but a few-microsecond glitch on the ESP32's rising
  edge can fire it too, and that false count lands 50 ms after the real one,
  past the 20 ms debounce. **With the direct wiring it fired on every rising
  edge, so every coin read exactly double and COIN_TABLE was calibrated to
  the doubled counts (2/10/20/40).** The "gaps 50–105 ms" recorded back then
  (fall→rise, rise→next fall) was the fingerprint all along. The PC817 changed
  the edge shape so the false count became occasional, which is what
  exposed it. The data fits exactly: counts were never below the true number
  and never above double (KSh 10: 5 real, read 7–10; KSh 40: 20 real, read
  33 and 37).

  My earlier "pulses are being lost in the signal path" was wrong. The
  builder's instinct that the table needed changing was right, for a
  different reason.

  **Fix (uncommitted, compiles clean, flashing next):**
  - `COIN_PULSE_DEBOUNCE_MS` 20 → **75**: between the false count (+50 ms)
    and the next real pulse (+155 ms).
  - `COIN_GAP_MS` 250 → **400**: real pulses are 155 ms apart, so twice the
    widest gap is 310 ms; still under the 600 ms ceiling.
  - `COIN_TABLE` → **1 / 5 / 10 / 20** pulses for KSh 5 / 10 / 20 / 40.
  - Every comment stating the old counts updated.
  All existing self-test and per-coin sanity checks pass with these values.

**Test 2 result after the fix ✅** (flashed with debounce 75 ms, gap 400 ms,
table 1 / 5 / 10 / 20):

| Check | Result |
|---|---|
| `P` with a KSh 5 | raw LOW periods 1, interrupt counted 1 |
| KSh 5 × 4 | `1 pulse` each, 5 → 20, vend (1452 ms) |
| KSh 10 × 2 | `5 pulses` each, gaps 155–155 ms, vend (2108 ms) |
| KSh 20 | `10 pulses`, gaps 155–156 ms, vend (1307 ms) |
| KSh 40 | `20 pulses`, 2946 ms burst, two pads back to back (1163, 1902 ms) |
| Gap warnings | none |

Every gap is now the true 155 ms; the 50 and 105 ms gaps that marked the
double count are gone. Before the fix the KSh 10 read wrong on 10 of 11
drops. Drop times are a finger at the sensor and mean nothing yet. The
50-cycle soak is the full proof.

**Test 3 — motor noise (`s`)** ⏳

---

## 2026-09-28 (later) — Why the boards keep dying: inductive kickback on the coin line

**Status: ✅ cause measured, ⏳ fix not yet built.** No firmware change. This is
the most consequential hardware finding in the project so far.

### The pattern

**Three ESP32 boards have gone silent.** That is not bad luck, it is a circuit
killing them. Do not put another board on the bench until the fix below is in.

### The measurement

Meter from supply ground to the acceptor's COIN output, **ESP32 disconnected**:

| Condition | Reading |
|---|---|
| Idle | ~0.002 mV — effectively 0 V |
| Hot-swapping the acceptor | **24 V**, and separately **36 V**, very briefly |

### What it means

**Idle 0 V clears the wiring.** The COIN output is a true open-collector, not
internally pulled to 12 V. The 10 k pull-up to 3.3 V is the correct way to
drive it. The wiring was never the mistake.

**24 V and 36 V are 2× and 3× the 12 V rail** — the signature of **inductive
kickback**. The acceptor contains a reject solenoid and motor windings.
Interrupting that current dumps the coil's stored energy as a spike
(`V = L·di/dt`) onto the signal line feeding the GPIO.

**The meter under-reports badly.** A DMM samples a few times a second and
filters heavily; it cannot capture a microsecond spike. A DMM showing 36 V
implies a true peak likely **well over 100 V**. The ESP32 GPIO absolute maximum
is **3.6 V**.

This explains everything observed: steady-state running is safe (the machine
worked for whole sessions), and every connect/disconnect fires a spike tens of
times over the pin rating. It also fits the previously logged motor coupling of
up to 472 induced pulses on GPIO 18 — that line has been abused for a while.

### Post-mortem on the dead boards ✅ (2026-09-30)

Continuity checks on two of the dead boards, unpowered:

| Board | Finding | Reading |
|---|---|---|
| A | **GPIO 18 shorted to GND.** 3V3 not shorted; stays cold when powered | The spike entered through the coin pin and destroyed its input stage |
| B | GPIO 18 not shorted. **3V3 shorted to GND; runs hot when powered** | The same path one step further: through the pin's clamp diode into the internal 3.3 V rail |

Board A shows the damage where the spike came in, board B shows the next step
along the path. This is direct physical evidence for the coin-line mechanism,
not an inference. Board A's 3V3 still regulates and is usable as a bench 3.3 V
source, with its GPIO 18 left unconnected. Board B must not be powered: its
shorted rail loads the buck.

### PC817 stage built and metered ✅ (2026-09-30)

Built as in the README, with the clamp diode (a 1N4007, as no 1N4148 was
bought) connected from COIN to the 12 V side of the 1 k rather than to pin 1.
That clamps COIN to about 12.6 V and still holds the LED to ~0.6 V reverse, so
it was kept. Meter at pin 4 (collector) to GND, pulled up to 3.3 V from a dead
board's still-working 3V3, GPIO 18 not connected:

| Check | Reading |
|---|---|
| Idle | 3.29 V |
| Coin dropped | dips to ~2.2 V on the meter and recovers (the meter averages the pulses) |
| COIN held to GND (a very long pulse) | **33 mV**, well below the 0.8 V logic-low threshold |
| Acceptor unplugged and replugged, several times | **No change** from 3.29 V |

The last row is the one that matters. The same action used to put 24–36 V on
the line. **⏳ Not yet verified with firmware:** coin decoding through the
optocoupler still has to be re-run on a live board.

Also fitted the same day: 10 k pull-downs on GPIO 19 and GPIO 26 (neither pin
had one), the active-HIGH inhibit relay, 1000 µF at the 12 V terminal block,
1000 µF + 100 nF at the buck output, 47 nF + 1N5408 at the motor. Buck
output 5.01 V; the VIN diode is a Schottky. **No TVS yet**, so switch the 12 V
off before plugging or unplugging the acceptor until one is fitted.

### Procedure is necessary but NOT sufficient

"Stop hot-plugging" is not a fix. **The solenoid energises and de-energises
during normal operation**, every time a coin is rejected. The same mechanism
fires on a healthy machine, smaller and less often. Plug-order discipline
(**ground first on connect, ground last on disconnect**; better, one switched
supply for everything) removes the worst case only.

### The fix — optocouple the coin line

Also expected to eliminate the motor-induced false pulses, since it removes the
galvanic path entirely:

```
12 V --[1k]--> PC817 LED ---- COIN signal (acceptor sinks it low)

PC817 transistor:
  collector --+-- ESP32 GPIO 18
              +--[10k]-- 3.3 V
  emitter   ----- ESP32 GND
```

Supporting parts, in order of value:

1. **Flyback diode (1N4007) across the solenoid**, at the coil — kills the
   spike at source rather than downstream.
2. **1 k in series** with the GPIO — limits fault current to what a clamp diode
   survives.
3. **3.3 V zener or TVS to GND** on the GPIO side, belt-and-braces.

### Also suspected, lower priority

The **5 V I2C LCD backpack** (PCF8574) idles SDA/SCL at 5 V through its
pull-ups. ESP32 GPIOs are not 5 V tolerant. But the current through typical
4.7 k pull-ups is only ~0.3 mA into the clamp diode — a slow degrader, not an
instant killer. Fix with an I2C level shifter or a 3.3 V-powered backpack.
**⏳ Unmeasured:** with the LCD powered and the ESP32 out of circuit, measure
SDA→GND and SCL→GND; 5 V confirms it needs shifting.

### Inhibit port: shorting inhibits ✅ (2026-09-29)

Shorting the acceptor's two inhibit pins by hand with it powered stops it
accepting coins. So **NC contacts are the fail-safe choice**: coil off (board
dead, booting or crashed) means contacts closed and the acceptor inhibited.

**Decided:** replace the active-LOW inhibit relay module with an **active-HIGH**
one matching the motor relay. When it is fitted: set
`INHIBIT_RELAY_ACTIVE_HIGH = true`, move GPIO 19's 10 k from pull-UP to
pull-DOWN, and rewrite the pull-up comment above that constant. No MOSFET.

**Flyback diode placement:** the solenoid is inside the acceptor, so the 1N4007
goes reverse-biased across the acceptor's 12 V input at its connector (cathode
to +12 V). It is a secondary defence; the PC817 is the primary one.

### Still to confirm

Reconnect the 10 k pull-up to 3.3 V, ESP32 still out of circuit, and measure
idle. Expect **3.3 V**, dropping to 0 V per coin pulse. Anything above 3.3 V
means a second problem.

---

## 2026-09-28 (later) — The board went silent, and the full disk was a red herring

**Status: ⏳ unresolved, blocked on bench measurement.** No firmware change.

### What was actually wrong with the tooling

`C:` had **zero bytes free**. `esptool` unpacks a Python runtime into a temp
directory on every run, so it was dying before it ever reached the serial port.
Every upload failure logged before this point is explained by that and says
nothing about the hardware. Disk cleared to 16.3 GB; `esptool` now runs to
completion.

### What is wrong with the board

With a working `esptool`, the board is silent in every state tested:

| Test | Result |
|---|---|
| `esptool --chip esp32 --port COM3 chip_id` | `No serial data received` |
| Passive listen @ 115200 and @ 74880 | 0 bytes |
| Forced EN reset via RTS, listening @ 74880 | 0 bytes |
| All four DTR/RTS combinations after reset | 0 bytes each |

**Two inferences worth keeping.**

1. **Zero bytes rules out a baud mismatch.** A wrong baud rate yields framing
   garbage, not silence. Nothing at all means no activity on the ESP32's TX
   line — an absent signal, not a misread one.
2. **The USB side is healthy.** COM3 enumerates as a CP210x, which runs off USB
   5 V. So the cable's data lines and the bridge chip are alive; the fault is
   downstream, on the ESP32 itself. The ROM bootloader prints its
   `ets Jul 29 2019` banner on every EN release regardless of flashed firmware,
   given power, a clock and a TX path. It printed nothing.

### Prime suspect

The board was **verified working earlier the same day** (all coin paths). The
last work done was the inhibit-relay investigation, which involved **jumpering
5 V** to release an active-low relay module. A slip there — 5 V onto a 3.3 V
GPIO or onto the 3V3 rail — would kill the regulator or the chip while leaving
the USB bridge enumerating exactly as observed. **Unconfirmed**, but inspect
before replacing anything.

### Next session: measure, in this order, stop at the first failure

1. **3V3 → GND** — expect ~3.3 V. 0 V or ~5 V means the regulator is gone.
2. **EN → GND** — expect ~3.3 V at rest. Near 0 V means permanent reset.
3. **Swap USB cable and port** — a cable that enumerates but sags on 5 V can
   brown out the ESP32 while the bridge still works.
4. **Try a spare ESP32 on COM3** and re-run `chip_id`. That settles
   board-versus-everything-else in one move.

### Unrelated, done the same session

The uncommitted working-tree changes to the sketch (173 insertions, stock
switch) now **compile clean** — 25% program storage, 6% dynamic memory, only
the cosmetic `LiquidCrystal_I2C` architecture warning. **⏳ Not flashed, not
verified on hardware**, and cannot be until the board responds.

---

## 2026-09-28 — The inhibit relay: an ESP32 cannot turn off a 5 V active-low module

**Settled, with the bare Blink example.** GPIO 19 toggling HIGH/LOW every
second, nothing else running, and the relay never moved. A jumper from **5 V**
releases it; a jumper from the ESP32's **3V3** does not.

Not a firmware problem. No firmware setting fixes it.

**Why the motor relay working was never evidence.** Two different circuits:

- **active HIGH** (motor): `pin -[R]-|>- GND`. 3.3 V switches it on, LOW off.
  A 3.3 V MCU drives it perfectly.
- **active LOW** (inhibit): `5V -|>-[R]- pin`. LOW switches it on. Turning it
  **off** needs the pin near 5 V; 3.3 V leaves ~1.7 V across the LED, enough
  to keep conducting. It energises and never releases.

I had this right first, retracted it when the motor-relay evidence was raised,
and was wrong to. The evidence was real but did not apply.

**Multi-channel boards dodge it** with a `JD-VCC` jumper separating logic from
coil supply. **Single-channel boards have no such jumper** — the case here.

**Fix: one NPN.** Pin HIGH → transistor on → pulls the module input to 0.2 V →
relay on. Pin LOW or floating → transistor off → module's own pull-up takes it
to 5 V → relay off.

Two config consequences when it is fitted:
- `INHIBIT_RELAY_ACTIVE_HIGH = true` — the transistor inverts the logic
- The pull resistor becomes a pull-**DOWN**, and the boot window becomes
  fail-safe by default rather than by careful wiring

**Verified.** ⏳ Transistor not yet fitted. Config unchanged until it is.

---

## 2026-09-28 — The soak stopped after every first cycle

**The bug.** `T25` ran one cycle and stopped:

```
  #1  delivered in 1127 ms
  Chute is blocked. Clear it and start again. Stopping.
```

Reproducible on every run, whatever the cycle count.

**Cause.** `padSeen()` is a debouncer and carries state from whenever it was
last called. Immediately after a delivery that state is BLOCKED — the pad
tripped the beam and nothing has called it since to notice the pad has gone.
The soak's beam check bailed out on the **first** reading, so it read that
stale value and declared a blocked chute on a chute that was empty.

**Why it never showed in a paid vend.** Luck. `ST_DELIVERED` polls `padSeen()`
on every pass for 2.5 seconds, so the debouncer was always fresh by the time
`startVend()` ran. Nothing polled it during the soak's gap between cycles.

**Fix.** One shared `chuteClear()` that polls the **whole** settle window and
only judges at the end, so the debouncer catches up before the verdict.
`startVend()`, `soakTest()` and `attemptClear()` all use it — the first had the
same latent bug and was surviving on that accident.

**Verified.** ⏳ Compiles. Needs a re-run of `T5` on hardware.

---

## 2026-09-28 — Paperwork, soak command, silent boot

**What changed.** Added `T<n>`, a soak that runs *n* vends back to back, times
every drop, and reports twice the slowest as the timeout to set. Added
`SERIAL_LOG_BOOT` so a field machine can boot completely silently. Created this
paperwork set.

**Why.** `DISPENSE_TIMEOUT_MS` had been set three times from whatever happened
to be in the last log, and once from a finger at the sensor. That is not a
measurement. The soak makes it one — and its ceiling is deliberately far longer
than the real timeout, because a stopwatch that stops before the event cannot
measure it.

`SERIAL_LOG_DEFAULT` did not cover the boot: the banner and self-test were
forced on regardless, so a unit set quiet still printed a second of text at
every power-up. "Off" was not off.

**Behaviour change.** A failing self-test on a quiet boot is re-run with the
console forced on. Silent when healthy, loud when broken.

**Verified.** ⏳ Compiles clean. Soak not yet run against real pads.

---

## 2026-09-28 — A recovered pad waits to be collected

**What changed.** A recovered balance covering a whole pad is now **held**
rather than dispensed on boot. A button press or a coin releases it.

**Why.** Power comes back at three in the morning, the machine wakes owing a
pad, and drops one into an empty washroom. Dispensing to nobody is not
honouring the debt — it is losing a pad on top of the money.

**Also fixed.** `REMEMBER_CREDIT_ACROSS_POWER_LOSS = false` did not fully
remove persistence: `setup()` still called `prefs.begin()` unconditionally. The
NVS partition is now never opened when the feature is off.

**Verified.** ✅ On hardware — both release routes seen in the log.

---

## 2026-09-27 — The unsigned underflow that broke KSh 5 and 10

**The bug.** Part payment was cleared on the very pass it was banked. Signature:

```
cleared after an actual wait of 4294967 s (limit 120 s)
```

2³²−1 ms. `lastCoinMs` was *ahead* of the loop's `now`, so `(now - lastCoinMs)`
wrapped to 4.29 billion and every timeout test built on it fired instantly.

**Cause — one word, a regression.** `loop()` reads `now` once at the top. The
original code stamped `lastCoinMs = now`. When banking moved into `bankCoin()`
it became `lastCoinMs = millis()`, which runs later in the same pass and is
therefore larger than `now`.

**Why only 5 and 10.** KSh 20 and 40 meet the price and vend before the timeout
check is ever reached. Only partial credit goes down that path.

**Fix.** Added `sinceMs()` to measure, used the existing wrap-safe `reached()`
to decide, and converted **all eight** comparisons of the stale `now` against a
stamp written later in the same pass. Two were live bugs: this one, and the
backlight sleeping on the first coin pulse.

**Verified.** ✅ On hardware.

---

## 2026-09-26 — Motor noise was being spent as money

**The bug.** KSh 40 vended three pads. Phantom 2-pulse "KSh 5" coins appeared
immediately after every `Motor on`.

**Cause.** Switching the coil induces edges on GPIO 18 indistinguishable from
coin pulses, arriving in ones and twos — the pulse count of the cheapest coin.
Up to **472 induced pulses in one 5-second run**.

**It was a regression of mine.** The older sketch *rejected* mid-vend coins, so
the noise was invisible. Changing to bank them turned it into money.

**Fix.** The coin interrupt counts nothing while the motor runs, nor for
`MOTOR_NOISE_SETTLE_MS` after. Discarded pulses are counted and reported with
their **rate**, because a fixed burst per run and a steady rate scaling with run
time are different faults needing different parts.

**Later finding.** The rate was 70/second steady — **brush arcing**, not the
switching spike. A flyback diode alone does nothing for it; it needs 100 nF
across the motor terminals. The earlier advice was wrong.

**Verified.** ✅ Muting works. ⏳ Suppression not finished — rate is 5–46/s.

---

## 2026-09-25 — The money model

A run of review rounds reshaped how money is handled. The full reasoning is in
[`DESIGN_DECISIONS.md`](DESIGN_DECISIONS.md); the headlines:

- **Pay on delivery, not on motor start.** Charging at motor start meant taking
  money for a pad that never came out
- **The meter survives a power cut.** Without it, a cut with KSh 5 on the meter
  was a clean swallow with no record anywhere
- **Never leave the stored balance above the real one** — that direction can
  vend a pad nobody paid for
- **A full pad's worth of credit never times out**, in any state
- **The retry button is a customer control**, on the front. `attemptClear()`
  was zeroing the credit, which threw away her money at the exact moment she
  was standing there asking for the pad

**Verified.** ✅ All on hardware.

---

## Recurring lesson

Three separate defects in this project came from **claims made without
checking**: that `putUShort()` compares before writing, that it was "three
writes per sale", that clearing a stale balance was strictly safe. Each was
fixed not by writing a more careful comment but by **removing the dependency on
the claim** — a shadow copy makes "identical writes are skipped" true by
construction; a `>` comparison makes the safety argument hold without a caveat.

A second pattern: every time the logic changed, a comment somewhere else kept
describing the old design. The inhibit rule, the state table, and the flash
claim were all the same failure. **A wrong comment is worse than none**, because
the next person reasons from it.
