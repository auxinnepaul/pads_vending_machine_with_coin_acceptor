# Lessons Learned — Pad Dispenser

What went wrong on this project, what it cost, and the rule we took from each.
Written for two readers: someone building this machine, and us on the next
project, which will have different parts and the same kinds of mistakes.

Every entry here actually happened. The dates point to the full account in
`FIRMWARE_PROGRESS.md` or the git log. Some of the mistakes were made by the
AI assistant working on the firmware; they are listed as plainly as the
others, because they are the ones most likely to happen again.

The short version, if you read nothing else:

1. **Never connect an inductive 12 V device's signal wire straight to a 3.3 V
   microcontroller pin.** Optocouple it. This cost three boards.
2. **Measure, don't reason.** Most wrong turns here came from a plausible
   explanation nobody had tested.
3. **A signal that measures 0 V at rest can still carry a 36 V spike.**
   A multimeter check at idle proves nothing about transients.
4. **Design for the dead controller.** Ask what the machine does when the
   ESP32 is off, booting or dead, and make that state the safe one.
5. **A wrong comment or a wrong diagnostic is worse than none.** The next
   person reasons from it.
6. **A 3.3 V board cannot turn off a 5 V active-LOW relay module.** It needs
   close to 5 V on its input to release, so it switches on and stays on. Use
   active-HIGH modules with an ESP32, or drive the input through a transistor
   (lesson 1.4).
7. **A consistent reading can still be wrong.** The coin table matched the
   counts perfectly for weeks, and every count was double. Calibrate from the
   raw signal, not from what your own code reports (lesson 2.10).

---

## Part 1 — Electrical

### 1.1 The coin line killed three ESP32s

**What happened.** The coin acceptor's COIN output was wired straight to
GPIO 18 with a 10 kΩ pull-up to 3.3 V, the way most tutorials show it. The
machine worked for whole sessions. Then boards started dying: first one went
completely silent, then another, then a third.

**Why.** The acceptor contains a solenoid. Plugging or unplugging it with the
12 V on made that coil's stored energy dump a spike onto the COIN wire. A
multimeter caught **24 V and 36 V**, and a multimeter is far too slow to show
the true peak. The GPIO is rated for **3.6 V**. The post-mortem confirmed it:
one dead board had GPIO 18 shorted to ground, where the spike came in; another
had its 3.3 V rail shorted, one step further along the same path.

