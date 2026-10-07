# Improvements backlog

What is parked, why, and the trigger that would make it worth building.

Ordered by what unblocks the most.

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

### 7. Decide the part-payment policy with the client

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
- **Fit the TVS (P6KE15A) and the relay-module decoupling.** Parts,
  not code; see `WIRING.md` §2.

- **`CLEAR_BTN_HOLD_MS`** is 1 second. Watch a real customer use it before
  deciding whether that is right — too short invites accidents, too long and
  she gives up.
- **`MAX_BUTTON_RETRIES`** is 3. One constant.
- **The LiquidCrystal_I2C architecture warning** is cosmetic — the library's
  `library.properties` still says `architectures=avr`. Changing it to `*`
  silences it and Library Manager overwrites that on the next update.
