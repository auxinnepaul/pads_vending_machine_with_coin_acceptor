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
| Relay module, 2 channel, active-high | ASK Electronics | 300 |
| Momentary push button (jam clear, inside the door) | ASK Electronics | 50 |
| Limit switch (pad stock sense) | ASK Electronics | 100 |
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
| Coin inhibit | 19 | relay 2 → HX-916 2-pin port. Active LOW, **10 kΩ pull-UP to 3V3** |
| Jam clear | 23 | momentary button to GND, `INPUT_PULLUP`, inside the locked door |
| Pad stock | 33 | limit switch to GND, `INPUT_PULLUP`. Closed = pads present |

Avoided: GPIO 0, 2, 5, 12, 15 (strapping), 1 and 3 (UART), 6–11 (flash).

### Three things that kill hardware

1. **Set the LM2596 to 5.0 V with a meter before the ESP32 is connected.**
   It ships at whatever the pot happens to be.
2. **Measure the coin acceptor's pulse output before it touches GPIO 18.**
   If it swings to 12 V, fit a divider or optocoupler. ESP32 pins are not 5 V
   tolerant, let alone 12 V.
3. **Fit the 10 kΩ pull-down on GPIO 26** so the motor stays still through boot
   and through every reset while flashing.
4. **Fit a 10 kΩ pull-UP to 3V3 on GPIO 19** — a pull-*up*, the opposite of
   GPIO 26, because the inhibit relay module is active LOW. The pin floats
   until `setup()` runs; if it sits low through that window the relay
   energises, the NC contact opens and the coin slot is live on every reset.
   Confirm with your ear: reset the board with the acceptor connected and
   listen for a click.

### Wire the pad stock switch closed-when-full

One side to GPIO 33, the other to GND. Nothing else — the internal pull-up
does the rest.

**Closed (LOW) = pads present. Open (HIGH) = empty.** That direction is the
safe one and it is worth being deliberate about. A broken wire, a pulled
connector or a corroded contact all leave the pin floating HIGH, which reads
EMPTY, so the machine stops taking money. Wired the other way round, those same
faults read FULL and the machine cheerfully sells pads it does not have. **A
failure should cost you a sale, never a customer's money.**

`PAD_SWITCH_FITTED` in SECTION 4 ships **false**, which bypasses the check
entirely. Set it true once the switch is actually wired — until then an unwired
pin floats HIGH, reads EMPTY, and the machine would sit refusing to vend while
you wonder why. The self-test says which mode it is in on every boot.

### Wire the inhibit relay to the NC contact

The second relay shorts the HX-916's 2-pin port to inhibit it. Use the
**NC (normally closed)** contact, not NO. With NC, the contact is closed —
acceptor inhibited — whenever this board is unpowered, booting, halted or
crashed. A dead controller refuses money instead of eating it. `INHIBIT_CONTACT_IS_NC`
in SECTION 4 must match how you actually wired it, and the self-test prints
which one it thinks you used.

Drive it from a **dry relay contact, never a GPIO** — on some HX-916 units
that pin sits at 12 V.

---

## Building it

Install **LiquidCrystal I2C** (Frank de Brabander) via Library Manager. It
declares itself as AVR-only, so the IDE prints an architecture warning on
ESP32. Ignore it — the library works.

Board: **ESP32 Dev Module** (`esp32:esp32:esp32`).

Verified to compile clean on **ESP32 Arduino core 2.0.11** — 303 KB flash
(23%), 22 KB RAM. The buzzer is driven through LEDC on core 2.x and through
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
  k        watch the pad stock switch live
  l        LCD test pattern
  b        buzzer test

  x        clear a latched fault
           (or hold the front-panel button for 1 s)
  r        zero the session counters
  h        toggle the heartbeat line
  L        toggle event logging (off for field installs)
```

### Logging in the field

`SERIAL_LOG_DEFAULT` in SECTION 7 silences the routine chatter — every coin,
vend, warning and fault — for an installed machine. Set it `false` before you
flash a unit that is going out.

It does not make the machine mute. The boot banner and the **whole self-test
still print, always**, so a reset during a service visit still tells you
everything. And **every typed command still answers in full**: plug a laptop in,
press `s`, and you get the complete status whatever the switch is set to. `L`
turns the running commentary back on for as long as you want it.

**Console changes are not saved.** Nothing in this build writes to flash. The
console is for finding the right number; once you have it, write it into the
CONFIG sections at the top of the `.ino` and reflash.

---

## Boot self-test

Every boot runs a nine-part self-test and prints it. Run it again any time
with `d`.

| # | Checks | Fails the boot if |
|---|---|---|
| 1 | Reset reason, heap, CPU | — (but calls out brownout and crash) |
| 2 | Coin table, price, change policy | table empty, 0-pulse entry, duplicate pulse counts |
| 3 | Pulse timing, coin line at rest | coin gap too close to the debounce |
| 4 | I2C scan, LCD present | — (runs headless instead) |
| 5 | IR polarity, chute clear at rest | **chute blocked at startup** |
| 6 | Relay pin, timeouts | — |
| 7 | Inhibit contact type, clear button at rest | — (warns if wired NO) |
| 8 | Stock switch, or a reminder that it is bypassed | — |
| 9 | Buzzer chirp | — |

A **fatal** result halts before `attachInterrupt`, so a machine that would
mis-price coins never counts one, and the acceptor stays inhibited. The console
stays alive so you can find out why. `HALT_ON_SELFTEST_FAIL` in SECTION 7 turns
that off.

Because a blocked chute is fatal, a power cut with one pad stuck will halt the
machine on the way back up. That is deliberate — the next vend would have
faulted anyway. Recovery is the clear button, and clearing from a halted boot
**re-runs the whole self-test** and refuses to return the machine to service
until it passes. Only then is the coin interrupt attached.

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

**3. Drop a real coin and read the console.** Every coin reports itself in
full:

```
COIN IN      : 20 pulses
PULSE TIMING : 1880 ms burst, gaps 94-99 ms
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

