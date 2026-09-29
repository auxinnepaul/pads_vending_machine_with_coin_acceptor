# Firmware progress

The living memory for this project. **Read the RESUME block first.** Update it
every time the work moves on, and add a dated section saying what changed, why,
what the behaviour change is, and whether it is verified on hardware.

Mark **✅ verified on hardware** vs **⏳ pending** explicitly. A thing that
compiles is not a thing that works.

---

# ▶ RESUME HERE

**State as of 2026-09-28.**

### ⛔ Blocker — the board stopped responding (2026-09-28, later)

**The ESP32 on COM3 is electrically silent.** Everything below about coins is
still true of the firmware; none of it can be re-tested until the board talks.

This is **not** the toolchain. A full disk had been making `esptool` fail while
unpacking its own runtime, which masked the real fault. With the disk cleared
(16.3 GB free) `esptool` runs correctly and reports `No serial data received`.

**Cause found (measured):** inductive kickback from the coin acceptor's
solenoid puts 24–36 V (true peak likely >100 V) onto the COIN signal line that
feeds a 3.6 V-max GPIO. **Three boards lost.** Do not connect another ESP32
until the coin line is optocoupled — see the dated section below.


The firmware is functionally complete and every coin path is verified on real
hardware. What remains is **mechanical and electrical**, not code.

### Verified on hardware ✅

- All four coins decode: 2 / 10 / 20 / 40 pulses → KSh 5 / 10 / 20 / 40
- Accumulation: 4 × KSh 5 and 2 × KSh 10 both reach KSh 20 and vend
- KSh 40 vends two pads back to back
- Change carries across a vend and lands on the correct screen
- Meter survives a power cut; recovered pad is held and released by button or coin
- Jam retry, the three-attempt cap, and the customer button
- Coin-noise muting (the machine no longer spends induced pulses)

### Pending ⏳ — all of it physical

| | Blocked on |
|---|---|
| **Real drop times** | No chute or panel. Every timing so far is a finger at the IR sensor, which measures how fast a hand moves |
| **`DISPENSE_TIMEOUT_MS`** | Currently **7000 ms placeholder**. Run `T25` once pads fall down a real chute and set twice the slowest |
| **Do pads feed off the coil without jamming** | The number one risk in the whole project, still entirely open |
| **Motor noise → zero** | Down from 70/s to 5–46/s. Needs 100 nF across the motor terminals |
| **Inhibit relay polarity** | Run `n0` / `n1` and set `INHIBIT_RELAY_ACTIVE_HIGH` from what the relay does |
| **LCD** | Not wired. I2C scan finds nothing |

### Next session should

1. Wire the LCD — everything the customer sees is currently invisible
2. `n0` / `n1`, settle the relay polarity, fix the pull-up/pull-down note
3. 100 nF across the motor terminals, watch `motor noise` in `s` fall
4. Build the chute, load the coil, run `T25`, bake the timeout in
5. Then the 50-cycle soak — that number is what the client actually asked for

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
