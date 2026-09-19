# Pads Vending Machine — Coin Acceptor Prototype

Bricapa Technologies · Client: Rebecca Mpiima · Ref `BT-PADVEND-2026-001`
Firmware **v2.0**

Proof-of-concept firmware. Its only job is to demonstrate that **a coin goes in
and a pad comes out.** Battery, sleep mode, cabinet and M-Pesa are deliberately
out of scope for this build.

Everything you need to bring it up, calibrate it and diagnose it is on the
**serial console at 115200**. Type `?` for the command list.

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
| Buzzer | 25 | passive, LEDC or `tone()` depending on core version |

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

## Building it

Install **LiquidCrystal I2C** (Frank de Brabander) via Library Manager. It
declares itself as AVR-only, so the IDE prints an architecture warning on
ESP32. Ignore it — the library works.

Board: **ESP32 Dev Module** (`esp32:esp32:esp32`).

Verified to compile clean on **ESP32 Arduino core 2.0.11** — 300 KB flash
(22%), 22 KB RAM. The buzzer is driven through LEDC on core 2.x and through
`tone()` on core 3.x, chosen automatically at compile time, so either core
works without edits.

---

## The serial console

Open the Serial Monitor at **115200**. Set the line ending to **Newline** if
you can; if you leave it on "No line ending" the console still works, it just
takes the command after a short pause in typing.

```
  ?        command list
  s        status — state, credit, counters, loop health
  d        run the full self-test again
  c        print the coin table
  z        scan the I2C bus

  CALIBRATION (lost on reset — write the value into CONFIG)
  p<ksh>   set the pad price          e.g.  p20
  g<ms>    set the coin gap           e.g.  g250
  o<ms>    set the dispense timeout   e.g.  o3000

  BENCH TESTS
  m<ms>    run the motor for <ms> — time one revolution
  t        full test vend, no coin needed
  v<n>     pretend a coin of <n> pulses went in
  i        watch the IR sensor live
  l        LCD test pattern
  b        buzzer test

  x        clear a latched fault
  r        zero the session counters
  h        toggle the heartbeat line
```

**Console changes are not saved.** Nothing in this build writes to flash. The
console is for finding the right number; once you have it, write it into the
CONFIG sections at the top of the `.ino` and reflash.

---

## Boot self-test

Every boot runs a seven-part self-test and prints it. Run it again any time
with `d`.

| # | Checks | Fails the boot if |
|---|---|---|
| 1 | Reset reason, heap, CPU | — (but calls out brownout and crash) |
| 2 | Coin table, price, change policy | table empty, 0-pulse entry, duplicate pulse counts |
| 3 | Pulse timing, coin line at rest | coin gap too close to the debounce |
| 4 | I2C scan, LCD present | — (runs headless instead) |
| 5 | IR polarity, chute clear at rest | — (warns loudly) |
| 6 | Relay pin, timeouts | — |
| 7 | Buzzer chirp | — |

A **fatal** result halts before `attachInterrupt`, so a machine that would
mis-price coins never counts one. The console stays alive so you can find out
why. `HALT_ON_SELFTEST_FAIL` in SECTION 7 turns that off.

Two reset reasons are worth watching for on this machine specifically:

- **BROWNOUT** — the motor is dragging the 12 V rail down. That is what the
  1000 µF is for.
- **PANIC / task watchdog** — the firmware crashed. Say so when reporting it.

---

## Bring-up order

Do these in sequence. Each one isolates a single failure.

**1. Flash and read the self-test.** It tells you what it found before you
touch anything. A blank LCD is not a mystery — section [4] scanned the bus and
printed every address on it.

**2. Teach the acceptor your coins.** Learning mode, thirty-plus real KSh 20
coins, a different pulse count per coin type. Then deliberately try to defeat
it with KSh 1, KSh 5, KSh 10, old-series coins and a 26 mm hardware washer.
That test is half the reason this machine exists.

**3. Drop a real coin and read the console.** Every coin prints five lines:

```
COIN IN      : 2 pulses
RECOGNISED   : KSH 20  (worth KSH 20)
CREDIT NOW   : KSH 20 of KSH 20
MEANING      : price met
WILL VEND    : 1 pad
LEFT OVER    : KSH 0
```

