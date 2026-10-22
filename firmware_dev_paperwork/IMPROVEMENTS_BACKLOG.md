# Improvements backlog

What is parked, why, and the trigger that would make it worth building.

Ordered by what unblocks the most.

---

## Questions for Rebecca at commissioning

**Ask her, then set the firmware the way she wants.** These are business
decisions about her customers' money, not engineering ones.

**Q1. A paid pad, after a jam, when the customer has left.** A customer pays,
the machine jams, the three retries run out, and the screen says to call the
number. If she has gone before staff fix it, the KSh 20 stays on the meter
(saved in flash) and the pad is held. Today it goes to **the next person who
presses the button or inserts a coin**.
  - **(a) Keep it** (current behaviour): the pad is honoured, but possibly to
    a stranger.
  - **(b) Let staff cancel it**, e.g. as part of the service visit (lifting
    the pads out and back) or a long button hold inside the door. The money
    stays in the box; the business keeps it.
  - **(c) Let it expire** after a set time, like part payment does.

  Whatever she picks, the payer can only be refunded by a person: the machine
  has no way to return coins.

**Q2. Part payment** (backlog item 7). Someone puts in KSh 5 or 10 and walks
away. After `CREDIT_TIMEOUT_MS` (2 minutes now) it is cleared and the
business keeps it. Longer, or never (`0`), means it eventually buys a pad for
whoever comes next.

**Q3. Confirm the basics:** price KSh 20; coins accepted 5 / 10 / 20 / 40;
the service number shown on the fault screen.

**Q4. How long the screen stays lit when nobody is using it.** Today the
backlight goes off after **2 minutes** idle (`BACKLIGHT_TIMEOUT_MS`); the text
stays readable in the dark, and a coin or the retry button lights it again.
  - **Shorter or keep 2 min:** a dark washroom stays dark; less glare.
  - **Longer:** a lit screen is easier to notice and read from the door.
  - **Never off** (`0`): always lit. The cost is small on mains power and
    the backlight LEDs are rated for tens of thousands of hours.

  Whatever she picks, it only changes the timing; waking by coin and button
  stays.

**Q5. Sound on or off.** Today the machine beeps: a short high note when a
coin goes in, a low one when a coin is refused, a brief one when the pad
drops, a long low one on a fault. It is a public washroom and a customer may
prefer the machine not to announce her.
  - **On** (`SOUND_ON = true`, current): confirmation she can hear.
  - **Off** (`SOUND_ON = false`): silent; the screen still says everything.
    The bench test `b` still chirps either way.

---

## Blocking the demo

### 1. ~~Wire the LCD~~ ✅ 2026-10-01

Wired through an I²C level converter (`WIRING.md` §9), found at 0x27. Still
needed: 100 µF + 100 nF at the LCD, since it sits ~4 cm from the buck's output caps.

### 2. Settle the inhibit relay polarity

**Status: ✅ done 2026-10-01.** Active-HIGH module on the NC contact,
`INHIBIT_RELAY_ACTIVE_HIGH = true`, GPIO 19 pull-down. `n` clicks it
correctly and there is no click through boot.

### 3. Suppress the motor, drive `motor noise` to zero

**Status: ✅ 0 pulses discarded (2026-10-01).** 47 nF + 1N5408 at the motor,
and the coin line through the PC817. The firmware mute stays as a backstop.

### 4. Build the chute and measure the real drop times

**Status:** `DISPENSE_TIMEOUT_MS = 7000` is a **placeholder**.

Every timing so far came from a finger at the IR sensor. Once pads fall down a
real chute: `T25`, read `SLOWEST`, set twice that, write it into SECTION 5.

### 5. The 50-cycle soak

**The number the client actually asked for.** How many out of fifty drop
cleanly. No firmware change moves it — that number is the coil and the pads.

---

## Worth doing before a site install

### 6. ~~Put the real service number in `SERVICE_CONTACT`~~ ✅ 2026-10-01

Set in `service_contact.h`, which is local only and in `.gitignore`; copy
`service_contact.example.h` to create it on another computer. The number is
deliberately not in git.

### 7. Decide the part-payment policy with the client (see Q2 above)

`CREDIT_TIMEOUT_MS` is two minutes. No value returns the money — it only
decides whether abandoned part payment stays with the business or eventually
buys a pad for whoever walks up next. `0` waits forever.

**This is Rebecca's call, not an engineering one.**

### 8. Decide whether to ship with logging off

`SERIAL_LOG_DEFAULT` and `SERIAL_LOG_BOOT`. Both `false` and the machine says
nothing unless spoken to, while every typed command still answers in full and a
failing self-test still shouts.

---

## Parked by decision — build when the trigger fires

### 9. Low-stock warning, not just empty

**Parked because:** the machine knows "empty" or "not empty", never "four
left". A count-down from the last refill needs somewhere to store the count,
and this build deliberately stores only the meter balance.

**Build when:** a site runs dry between scheduled visits and somebody wants
warning. The feasibility study describes the count-down approach.

### 10. A vend log that survives power loss

**Parked because:** vend count, takings and fault count are session-only by
design — a proper log belongs with the cabinet build, not before it.

**Build when:** the machine goes to a real site and the takings need to be
reconciled without somebody standing there with a laptop.

### 11. Battery, sleep mode, wake button

**Parked because:** explicitly out of scope for this build. The 3–4 week
battery figure in the client study is arithmetic, not a measurement.

**Groundwork already done:** the backlight sleeps after two idle minutes and
wakes on the first coin pulse, which is the discipline the study says the
battery depends on.

**Build when:** the mechanism is proven and a no-mains site is real.

### 12. Reverse on jam

**Parked because:** the relay drives the coil one way only. This was a
deliberate cost decision agreed with the client and becomes permanent once the
motor is bought.

**Build when:** never, for this machine. It would need an H-bridge, which is
the motor driver the first build removed.

### 13. A high-side switch for the acceptor's power

**Parked because:** the inhibit relay covers the normal case — a faulted or
empty machine refuses the *next* coin.

**Would fix:** a coin already mid-flight when things go wrong is still lost.
Cutting power to the acceptor entirely would not fix that either.

**Build when:** probably never. The residual loss is one coin in a rare case.

### 14. M-Pesa

Out of scope for this build. Option D in the feasibility study.

---

## Small things

- **`k`, `l` and `L`** are listed in the console help (and in
  `CONSOLE_COMMANDS.md`) but were never implemented; typing them says
  "Unknown command". Write them or take them out of the help.
- **Per-state loop max.** `loop max` is one number for everything. Splitting
  it by state would show whether the 227 ms pass is before or during the
  coil's turn. Worth it only if the soak shows a missed drop.
- ~~**Fit the TVS**~~ ✅ P6KE15CA fitted 2026-10-22, and the 1 MΩ GPIO-27
  pull-down. (Relay-module and LCD decoupling: not needed, both within a few
  cm of the buck's output caps.)

- **`CLEAR_BTN_HOLD_MS`** is 1 second. Watch a real customer use it before
  deciding whether that is right — too short invites accidents, too long and
  she gives up.
- **`MAX_BUTTON_RETRIES`** is 3. One constant.
- **The LiquidCrystal_I2C architecture warning** is cosmetic — the library's
  `library.properties` still says `architectures=avr`. Changing it to `*`
  silences it and Library Manager overwrites that on the next update.
