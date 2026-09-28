# Troubleshooting

Symptom first. Every entry here has actually happened on this machine.

For "what does this log line mean", see [`LOG_REFERENCE.md`](LOG_REFERENCE.md).

---

## The screen fills with what look like Chinese characters

**Not a random glitch, and the character set is the clue.**

The LCD is driven in **4-bit mode** — every byte crosses as two nibbles. Miss
one nibble and every byte after it shifts by four bits. The HD44780 font has
**Japanese katakana at 0xA0–0xDF**, so mangled bytes land there. You are seeing
correct rendering of wrong bytes, which is why it never recovers on its own.

**What makes it miss a nibble:** anything that dips the rail hard enough for the
LCD controller to brown out while the ESP32 — which has its own regulator —
carries on clocking data at it.

Three things on this machine do it:

| Source | When |
|---|---|
| The motor | every vend |
| **The coin acceptor** | **every coin**, and again at power-up |
| Either relay | whenever a coil de-energises |

**Do not assume it is the motor.** On this build the display corrupted on a
bench with **no motor connected at all** — it was the acceptor, which fires a
gate solenoid on every single coin.

**Note whether it happens on a coin or on a vend.** That separates the two, and
they need suppression in different places.

**The firmware self-heals** — it rebuilds the display after all three events and
once a minute regardless, and waking the backlight is always a full rebuild. If
you see garbage appear and then clear, the firmware is doing its job **and your
rail still needs the capacitors.**

`l` forces a rebuild by hand. If that clears it but it returns on the next
vend, it is confirmed.

---

## Free credit appearing, or extra vends nobody paid for

**Motor noise on the coin line.** Switching the coil induces edges on GPIO 18
that are indistinguishable from real pulses, and they arrive in **ones and
twos** — which is the pulse count of the cheapest coin in the table.

Measured on this machine before suppression: **up to 472 induced pulses in one
5-second run.**

The firmware mutes the coin interrupt while the motor runs and for 500 ms
after, so these no longer become money. But they are still being generated:

```
Discarded 149 pulses induced by the motor over 1636 ms of running.
  That is about 91 per second of motor run.
```

**The rate is the diagnosis:**

| | Cause | Cure |
|---|---|---|
| A **fixed burst** per run, whatever its length | relay coil and motor field collapsing at switch-off | flyback diode across the motor, **band to positive**, at the motor |
| A **steady rate** that scales with run time | continuous arcing at the brushes | **100 nF ceramic across the motor terminals** |

Measured here: 70/second steady → **brush arcing**. A flyback diode alone does
nothing for that.

**Inhibiting the acceptor does not help** and it is worth being clear why:
inhibit stops the mechanism taking a coin, it does nothing about induced
voltage on a signal wire.

Drive `motor noise` in `s` to **zero**.

---

## The inhibit relay will not switch, or is always on

**First: the log already tells you what the firmware did.**

```
  [GPIO 19 driven HIGH -> expect the relay to be RELEASED, contact CLOSED = slot blocked]
```

"The firmware says inhibited" and "the relay actually moved" are different
claims. Only the first is visible from a console.

**Measure it:** `n1` holds the pin HIGH, `n0` holds it LOW, `n` releases and
clicks three times.

| | Means |
|---|---|
| Relay follows `n0` / `n1` | Driven correctly. Whichever level **energises** it is the module's active level — set `INHIBIT_RELAY_ACTIVE_HIGH` to match |
| Nothing moves either way | Check the pin, the module's supply, and that its **ground is common with the ESP32's**. A relay module with no shared ground does nothing at all, and it looks exactly like a dead pin |
| Energises but never releases | It needs more than the pin can supply. Drive it through a small NPN |

**"Always on" may be correct.** With the **NC** contact, *accepting coins*
requires the relay to be held energised — and idle is the normal state. It
drops out only when inhibiting, and briefly during the boot window before
`setup()` drives the pin.

---

## Credit does not accumulate — every coin shows the same amount

**Check the `AT` line first:**

```
AT           : 412 s since boot,  9 s after the previous coin
```

If the gap exceeds `CREDIT_TIMEOUT_MS` (two minutes), part payment expired and
the machine is behaving correctly.