**Why nobody caught it.** The COIN line measures **0 V at idle**, so the
standard advice ("measure the output first; if it swings to 12 V, use an
optocoupler") passed. The danger was only ever in the transients.

**Why the tutorials didn't warn us.** Most are written for 5 V Arduinos, which
tolerate far more abuse than a 3.3 V ESP32.

**Lesson.** Any signal from a device that contains a coil, motor or solenoid
goes through an **optocoupler** (a PC817 costs KSh 25). It passes light, not
voltage, so nothing on the acceptor's side can reach the pin. After fitting
it, unplugging and replugging the acceptor no longer moved the GPIO-side
reading at all.

### 1.2 Multiple dead boards mean a circuit fault, not bad luck

**What happened.** The first dead board looked like an accident. So did the
second.

**Lesson.** One failure can be chance. **Two failures of the same kind are
the circuit telling you something.** Stop, find the cause, and don't connect
another board until you have. Each new board on an unchanged bench just
becomes the next casualty.

### 1.3 Hot-plugging is where the damage happens

**What happened.** The acceptor was being connected and disconnected while
everything else was running.

**Lesson.** On a machine with coils: **switch the supply off before plugging
or unplugging anything, and connect ground first.** That's necessary but not
enough on its own. The solenoid also switches during normal operation, every
time it rejects a coin, so procedure alone never removes the cause.
Protection has to be in the circuit.

### 1.4 A 3.3 V pin cannot turn off a 5 V active-LOW relay module

**What happened.** The inhibit relay switched on but never released. It was
eventually settled with the plain Blink example: GPIO 19 toggling, nothing
else running, relay never moved. A jumper from 5 V released it; a jumper from
3.3 V did not.

**Why.** An active-LOW module's input has its own pull-up to 5 V through the
opto LED. To switch it off, the input must rise close to 5 V. An ESP32 "high"
is only 3.3 V, which leaves about 1.7 V across the LED, and that is still
enough to keep it on.

**The trap inside the trap.** The motor relay, an active-HIGH module, worked
perfectly from 3.3 V, and that was taken as proof the 3.3 V theory was wrong.
It wasn't proof. They're different circuits. A working active-HIGH relay
tells you nothing about an active-LOW one.

**Lesson.** With a 3.3 V microcontroller, **buy active-HIGH relay modules.**
If you're stuck with an active-LOW one, drive it through an NPN transistor.
And when evidence is offered against a theory, check the evidence applies to
the same circuit before giving the theory up.

### 1.5 Floating pins during boot

**What happened.** The relay pins had no pull-down resistors. The firmware
documented that they needed them; the bench didn't have them.

**Why it matters.** Between power-on and `setup()`, every ESP32 pin floats.
A floating relay pin can drift high: the motor twitches and drops a pad
nobody paid for, or the inhibit relay opens the coin slot while no firmware
is running to count coins. The ESP32's internal pull resistors don't help
here: they are disabled at reset, which is exactly the window you need
covered.

**Lesson.** Every output that drives a relay, motor or valve gets an
**external** resistor that holds it in its safe state through boot. Test it:
press reset a few times with everything connected and listen. **No relay may
click.**

### 1.6 Pick the fail-safe contact, not the energy-saving one

**What happened.** We chose between wiring the inhibit relay's NO or NC
contact. NO keeps the relay off most of the time, which seems tidier.

**Why NC is right.** With NC, a dead, crashed or unpowered controller leaves
the contact closed, so the acceptor is inhibited. With NO, a dead controller
leaves the slot open and the machine **keeps taking money with nothing
running to honour it.** The relay costs 0.35 W to hold on. That's nothing.

**The same reasoning ruled out a MOSFET** across the inhibit port. It would
have worked, but a MOSFET switches off when its controller dies, which would
leave the slot open. A relay's contacts are also separated from the coil by
an air gap, which a MOSFET never is.

**Lesson.** For every output, ask: **what does this do when the controller is
dead?** Make that the safe state, even if it costs a little power.

### 1.7 A MOSFET is not an isolator

**What happened.** The question came up whether MOSFETs "are not connected to
the ESP32" because they switch a motor.

**Why not.** The gate wire goes straight to the GPIO. The gate is insulated
from the channel only by a thin oxide layer, which breaks down at about 20 V,
and MOSFETs usually fail shorted. A dead MOSFET can put the motor supply on
the pin.

**Lesson.** Isolation comes from **optocouplers and relays**. MOSFETs give
you control, not protection.

### 1.8 Part numbers that are one letter apart

**What happened.** The IRLZ44N wasn't in stock. The shop's list had an
**IRFZ44**, which looks almost identical.

**Why it matters.** `IRL` means logic-level: fully on at 3.3–5 V. `IRF`
parts need about 10 V on the gate. From an ESP32 they sit half-on, heat up,
and starve the motor. The ready-made "IRF520 MOSFET module" has the same
problem, although it's sold "for Arduino".

**Lesson.** Read the whole part number, and check the gate threshold and the
voltage at which `R_DS(on)` is specified. On a listing with no specs, assume
nothing.

### 1.9 Where the power goes matters more than the amps

**What happened.** An AMS1117 regulator module was considered for powering
the ESP32 from 12 V, and a small buck converter was preferred "because it
has more amps".

**Why the buck was right for a different reason.** A linear regulator burns
off the extra voltage as heat: `(12 − 3.3) V × 0.5 A ≈ 4.4 W`, when it can
shed about 1 W. It would overheat. The buck converts instead of burning.

**Also.** The ESP32 board's own AMS1117 is fine when fed 5 V. Feeding **5 V
into VIN** keeps that regulator between the buck and the chip. A trimpot buck
that drifts can then damage the regulator, not kill the ESP32 outright.

**And.** An ordinary silicon diode in the supply to VIN drops 0.7–1.0 V and
leaves the 3.3 V regulator too little headroom. The ESP32 then resets
whenever the motor starts. Use a Schottky (about 0.3 V).

**Lesson.** Work out where each regulator's lost power goes before choosing
it. Set trimpot supplies with a meter, **with nothing connected**, before any
board goes on.

### 1.10 A regulator that dies can pass its input through

**What happened.** The optocoupler protects the data line, but the ESP32
still shares the 12 V supply through the buck.

**Why it matters.** A buck regulates normally, but a spike above its input
rating (40 V for an LM2596) can short its switch. A shorted switch connects
12 V straight to the 5 V rail, and everything on that rail dies together.

**Lesson.** Protect the input of the regulator, not just the signal lines. A
TVS diode across the supply, plus bulk capacitance, is cheap insurance.
Isolation has to cover **both** routes into the controller: signal and power.

### 1.11 Two kinds of motor noise need two different parts

**What happened.** The motor's electrical noise was showing up as fake coin
pulses. The first advice, a flyback diode, didn't fix it.

**Why.** Reporting the noise as a **rate** exposed it: a steady 70 pulses a
second is brush arcing, not the switch-off spike. A flyback diode only fixes
the switch-off spike. Brush arcing needs a small ceramic capacitor (47–100 nF)
across the motor terminals.

**Lesson.** Measure what kind of noise you have before choosing the fix. A
fixed burst per switching event and a steady rate while running have
different causes.

### 1.12 Capacitors: local and fast beat far away and big

**What happened.** Two questions came up: "the electrolytic is enough, why add
a 100 nF?" and "why a second 1000 µF at the acceptor if there's one at the
supply?"

**The answers.** A big electrolytic reacts too slowly for fast switching
edges; a small ceramic beside it handles those. They do different jobs. Wires
have inductance, so a capacitor far away can't deliver a sudden current burst
quickly. But the builder was right that a second 1000 µF at the acceptor was
overkill on a short cable. A 100 nF at the acceptor end is enough.

**Lesson.** Pair every electrolytic with a 100 nF ceramic. Put decoupling
where the current is drawn. Size it to the wire length, not by habit.

### 1.13 A rail dip corrupts a display that has no idea it's corrupted

**What happened.** The LCD filled with Japanese-looking characters. At first
this was blamed on the motor; the builder saw it happen with **no motor
connected**, just from powering the acceptor.

**Why.** A supply dip browns out the LCD controller while the ESP32 keeps
talking to it. The LCD runs in 4-bit mode, and one lost half-byte shifts
every following byte into the katakana part of its character set. The
display never knows, so it stays wrong.

**Lesson.** Every inductive load on the rail can cause it, not just the
obvious one. The firmware now rebuilds the display after each event known
to dip the rail, and once a minute as a backstop. The real cure is still the
supply: capacitors, star wiring, short I²C wires.

### 1.14 Keep low-level signal wires away from relay wires

**What happened.** The inhibit relay switching made the pad stock switch
flicker between EMPTY and PRESENT several times a second. That loop reset the
board twice.

**Lesson.** Route switch and sensor wiring separately from relay and motor
wiring, decouple the relay module's supply, and bring all grounds to one
point. Firmware can ignore the flicker, but only the wiring removes it.

---

## Part 2 — Firmware

### 2.1 The money comes first: protect the claim, not the cash

**What happened.** The acceptor banks a coin **before** it tells the firmware.
There is no eject and no refund. Several early designs lost the customer's
money: charging when the motor started rather than when the pad fell; the
jam button zeroing the credit at the exact moment she was asking for her pad;
a power cut silently wiping a part payment.

**Lesson.** Find the one physical fact your system cannot change, and derive
the rules from it. Here: the firmware can't protect the cash, so it protects
its own record of what it owes (`DESIGN_DECISIONS.md`). Pay on delivery; keep
the balance through a power cut; never let a retry or a timeout destroy a
claim.

### 2.2 Not all errors are equally bad

**What happened.** A safeguard cleared any stored balance that didn't match
the live one. That wiped out a legitimate part payment whenever a later
write failed.

**Lesson.** Look at which direction an error goes. A stored balance **higher**
than reality can hand out a pad nobody paid for, so clear it. One **lower**
can only under-pay a customer, so keep it. A rule that treats both the same
is too blunt.

### 2.3 Unsigned time arithmetic and a stale "now"

**What happened.** Part payment cleared on the very pass it was banked. The
log said it had waited **4,294,967 seconds**.

**Why.** `loop()` reads `now` once at the top. A refactor changed one stamp
from `= now` to `= millis()`, which runs later and is therefore **larger**
than `now`. `now - stamp` went negative, which in unsigned arithmetic wraps to
about 4.29 billion, and every "has enough time passed?" test fired instantly.
It only hit KSh 5 and 10, because larger coins vend before the timeout check
is reached.

**Lesson.** Never subtract a timestamp that might be newer than your "now".
Use wrap-safe helpers (`reached()` to decide, `sinceMs()` to measure). A
nonsense number in a log (4,294,967) is an unsigned wrap until proven
otherwise.

### 2.4 A change in one place turned noise into money

**What happened.** KSh 40 vended three pads. The motor was inducing fake coin
pulses, up to 472 in one run, in the same one- or two-pulse bursts as the
cheapest coin.

**Why it appeared suddenly.** The old code **ignored** coins arriving during
a vend, which hid the noise. Changing it to **bank** them (correctly, since a
coin mid-flight is real money) turned the noise into credit.

**Lesson.** When you change what a path accepts, ask what else arrives
through it. The fix was to stop counting pulses while the motor runs, and
for a short settling time after.

### 2.5 A debouncer remembers old state

**What happened.** The soak test stopped after every first cycle, claiming
the chute was blocked when it was empty.

**Why.** The IR debouncer still held the reading from the pad that had just
fallen. Nothing had polled it since, and the soak read it once and trusted
it. Paid vends only worked by luck: they polled continuously for 2.5 seconds
first.

**Lesson.** Anything with memory (debouncers, filters, averages) must be
refreshed before you trust it. If something works in one path and fails in
another, look for state the working path happens to refresh.

### 2.6 Check return values, and check the library's source

**What happened.** The code assumed saving the balance to flash always
succeeded, and assumed the library skipped writing identical values. Both
were wrong, confirmed by reading `Preferences.cpp` in the ESP32 core. The
machine could report "saved" and lose the money at the next power cut.

**Lesson.** Read the source of the library you're relying on; don't
describe it from memory. Better still, **remove the dependency on the
claim**: the firmware now keeps its own copy of what was last written, so
"identical writes are skipped" is true because of our code, not because of
a hope about theirs.

### 2.7 Counters that can wrap will eventually wrap

**What happened.** A noisy coin line could push the 16-bit pulse counter past
65,535 and wrap it round to a number matching a real coin: free pads from
noise.

**Lesson.** Saturate counters that feed decisions about money, and put an
explicit ceiling on the balance, even where it "can't happen".

### 2.8 Things that only exist in one state must be designed for the others

**What happened.** A balance recovered after a power cut was dispensed on
boot, into an empty washroom at 3 am.

**Lesson.** When something survives a restart, decide what happens to it
when nobody is there. Here, a recovered pad now waits for a button press or
a coin.

### 2.9 Write for the person reading the screen

**What happened.** Messages like `HOLD BTN 1 SEC` and `SEE SERIAL` fitted the
16-column display, and meant nothing to someone standing in a washroom.

**Lesson.** A small display is a reason to page a sentence, not to shorten
it. Show the customer what she can act on; show the technician what they
need.

### 2.10 The coin table was calibrated to a counting fault

**What happened.** After the PC817 went in, the same coin started giving
different counts: a KSh 10 read 7, 8, 9 or 10, and coins went into the cash
box without being credited. It looked like pulses were being lost in the new
wiring, and that was the diagnosis at first.

**What was really going on.** A console command (`P`) that sampled the raw
pin showed **5 clean pulses** for a KSh 10, while the interrupt had counted
**9**. The interrupt is set to trigger on falling edges, but on the ESP32 a
few-microsecond glitch on a **rising** edge can fire it too. Each pulse is
50 ms low, so that false count landed 50 ms after the real one, past the
20 ms debounce.

**Why it went unnoticed for weeks.** With the coin wired straight to the pin,
the false count fired on **every** rising edge. Every coin read exactly double
(2 / 10 / 20 / 40 instead of 1 / 5 / 10 / 20), perfectly consistently, so the
coin table was calibrated to the doubled numbers and everything "worked". The
PC817 changed the edge shape, the false count became occasional, and only
then did the counts wander. **The clue had been in the logs all along**: gaps
of 50 and 105 ms between pulses, which are a pulse's own end and the next
pulse's start, not two separate pulses.

**The fix.** A 75 ms debounce, which sits between the false count (+50 ms)
and the next real pulse (+155 ms); the coin gap raised to 400 ms; the table
set to the real counts. After that every coin read exactly, every time.

**Lessons.**
- **Calibrate from the raw signal, not from your own counter.** A counter
  that is wrong in a consistent way produces numbers you can calibrate to,
  and they will look perfect.
- **An edge interrupt is not an edge detector you can trust blindly.** Set
  the debounce from the measured pulse shape (low time and period), not from
  a generic "ringing" figure.
- **Build a tool that shows what the hardware is doing** when the counts
  don't make sense. The `P` probe took minutes to write and settled in one
  coin what a day of theorising had not.
- **When a change "breaks" something, check whether it broke it or exposed
  it.** The optocoupler didn't cause the fault; it made a hidden one visible.

### 2.11 A later decision quietly broke an earlier one

**What happened.** On 25 September the design said: keep the coin slot open
during a vend, because a coin landing mid-vend is banked and buys the next
pad. On 26 September a separate fix muted the coin line while the motor runs,
because the motor was inventing coins. Each decision was right on its own.
Together they meant a coin inserted while the motor turned had its pulses
thrown away: coin in the box, nothing credited. Nobody connected the two until
the builder noticed, a week later, that coins could still go in mid-vend.

**The fix.** Shut the slot from the moment the price is met until the pad is
collected, and before starting the motor, wait for any coin already inside
the acceptor to finish counting.

**Lessons.**
- **When you add a rule, re-read the rules it touches.** The mute changed what
  "a coin arriving during a vend" means; decision 2 should have been reopened
  that day.
- **Write decisions down with their reasons.** Decision 2's reason ("a coin
  landing mid-vend is banked") was checkable, and it was the reason that had
  become false.
- **Watch people use the machine.** A builder dropping coins at the wrong
  moment found what a week of reviews of the code had missed.

---

## Part 3 — Diagnostics and tooling

### 3.1 A diagnostic that lies is worse than none

**What happened, twice.** A "time since last coin" line always printed 0,
because it measured from a timestamp just overwritten. The motor-noise rate
during a soak was reported 14× too high (1549/s instead of about 112/s),
because the count covered five runs and the time covered one.

**Lesson.** Test a diagnostic against a case where you know the answer
before you trust it. A wrong number sends the next person after a fault that
doesn't exist.

### 3.2 A full disk looked like a dead board

**What happened.** Uploads failed. The drive had **zero bytes free**, and
esptool couldn't even unpack itself. That masked a genuinely dead board
behind it, and the two faults were easy to confuse.

**Lesson.** When tooling fails oddly, check the machine first (disk space,
the right port, the right file). Then separate the faults: fix the tooling,
**then** re-test the hardware, and don't let one diagnosis explain both.

### 3.3 How to tell a dead board from a sleeping one

**What we learned.** The ESP32 prints a boot message at 115200 baud on every
reset, whatever firmware is on it. So:

| Symptom | Meaning |
|---|---|
| No bytes at all, in any reset state | Chip dead (or no power / no TX) |
| Garbage at one baud, readable at another | Wrong baud rate |
| esptool: "No serial data received" | Nothing answering |
| esptool: "Invalid head of packet" | The app is running and talking over the sync |
| esptool: "Wrong boot mode (0x3)" | Talking fine; IO0 wasn't low at reset |

The fastest proof of all: a **second board on the same cable and port**. If
it talks and the first doesn't, the first is dead.

### 3.4 Know which board is on the bench

**What happened.** A "spare" board turned out to be running a completely
different project's firmware (a biogas monitor) and was nearly overwritten.

**Lesson.** Read the boot log before flashing an unknown board. Label boards.
Keep a note of which firmware is on which.

### 3.5 One copy of the sketch

**What happened.** Old copies of the sketch in `Documents/Arduino` drifted
from the repo. Debugging one of them cost two rounds, and flashing one would
have loaded the wrong relay polarity for the new hardware.

**Lesson.** Keep **one** canonical copy in version control. Move or delete
the others, and check the path in the IDE's title bar before you flash.

### 3.6 A meter shows averages; know what it can't see

**What we learned.** During a coin, the meter showed 2.2 V, not 0 V: it was
averaging pulses 20–50 ms long. It also can't see microsecond spikes, which
is why 36 V on the meter meant much more in reality.

**Lesson.** To check a low level, **hold the input in that state** (we held
COIN to ground and read 33 mV). For transients, a meter gives a lower bound
at best; use a scope if you have one.

### 3.7 A library call hid a one-second freeze

**What happened.** The status report showed one loop pass taking 1.3 s, then
2.2 s, and its own note blamed the I²C bus. The real cause was
`lcd.init()`. In `LiquidCrystal_I2C` 1.1.2 that is the display's power-up
routine, and it runs `delay(1000)` before sending anything. The firmware
called it on every display rebuild, including right after the motor
started, so for the first second of every vend the machine was not watching
the IR beam. Finger tests hid it, because a finger holds the beam blocked
where a real pad falls through in a fraction of a second.

**Lessons.**
- **Read the library before calling it in a loop.** "init" means power-up;
  a routine written for once at boot has no business in a repeated path.
- **Measure the stall one event at a time.** Resetting the counter (`r`) and
  checking after each kind of event pointed straight at the stock switch,
  then the relay, then the display rebuild.
- **Fixing the freeze kept the self-repair.** The rebuilds exist so a garbled
  display recovers on its own. The HD44780's software reset does that in
  ~15 ms; only the power-up wait was dropped. Loop max went from 2.2 s to
  51 ms idle.
- **The diagnostic was wrong twice:** it blamed I²C for a delay() call, then
  called a normal 50 ms repaint a stall. Calibrate a diagnostic's thresholds
  against measured normal behaviour.

---

## Part 4 — How we worked

### 4.1 Reasoning from a plausible mechanism instead of a measurement

This caused more wasted time than any single bug:

- The 3.3 V relay explanation was right, then abandoned when the motor relay
  was offered as counter-evidence, then restored. **A 30-second Blink test
  would have settled it at the start.**
- The inhibit pin was moved from GPIO 19 to 13 because of a misread
  description, then moved back.
- LCD corruption was blamed on the motor "with more confidence than the
  evidence supported".
- A flyback diode alone was recommended for noise that turned out to be brush
  arcing.
- Wandering coin counts were diagnosed as "pulses lost in the new wiring".
  The raw-waveform probe showed the opposite: extra counts from rising edges
  (lesson 2.10).

**Lesson.** When a question can be answered by a quick test on the bench, do
the test instead of arguing. Build the measurement first; `n0`/`n1` and the
`s` noise rate exist because of this.

### 4.2 Comments drift

**What happened.** Every time behaviour changed, a comment somewhere else went
on describing the old design: the inhibit rule, the state table, the flash
claim, the README pin table (the stock switch listed on two different pins),
the wiring card.

**Lesson.** After changing behaviour, **search for the subject, not the
wording you remember**. Update every description, and keep one authoritative
document per topic (`WIRING.md` for wiring) that the others point to.

### 4.3 "Compiles" is not "works"

**Lesson.** Mark everything ✅ verified on hardware, 🔌 fitted but not yet
run, or ⏳ not done. Commit untested firmware separately from documentation,
and re-test anything whose inputs changed. The coin tests passed with the
coin line wired directly; the optocoupler changes that path, so they are
being run again.

### 4.4 The builder's field observations beat the desk

Several of the best corrections came from the bench: LCD corruption with no
motor connected; "these capacitors are on VCC and GND, not the data line, so
how did they kill it?"; "the inhibit port carries no power". Some were right
outright, and all of them forced a better explanation.

**Lesson.** Treat a surprising observation from the person with the meter as
data, not as a misunderstanding to correct.

---

## Checklist for the next build

**Before any microcontroller is connected**
- [ ] Every supply set and measured with nothing connected to its output
- [ ] Signals from coils, motors, solenoids or 12 V devices go through an
      optocoupler, and are metered while the device is plugged and unplugged
- [ ] Relay modules are active-HIGH, or driven through a transistor
- [ ] Every output that moves something has an external resistor holding it
      safe through boot
- [ ] TVS and bulk capacitance on the main supply; flyback diode and a
      ceramic across every motor; 100 nF beside every electrolytic
- [ ] Grounds meet at one point; signal wires routed away from relay and
      motor wires

**Design questions**
- [ ] What does the machine do when the controller is dead? Is that safe?
- [ ] What physical fact can't the firmware change? Do the rules follow
      from it?
- [ ] Which direction of each error is dangerous, and does the code treat
      the two directions differently?

**Firmware habits**
- [ ] Pulse counts and debounce set from the **raw waveform** (low time,
      period), measured on the pin, not from what the counter reports
- [ ] One "now" per loop; wrap-safe time helpers only
- [ ] Return values of storage and communication calls are checked
- [ ] Counters feeding money or safety decisions saturate
- [ ] Every diagnostic tested against a known answer

**Process**
- [ ] One canonical copy of the code, in version control
- [ ] A quick bench test before any argument about how the hardware behaves
- [ ] Status marked verified / fitted / pending, never implied
- [ ] Two identical failures: stop and find the cause before replacing parts