An unrecognised coin says so and tells you the exact line to add to
`COIN_TABLE`. Nothing is ever swallowed silently. Make SECTION 2 agree with
what you saw, then reflash.

If one coin reads as several, the gap is too short — raise it with `g350` until
it reads right, then write it into SECTION 5.

**4. IR sensor,** motor disconnected. Type `i` and wave a pad through the
chute. If `BLOCKED` and `clear` are the wrong way round, flip
`IR_DETECTED_IS_LOW`.

*The KY-032 uses an NE555 and may be unreliable at 3.3 V. If it misbehaves,
move it to 5 V and put a 10 k / 20 k divider on its output — do not feed 5 V
into a GPIO.*

**5. Relay, motor disconnected.** `m500`. One click on, one click off. Confirm
it does **not** click on boot or reset.

**6. Motor, no coil.** `m1000`, `m2000` until you have timed one revolution.
Set `DISPENSE_TIMEOUT_MS` to about double that — try it live with `o<ms>`, then
write it into SECTION 5. Watch the 12 V rail on a meter while it starts; if it
dips enough to reset the ESP32, the self-test will say BROWNOUT on the next
boot.

**7. Whole thing, with pads.** `h` to turn the heartbeat on, then fifty cycles
minimum. Count how many actually drop cleanly. That number is the answer to the
only question that matters.

---

## Configuration

All at the top of the `.ino`, in numbered sections. Nothing below SECTION 7
needs changing to commission a machine.

| Section | What is in it |
|---|---|
| 1 | Pin map |
| 2 | **The coin table** — pulses → shillings, per coin |
| 3 | Price, and what happens to overpayment |
| 4 | IR polarity, relay polarity, LCD address |
| 5 | All timing. Three values are adjustable live from the console |
| 6 | Buzzer tones |
| 7 | What the console does on its own |

### The coin table

A multi-coin acceptor does not say "that was twenty shillings" — it says "that
coin was channel 3", by sending three pulses. SECTION 2 is where you write down
what each channel is worth:

```c
const CoinDef COIN_TABLE[] = {
  //  pulses   value   name
  {      1,      10,   "KSH 10"  },
  {      2,      20,   "KSH 20"  },
};
```

Pulse counts must be unique — the self-test refuses to boot a table with
duplicates, because two coins on one channel is money you cannot account for.

---

## Behaviour

```
IDLE ──coin──► decode pulses ──unknown──► reject tone, logged, not banked
                     │
                  known coin
                     ▼
              credit += value ──below price──► "NEED KSH n MORE"
                     │
                 price met
                     ▼
              check beam is clear
                     │
       not clear ────┴──► FAULT "CHECK SENSOR"   (credit NOT taken)
                     │
                  clear
                     ▼
               DISPENSING ──beam breaks──► DELIVERED ──2.5 s──► IDLE
                     │
              timeout (3 s)
                     ▼
            FAULT "EMPTY OR JAMMED"
```

**The money is taken only once the motor is actually turning.** If the beam
check refuses, the credit stays on the meter rather than vanishing into a fault
the customer did not cause.

**No change is given** — there is no hopper and no second motor. Overpayment
stays on the meter and buys the next pad, which is what
`KEEP_CHANGE_AS_CREDIT` controls.

**Faults latch.** The machine stops and reports rather than reversing or
retrying — the relay drives one direction only, which is a deliberate decision
agreed with the client. Power-cycle, or type `x`.

**A hard motor limit** is checked every pass of the loop, in every state,
independently of the state machine. Even a firmware bug cannot leave the coil
turning. If it ever fires, the console says so and calls it a bug rather than a
jam.

Coins inserted while dispensing or faulted are **logged but not banked**, so
the machine never owes a pad it cannot deliver.

---

## Known limits of this build

Stated here so nothing is claimed that has not been demonstrated.

- **Nothing is saved.** No NVS, no flash writes. Vend count, takings and fault
  count are session-only and reset on power-up. Deliberate for a bench
  prototype; persistence belongs with the cabinet build, not before it.
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
