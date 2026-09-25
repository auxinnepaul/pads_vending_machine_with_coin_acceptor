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

**Be clear about what this switch does and does not buy you:**

- **With `PAD_SWITCH_FITTED` false there is no empty detection at all.** The
  machine cannot know it has run out. That is the shipped configuration, and
  every claim about out-of-stock behaviour below depends on you turning it on.
- **Even switched on, it is not instant.** The reading is debounced for
  `PAD_SWITCH_DEBOUNCE_MS` (400 ms) because a mechanical switch under a
  settling stack of pads chatters badly. There is a window of a few hundred
  milliseconds after the coil empties where the machine still believes it has
  stock and will still take a coin.
- **What saves that customer is pay-on-delivery, not the switch.** A coin taken
  in that window starts a vend, the vend times out with nothing to give, and
  the credit is never taken — so the money is still on the meter, and the
  refill that follows dispenses the pad it is owed. The switch narrows the
  window; the payment model is what makes the window survivable.
- **Stock is a switch, not a count.** It knows "empty" or "not empty", never
  "four left".

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

Install **LiquidCrystal I2C** (Frank de Brabander) via Library Manager.

### That architecture warning is not an error

```
WARNING: library LiquidCrystal_I2C claims to run on avr architecture(s)
and may be incompatible with your current board which runs on esp32
```

It is a **warning**, the sketch compiles, and the library is the right one.
The cause is one line of metadata in the library's own `library.properties`:

```
architectures=avr
```

That file was written when the library only targeted AVR boards, and it has
never been updated. The IDE compares that line against your board and warns on
any mismatch — it has not inspected the code. The library itself only uses
`Wire` and standard Arduino calls, all of which exist on ESP32, which is why it
builds and runs cleanly.

To silence it, change that one line to `architectures=*` in:

```
~/Documents/Arduino/libraries/LiquidCrystal_I2C/library.properties
```

Purely cosmetic, and Library Manager will overwrite it on the next update.
Leaving it alone is fine.

---

## Funny symbols on the screen

If the display sometimes fills with gibberish that looks like Chinese or
Japanese characters, that is a **specific, diagnosable fault**, not a random
glitch — and the character set is the clue.

**What is happening.** The LCD is driven in **4-bit mode**: every byte crosses
the wire as two nibbles. Miss a single nibble and every byte after it is
shifted by four bits. The HD44780's built-in font has **Japanese katakana at
0xA0–0xDF**, so mangled bytes land in that block and the screen fills with what
look like Chinese characters. Nothing about the display knows it has lost sync,
so it stays corrupted until it is re-initialised.

**What makes it miss a nibble.** Anything that dips the rail hard enough. Three
things on this machine do:

| Source | When |
|---|---|
| The motor | every vend — the biggest single dip |
| The coin acceptor | **every coin**, and again at power-up |
| Either relay | whenever a coil de-energises |

**Do not assume it is the motor.** On this build the display corrupted on a
bench with no motor connected at all — it was the acceptor. It fires a gate
solenoid to take or reject every single coin, and that solenoid is an inductor
being switched. The firmware rebuilds the display after all three, but the
suppression below is what actually stops it.

---

## Snubbing the inductive loads

Two inductive things hang off the 12 V rail: the motor and the acceptor's gate
solenoid. Both dump a reverse spike when switched off. Left alone that spike
travels the whole rail, which is what browns out the LCD.

### The flyback diode across the motor

**Part:** `1N5408` (3 A) is the right default. `1N4007` (1 A) works for a small
gearmotor but is marginal on a wiper motor. A Schottky such as `SR5100` is
better still and not needed.

**Orientation — get this right or you short the supply:**

```
        relay COM/NO ─────┬───── MOTOR + ────┐
                          │                  │
                        ──┴──                │   band (cathode) goes
                         ╱ ╲   1N5408        │   to the POSITIVE side
                        ╱   ╲                │
                       ─────────  ◄── band   │
                          │                  │
        12 V GND ─────────┴───── MOTOR − ────┘
```

**The banded end (cathode) goes to the motor's POSITIVE terminal.** The diode
is reverse-biased in normal running, so it does nothing at all. Only when the
relay opens and the motor's field collapses — reversing the voltage across it —
does it conduct and give that energy a loop to die in, instead of throwing it
back down the rail.

Fit it **at the motor terminals**, not at the relay. The wire between them is
inductive too, and a diode at the far end leaves that spike free to travel.

If you fit it backwards it conducts the moment you apply 12 V and becomes a
dead short. Check the band before you power up.