**3b. Check the pulse timing.** The `PULSE TIMING` line on every coin gives the
burst length and the min/max gap between pulses. Drop one of each coin and note
the widest gap you see — that is your margin against a coin splitting in two.
Then insert two coins as fast as the acceptor will take them and confirm they
still read as two.

**3c. Fit the stock switch,** then set `PAD_SWITCH_FITTED` true and reflash.
Type `k` and work the lever: you want `PADS PRESENT` with the coil loaded and
`EMPTY` with it empty. If those are the wrong way round, flip
`PADS_PRESENT_IS_LOW`. Out of stock is **not** a fault and does not latch — the
machine inhibits coins, shows `SORRY - NO PADS`, and returns to service by
itself the moment the switch says pads are back, with no reset needed.

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
  {      2,       5,   "KSH 5"   },
  {     10,      10,   "KSH 10"  },
  {     20,      20,   "KSH 20"  },
  {     40,      40,   "KSH 40"  },
};
```

These are the counts **measured on the bench** from this acceptor, not
defaults. At `PAD_PRICE = 20` with change kept on the meter they give the
client exactly what was agreed: four KSh 5, or two KSh 10, or one KSh 20 buys
one pad; one KSh 40 buys two, one after the other.

Every coin also reports its own pulse timing, which is what tells you whether
`COIN_GAP_MS` is safe:

```
COIN IN      : 40 pulses
PULSE TIMING : 3840 ms burst, gaps 92-104 ms
```

The gap has to sit in a window with a wall on each side. Too **low** and one
coin splits into two — the customer is short-changed. Too **high** and two
coins inserted quickly merge into one unknown coin and the money is lost. The
acceptor cannot deliver two coins closer than about 600 ms, so keep it well
under that. The firmware warns on the console if either margin gets thin.

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
                     │
       ┌─────────────┴─────────────┐
  stock says empty            stock says full,
                              or no stock switch
       ▼                             ▼
     EMPTY                     FAULT "JAMMED"
 (self-recovers)              (needs the button)
```

IDLE also drops to **EMPTY** the moment the stock switch opens, and climbs
back out on its own when it closes.

**The money is taken only once the motor is actually turning.** If the beam
check refuses, the credit stays on the meter rather than vanishing into a fault
the customer did not cause.

**No change is given** — there is no hopper and no second motor. Overpayment
stays on the meter and buys the next pad, which is what
`KEEP_CHANGE_AS_CREDIT` controls.

**Faults latch, and stop taking money.** On any fault the second relay
inhibits the coin acceptor, so a jammed or empty machine physically refuses
coins instead of eating them. The LCD rotates `OUT OF ORDER` over the reason,
`NO COINS TAKEN`, and `HOLD RESET BTN`.

**To clear:** hold the button inside the locked door for 1 second, or type `x`.
Either route re-checks the beam first and **refuses to clear while the chute is
still blocked** — clearing with a pad still wedged would just fault again on
the next coin, after taking the money. A fault that arrives while the button is
already held cannot ride that hold into clearing itself; it has to be released
and pressed fresh.

The acceptor is also inhibited for the duration of every vend, so a coin cannot
arrive mid-dispense, and for as long as the stock switch says the coil is empty.

**Out of stock is not a fault.** It does not latch and it does not need a
reset: the machine inhibits coins, shows `SORRY - NO PADS`, and returns to
service on its own the moment the switch says pads are back. Credit already on
the meter survives the refill, or times out like any other partial payment.

**A dispense that times out asks the stock switch which it was.** Running dry
and jamming want opposite handling — one is routine and must come back by
itself after a refill, the other needs somebody to open the door. If the switch
says the coil is empty the machine goes to EMPTY, not to a latched fault, so a
site that simply ran out does not sit dead waiting for a call-out. With no
stock switch fitted it cannot tell, so it stays cautious and latches
`EMPTY OR JAMMED` as before.

Clearing a jam on an empty coil goes straight to EMPTY rather than flicking
into service for the one pass it would take the stock check to notice.

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
- **Stock is a switch, not a count.** The machine knows "empty" or "not empty",
  not "four left". A low-stock warning before a site runs dry needs the
  count-down from the last refill described in the client study, and that needs
  somewhere to store the count — which this build deliberately does not have.
- **A coin already in the slot when a fault hits is still lost.** The inhibit
  relay stops the *next* coin, not one that is already being read. Firmware
  logs it and refuses to bank it, so the machine never owes a pad it cannot
  deliver, but it has no way to hand the money back.
- **95% coin recognition** is the acceptor's rating. Roughly one good coin in
  twenty is refused and must be re-inserted. Expect it at the demo; it is not
  a fault.
