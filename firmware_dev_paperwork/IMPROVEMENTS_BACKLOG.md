# Improvements backlog

What is parked, why, and the trigger that would make it worth building.

Ordered by what unblocks the most.

---

## Blocking the demo

### 1. Wire the LCD

**Status:** not wired. The I2C scan finds nothing on the bus.

Everything the customer sees is currently invisible — the paged messages, the
countdown, the retry prompt, the accepted-coin list. The machine vends
correctly and tells nobody anything.

**Not a code task.** See `TROUBLESHOOTING.md` → "Nothing on the I2C bus".

### 2. Settle the inhibit relay polarity

**Status:** decided by hardware. The active-LOW module was replaced
(2026-09-30) with an active-HIGH one identical to the motor relay, because
3.3 V cannot release a 5 V active-LOW module. `INHIBIT_RELAY_ACTIVE_HIGH =
true`, GPIO 19 pull-**down**. ⏳ Still to confirm on a live board.

Confirm with `n1` then `n0`: the relay should energise on `n1` and release on
`n0`. Then reset with the acceptor connected and listen: no click through boot.

### 3. Suppress the motor, drive `motor noise` to zero

**Status:** down from 70/s to 5–46/s. Not finished.

100 nF ceramic across the motor terminals first — measured as brush arcing, not
the switching spike. Then the flyback diode for the turn-off spike, and 100 nF
from each terminal to the case if it is metal.

The firmware mutes the coin line so this cannot become money, but that is a
plaster over a hardware fault.

### 4. Build the chute and measure the real drop times

**Status:** `DISPENSE_TIMEOUT_MS = 7000` is a **placeholder**.

Every timing so far came from a finger at the IR sensor. Once pads fall down a
real chute: `T25`, read `SLOWEST`, set twice that, write it into SECTION 5.

### 5. The 50-cycle soak

**The number the client actually asked for.** How many out of fifty drop
cleanly. No firmware change moves it — that number is the coil and the pads.

---

## Worth doing before a site install

### 6. Put the real service number in `SERVICE_CONTACT`

Currently `"CALL No. ON BOX"`. The self-test nags when there is no digit in it,
because `GET HELP` without saying who to get is no help at all. 16 characters
maximum.

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

- **`CLEAR_BTN_HOLD_MS`** is 1 second. Watch a real customer use it before
  deciding whether that is right — too short invites accidents, too long and
  she gives up.
- **`MAX_BUTTON_RETRIES`** is 3. One constant.
- **The LiquidCrystal_I2C architecture warning** is cosmetic — the library's
  `library.properties` still says `architectures=avr`. Changing it to `*`
  silences it and Library Manager overwrites that on the next update.
