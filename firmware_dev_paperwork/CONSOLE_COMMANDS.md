# Console commands

**115200 baud.** Type `?` on the machine for the same list.

Line ending "Newline" if you can. "No line ending" also works — a command is
taken after a short pause in typing.

**Nothing typed here is saved.** The console is for *finding* a number; SECTION
5 of the sketch is where it lives once you have it. The meter balance is the
only thing this firmware keeps in flash, and no console command writes to it.

**A typed command always answers in full**, even on a machine installed with
logging off. `handleCommand()` forces logging on for its own duration.

---

## Looking at things

| | Does |
|---|---|
| `?` | The command list |
| `s` | Status — state, credit, counters, slowest drop, motor noise, loop health |
| `d` | Re-run the whole nine-part self-test |
| `c` | Print the coin table with what each coin buys |
| `z` | Scan the I2C bus |

`s` is the one to reach for first. It reports the two numbers that say whether
the machine is healthy: **`motor noise`** (should be 0) and **`slowest drop`**
against the configured timeout.

## Watching a sensor live

| | Does |
|---|---|
| `i` | Watch the IR drop beam. Wave a pad through |
| `k` | Watch the pad stock switch. Work the lever |

Any other command stops a running watch, so its chatter never buries the answer
you just asked for.

## Calibration — lost on reset

Write the value you settle on into SECTION 5 and reflash.

| | Sets | Notes |
|---|---|---|
| `p<ksh>` | Pad price | e.g. `p20` |
| `g<ms>` | Coin gap | e.g. `g350`. Walls on both sides — see below |
| `o<ms>` | Dispense timeout | e.g. `o4500`. Get it from `T<n>` |
| `w<ms>` | Part-payment wait | `w0` = never expires |

**`g` has a wall on each side.** Too low and one coin splits in two,
short-changing the customer. Too high and two coins merge into one unknown coin
and the money is lost. The acceptor cannot deliver two coins closer than about
600 ms, so that is the ceiling.

**`w0` is the fastest way to isolate a credit question** — it removes the
timeout from the picture entirely without a reflash.

## Bench tests

| | Does |
|---|---|
| `m<ms>` | Run the motor for `<ms>` and report when the beam broke |
| `t` | One full test vend, no coin needed |
| `T<n>` | **Soak**: `n` vends back to back, times every drop, tells you what to set the timeout to |
| `v<n>` | Pretend an `n`-pulse coin went in |
| `n` | Click the inhibit relay 3× — does it actually move? |
| `n0` / `n1` | Hold the inhibit pin LOW / HIGH to read the relay's polarity |
| `P<s>` | **Coin probe**: waits up to `<s>` seconds (default 30) for one coin, samples GPIO 18 every ~20 µs and prints every LOW and HIGH period beside what the interrupt counted. The two numbers must match. Reads the pin only; the coin is counted and banked as normal. Found the double-counting fault on 2026-10-01 |
| `l` | Rebuild the LCD and draw a test pattern |
| `b` | Buzzer test |

`m` and `T` **block**, so the `loop()` motor backstop is not running during
them — their own ceilings are the only limit. Fine for a command an operator
types while watching the machine, wrong for anything automatic, which is why
nothing automatic uses either.

`d`, `l`, `m` and `T` refuse while a vend is in progress. While the motor runs,
`loop()` is the only thing watching the beam and enforcing both timeouts.

## Running the machine

| | Does |
|---|---|
| `x` | Clear a latched fault — staff override, forgives the retry count |
| `r` | Zero the session counters |
| `h` | Toggle the heartbeat line (soak tests) |
| `L` | Toggle event logging |

---

## Reading a coin report

```
AT           : 412 s since boot,  9 s after the previous coin
COIN IN      : 10 pulses
PULSE TIMING : 1396 ms burst, gaps 155-156 ms
RECOGNISED   : KSH 20  (worth KSH 20)
CREDIT NOW   : KSH 20 of KSH 20
MEANING      : price met
WILL VEND    : 1 pad
LEFT OVER    : KSH 0
```

**`AT`** — the gap since the previous coin. Check this before assuming credit
vanished for no reason; part payment expires after `CREDIT_TIMEOUT_MS`.

**`PULSE TIMING`** — the only way to see whether `COIN_GAP_MS` has margin. The
firmware warns when the widest gap approaches half of it, or the tightest
approaches the debounce floor.

**`RECOGNISED : NO`** prints the exact `COIN_TABLE` line to add.

## Reading the noise report

```
Discarded 149 pulses induced by the motor over 1636 ms of running.
  That is about 91 per second of motor run.
```

**The rate is the diagnosis, not the count:**

| | Cause | Cure |
|---|---|---|
| A **fixed burst** per run, whatever the length | relay coil and motor field collapsing | flyback diode |
| A **steady rate** scaling with run time | continuous brush arcing | 100 nF across the motor terminals |

Any reading above zero means the suppression is not finished. See
[`TROUBLESHOOTING.md`](TROUBLESHOOTING.md).

## Reading a soak result

```
SOAK RESULT
  delivered    : 24 of 25
  no pad       : 1   (4%)
  fastest      : 890 ms
  average      : 1230 ms
  SLOWEST      : 2140 ms   <- this is the one that matters

  SET DISPENSE_TIMEOUT_MS TO 4500 ms
```

**Twice the slowest, not the average** — the machine has to survive its worst
pad. And the **miss percentage is the real result**: that number is the coil and
the pads, and no firmware change moves it.