*Side effect, and a welcome one here: a flyback diode makes the motor
freewheel rather than stop dead, which is gentler on the coil and the pads.*

### The coin acceptor

You cannot get inside it to put a diode across its solenoid, so suppress it at
its terminals instead:

- **1000 µF electrolytic + 100 nF ceramic** directly across the acceptor's
  12 V and GND, at the acceptor. The electrolytic absorbs the sag; the ceramic
  handles the fast edge the electrolytic is too slow for.
- Optionally a **TVS diode** (`SMBJ15A` or `P6KE15A`) across the same two
  terminals, band to +12 V. It clamps anything the capacitors do not.

### Star-wire the 12 V rail

This matters as much as the components. Run **separate pairs** from the supply
to each load, rather than daisy-chaining:

```
                    ┌──── coin acceptor        (+ 1000 µF, 100 nF at it)
  12 V supply ──────┼──── relay → motor        (+ flyback diode at the motor)
   (+ 1000 µF)      └──── LM2596 → 5 V ──┬──── ESP32 VIN
                                         └──── LCD      (+ 100 µF, 100 nF at it)
```

Shared wire is shared voltage drop. Daisy-chained, the acceptor's surge appears
directly across the LCD's supply pins. Star-wired, it is mostly confined to the
acceptor's own pair.

### The backlight sleeps, and waking is a rebuild

After `BACKLIGHT_TIMEOUT_MS` (2 minutes) of a genuinely idle machine, the
backlight goes out. The **first coin pulse** brings it back — not the finished
coin. A 40-pulse KSh 40 takes seconds to complete, and she is looking at the
screen from the moment the coin drops.

**Waking is always a full rebuild, never just switching the light on.** That is
what makes the sleep worth having beyond saving the backlight: whatever
happened to the display while nobody was watching is gone before the first
person sees it. The screen is at its freshest at the exact moment somebody
starts using it.

Only **plain idle** sleeps. `OUT OF ORDER` and `SORRY - NO PADS` stay lit —
that message is the whole reason the screen is there. A machine with credit on
the meter stays lit too, because somebody is mid-transaction. Any console
command wakes it, so a service visit is never looking at a dark screen.

The periodic rebuild is skipped while asleep, since waking rebuilds anyway.

`BACKLIGHT_TIMEOUT_MS = 0` keeps it on permanently.

*This is also the groundwork for the battery build. The backlight is nearly all
of what the display consumes — roughly 20 mA against 2 mA for the logic — and
the client study is explicit that it has to sleep when the machine sleeps.*

### And at the display itself

**100 µF electrolytic + 100 nF ceramic across the LCD backpack's VCC and GND
pins**, as physically close as you can solder them. These backpacks ship with
almost no decoupling of their own, and this is frequently the single most
effective fix for exactly this symptom.

**Fix the supply first.** This is the real cure:

1. **Fit the 1000 µF across the 12 V rail**, close to the buck converter. This
   is exactly what it is for.
2. **Fit the flyback diode across the motor terminals**, cathode to positive.
   Without it the coil dumps a large reverse spike onto everything every time
   the relay opens.
3. **Feed the LCD from the buck output directly**, not daisy-chained through
   the ESP32's 5V pin. Star-wire the 5 V rail: buck → each load separately, so
   the motor's dip is not shared through a thin header trace.
4. **Keep SDA and SCL short.** A display on a cabinet door with 40 cm of loose
   jumper wire is an antenna. Twist SDA and SCL with a ground wire, or use
   ribbon with grounds between.
5. **Check the reset reason.** Type `d` after it happens. If section [1] says
   `BROWNOUT`, the dip is bad enough to reset the whole board and items 1–3 are
   not optional.

**The firmware also self-heals.** `motorOn()` sets a flag, and the next screen
write re-initialises the display from scratch before painting. So corruption
introduced by a vend is cleared by the very next message — normally within
about two seconds, rather than persisting until someone power-cycles the
machine. It costs ~50 ms once per vend.

That is belt-and-braces, **not a substitute for fixing the supply.** If you can
see garbage appear and then clear on the next message, the firmware is doing
its job and your rail still needs the capacitor.

**To test recovery by hand:** type `l`. It forces a full re-initialisation and
draws a test pattern. If that clears the screen but it goes bad again on the
next vend, it is the motor, and the list above is your fix.

The I2C bus is also set explicitly to 100 kHz — the slowest standard rate and
the most tolerant of long unshielded runs. Nothing on this bus needs to be
faster.

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
  w<ms>    part-payment wait          e.g.  w120000
           w0 = wait forever, never clear part payment

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