**To isolate it in one step:** type `w0`. Part payment then never expires.
Insert two KSh 10. If it vends, it was the timeout. If it still does not, it is
a bug and worth a log.

**This was a real bug once**, and its signature was unmistakable:

```
cleared after an actual wait of 4294967 s (limit 120 s)
```

4,294,967 s = 2³²−1 ms — an unsigned underflow. `lastCoinMs` was *ahead* of the
loop's `now`, so the subtraction wrapped and the timeout fired on the same pass
the coin was banked. It only showed on KSh 5 and 10 because 20 and 40 meet the
price and vend before the timeout is ever checked.

**If you ever see a wait in the millions, that is this class of bug again.**
Never compare `(now - stamp)` directly against the loop's `now`; use
`reached()` or `sinceMs()`.

---

## JAMMED on a coil that is working

**The dispense timeout is too tight.** A slow pad gets called a jam, and a
paying customer is told the machine is broken.

Check `s`:

```
  slowest drop : 3503 ms, timeout is 7000 ms
```

It warns when the timeout falls under twice the slowest drop it has seen.

**Set it properly:** run `T25`, read `SLOWEST` from the result, set twice that
with `o<ms>`, then write it into SECTION 5.

**The shipped 7000 ms is a placeholder**, not a measurement.

---

## Nothing on the I2C bus

```
  I2C bus scan:
    nothing responded. Check SDA/SCL and the LCD's 5 V.
```

Not a wrong address — **nothing answered at all**. In order of likelihood:

1. SDA/SCL swapped or not connected
2. The LCD has no 5 V
3. No shared ground between the LCD and the ESP32
4. The backpack is dead

If the scan finds a device at a *different* address, put that in SECTION 4. The
machine runs headless meanwhile and still vends.

---

## The board will not boot at all

**Check whether anything is connected to GPIO 12.** It is MTDI, and the chip
reads it at boot to pick the flash regulator voltage. Anything with a pull-up
on it holds 12 high at reset, the ESP32 sets the flash rail to 1.8 V, and the
board stops booting.

Free and safe on this build: **14, 16, 17, 32, 33.**

Also avoid 0, 2, 5, 15 (strapping), 1 and 3 (USB serial), 6–11 (flash), and
34–39 (input only, no internal pull-up — useless for a switch to ground).

---

## BROWNOUT in the self-test

```
  last reset  : BROWNOUT — the 12 V rail sagged
```

The motor is dragging the supply down hard enough to reset the board. Fit the
**1000 µF across the 12 V rail**, close to the buck. This is not optional.

---

## The machine halted at boot

A **fatal** self-test result stops `setup()` before the coin interrupt is
attached, so the machine cannot take money it could not price. The console
stays alive.

Fatal results: empty coin table, a 0-pulse entry, **duplicate pulse counts**,
coin gap too close to the debounce, **chute blocked at startup**.

**Recovery:** fix the cause, then hold the retry button. Clearing from a halted
boot **re-runs the whole self-test** and refuses to return the machine to
service until it passes.

A blocked chute at boot is fatal on purpose, so a power cut with one pad stuck
halts the machine on the way back up. The next vend would have faulted anyway.

---

## The machine sits refusing to vend, and nothing looks wrong

**Check `PAD_SWITCH_FITTED`.** If it is `true` but the switch is not wired, the
pin floats HIGH, which reads **EMPTY**, and the machine correctly refuses to
sell pads it believes it does not have.

Either wire the switch or set the flag `false`. The self-test says which mode
it is in on every boot.

---

## Everything works but the takings do not match the pad count

Three places money legitimately goes without a pad, all logged, none silent:

1. **Abandoned part payment** — expires after `CREDIT_TIMEOUT_MS`
2. **A coin mid-flight when the machine faulted or ran out** — the inhibit
   relay stops the *next* coin, not one already being read
3. **An attendant clearing a fault** — the console prints the amount it is
   clearing

And one the other way: a power cut in the few milliseconds between a pad
landing and the balance reaching flash makes the machine vend one extra on
boot. Documented and deliberate — it errs toward the customer.