**Console changes are not saved.** The console is for finding the right number;
once you have it, write it into the CONFIG sections at the top of the `.ino`
and reflash. The **meter balance** is the one and only thing this firmware
keeps in flash — see "Surviving a power cut" below.

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

### Part payment and the countdown

Somebody puts **KSh 10** into a **KSh 20** machine. The screen keeps asking for
the rest and counts down, refreshed once a second:

```
CREDIT KSH 10
ADD 10 IN 1:35
```

Default wait is **two minutes** (`CREDIT_TIMEOUT_MS`, SECTION 5). If the
countdown reaches zero the credit is cleared and the machine returns to
`INSERT COIN`.

**Set `CREDIT_TIMEOUT_MS = 0` to wait forever.** Part payment then never
expires — the screen just keeps asking, with no countdown:

```
CREDIT KSH 10
ADD KSH 10 MORE
```

Try either live with `w<ms>`; `w0` is forever. Shorten it to something like
`w15000` to watch the countdown without standing there for two minutes.

### Change from a vend lands on the same screen

`KEEP_CHANGE_AS_CREDIT` means overpayment stays on the meter, so there are two
different routes to having part payment:

- she paid **part** of the price and stopped, or
- she **overpaid**, a pad came out, and the remainder was handed back onto the
  meter.

KSh 5 on the meter, she inserts KSh 20 → total 25 → a pad is dispensed → KSh 5
change → straight back to asking for the rest:

```
CREDIT KSH 5
ADD 15 IN 1:55
```

**Both routes land on exactly the same screen**, because one function draws it.
A customer with KSh 5 showing needs to be told to add 15 and how long she has,
and that must not depend on how she got there.

KSh 40 into a KSh 20 machine vends two pads back to back for the same reason —
after the first, credit is still 20, so the machine vends again before it goes
back to idle.

**The honest trade-off.** There is no refund path — the coin banks into the
housing before the firmware hears a pulse — so no value here gets the money
back to the person who paid it. All this number decides is who ends up with it:

| Setting | Who gets abandoned part-payment |
|---|---|
| Shorter | the business keeps it |
| Longer | she gets a better chance to find the second coin |
| `0` | it always eventually buys a pad — but for whoever walks up next |

Two minutes is the default because it is long enough to find a coin, ask a
friend, or give up honestly. Twenty seconds, which this used to be, was not.

The countdown repaints **line 2 only**, without clearing the screen, so a
once-a-second refresh does not flicker.

### Surviving a power cut

Somebody puts KSh 5 into a KSh 20 machine and the mains drops. Without
protection that is a **clean swallow**: the coin is in the box, the firmware
forgets it ever arrived, and nobody — not the customer, not the log, not the
takings — has any record of it. Mains cuts are not hypothetical at a Kenyan
site.

So the meter balance is written to flash whenever it changes, and restored at
boot:

```
RECOVERED KSH 5 left on the meter when the power went.
  Part payment. The countdown restarts now, so it expires 120 s
  from this boot rather than sitting here indefinitely.
```

**The countdown restarts from boot rather than resuming.** That is the detail
that makes this safe. A short cut protects the customer who is still standing
there. A long one must not leave a stranger's balance on the screen all night
for whoever opens up in the morning — so a restored balance expires
`CREDIT_TIMEOUT_MS` after the power returns, and then the machine is clean.

If the restored balance already covers a whole pad, the machine vends it on
boot. That is a pad already paid for.

Turn it off with `REMEMBER_CREDIT_ACROSS_POWER_LOSS` in SECTION 3.

**What it costs.** One write per coin, plus one on the vend. With KSh 5 coins
and a KSh 20 pad that is **five writes for a single sale** — the smallest coin
in the table sets the worst case, so re-check this if you ever add a smaller
one. Roughly 90,000 writes a year at fifty sales a day.

Still not a wear problem, but for a different reason than you might assume:
NVS packs many writes into a flash page before erasing anything, and
wear-levels those erases across the partition, so the flash sees far fewer
erase cycles than the firmware does writes. Decades, comfortably.

Redundant writes are skipped by comparing against a shadow copy of what is
actually stored. **`putUShort()` does not do this for you** — it calls
`nvs_set_u16()` and commits every time, whatever the old value was.

**If flash stops accepting writes**, the machine says so loudly, once, and
keeps trading — refusing sales because flash is unhappy turns a degraded
machine into a dead one. `s` and `d` both keep reporting it, so the power-cut
protection is never quietly absent:

```
balance saved: NO — a power cut will lose it
```

**What it does not fix.** It restores the *claim*, not the cash. Nothing can
return the coin. And there is a window of a few milliseconds between a pad
being delivered and the new balance reaching flash — a cut in exactly that gap
means the machine vends one extra pad on boot. That errs toward the customer,
which is the right direction to err.

### The customer pays on delivery

**Nothing comes off the meter until the beam confirms a pad actually fell.**
The motor starting does not cost anything; only the pad landing does.

### Why it has to work this way

**The coin is in the cash box before the firmware knows it exists.** The
acceptor banks a coin first and only then sends its pulses. By the time this
code hears anything, the money has already dropped into the housing and there
is no eject, no return chute and no hopper anywhere in this machine.

So inhibiting the acceptor stops the **next** coin. It cannot give back one
already taken, and nothing else can either.

That single fact shapes the whole money design. The firmware cannot refund
cash, so what it protects instead is **the claim**: the machine's record that
it owes you a pad. Charging on delivery means a jam, an empty coil, a mis-aimed
sensor or a missed beam all leave that claim intact on the meter — and it is
honoured as soon as the machine can serve it.

Be clear about the distinction. Pay-on-delivery does **not** get anybody their
money back; nothing can. It means the machine does not also forget it owes them
a pad.

There is exactly one line in the firmware that spends credit, and it runs after
the beam has confirmed delivery.

Two consequences worth knowing:

- **A failed vend does not lose the claim.** The machine goes to FAULT or EMPTY
  with the credit intact. If it went to EMPTY, a refill puts it straight back
  in service and that credit buys the pad it was owed.
- **A full pad's worth of credit never times out.** Part payment is forgotten
  after `CREDIT_TIMEOUT_MS` so the next customer does not walk up to somebody
  else's money — but credit that already covers a pad has been paid for, and it
  sits and waits, in IDLE and in EMPTY alike. Wiping it would put money in the
  box with no pad out of it, which is the exact failure charging on delivery
  exists to prevent.
- **Clearing a fault does wipe the credit**, on purpose. By the time somebody
  unlocks the door the customer is long gone, and carrying it forward would
  just hand the next person a free pad. The console prints the amount it is
  clearing so it is a decision, not an accident.

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

### When the coin slot is open

One rule, derived from the machine's state in one place in `loop()` and nowhere
else:

**The slot is live in every state except `EMPTY` and `FAULT`** — the two where
the machine cannot honour a coin.

It deliberately stays open **during a vend.** A coin that lands mid-vend is
banked and buys the next pad, so there is nothing to protect against, and
inhibiting for every sale would work the relay twice per vend for no benefit —
thousands of needless operations over the life of a machine. The relay only
moves when the machine genuinely goes out of service, which should be rare.

Deriving it from state in a single place means the slot can never disagree with
what the machine is doing. There is no way to enter a state and forget to open
or close it, because nothing else touches it.

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

**A coin that lands mid-vend is honoured, not discarded.** Inhibiting the
acceptor only stops the *next* coin — one already rolling through the mechanism
finishes its pulse burst after the vend has started, and the machine physically
has that money. It goes on the meter and buys the next pad, because the machine
is about to be back in service anyway. Throwing it away would mean keeping the
money and giving nothing back.

Coins that land while **faulted or out of stock** are logged but **not** banked.
There the machine genuinely cannot promise a pad, so it must not take the money
onto the meter. Those coins are still lost — there is no hopper and nothing to
refund with — but they are reported rather than pocketed in silence.

---

## Known limits of this build

Stated here so nothing is claimed that has not been demonstrated.

- **Only the meter balance survives a reboot.** Vend count, takings and fault
  count are session-only and reset on power-up. That is deliberate for a bench
  prototype — a proper vend log belongs with the cabinet build. The balance is
  the exception because losing it silently swallows a customer's money.
- **No speed control.** The relay has no PWM. Dispensing speed is fixed by the
  motor you buy and can only be changed by changing the motor.
- **No reverse**, so no automatic jam recovery.
- **Mains only.** Battery, sleep mode and the wake button are stage two. The
  3–4 week battery figure in the client study is still arithmetic, not a
  measurement.
- **One pad product.** The coil is wound to one pitch.
- **Abandoned part-payment is eventually lost, and that is a policy choice you
  set.** See "Part payment and the countdown" above. `CREDIT_TIMEOUT_MS = 0`
  turns it off entirely. Credit that already covers a whole pad is **never**
  timed out at any setting.
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
