# Bench testing

How to bring a machine up, prove each part, and find the numbers that have to
be measured rather than chosen.

Console at **115200 baud**. Line ending "Newline" if you can; "No line ending"
also works, it just takes the command after a short pause in typing.

---

## Before the ESP32 is connected to anything

These two cannot be undone if skipped.

**1. Meter the buck output.** Want **5.0 V**. It ships at whatever the pot
happens to be, often well above.

**2. Meter the coin acceptor's pulse line.** Want **3.3 V or less** on the
swing. If it reaches 12 V it needs a divider or optocoupler before it touches
GPIO 18. ESP32 pins are not 5 V tolerant, let alone 12 V. This kills the pin
silently, once.

---

## Bring-up order

Each step isolates one failure. Do them in order.

### 3. Reset with the acceptor connected, motor disconnected

**Listen.** Want **no click**. A click means the inhibit relay energises during
the boot window, so the coin slot is live on every reset.

### 4. Flash and read the self-test

Nine sections. Section [1] names `BROWNOUT` and `PANIC` specifically — those
two are what actually happen to this machine. Section [4] scans the whole I2C
bus, so a blank LCD is never guesswork.

A **fatal** result halts before the coin interrupt is attached, so the machine
cannot take money it could not price.

### 5. Coins — one of each

```
AT           : 412 s since boot,  9 s after the previous coin
COIN IN      : 10 pulses
PULSE TIMING : 1396 ms burst, gaps 155-156 ms
RECOGNISED   : KSH 20  (worth KSH 20)
CREDIT NOW   : KSH 20 of KSH 20
MEANING      : price met
WILL VEND    : 1 pad
```

Want **1 / 5 / 10 / 20 pulses** with **every gap about 155 ms**. A mix of
50, 105 and 155 ms gaps, or the same coin giving different counts, means
pulses are being counted twice: run `P` and see `COIN_PULSE_DEBOUNCE_MS`.

Then **two coins as fast as the acceptor will take them** — they must still read
as two. That is the other wall on `COIN_GAP_MS`.

**Also try to defeat it:** KSh 1, KSh 5, KSh 10, old-series coins and a 26 mm
hardware washer. That test is half the reason this machine exists.

### 6. Accumulation

Insert **2 × KSh 10**. Want `KSH 10` then `KSH 20` then a vend.
Then **4 × KSh 5**. Want 5, 10, 15, 20, vend.

If credit resets between coins, check the `AT` line for the real gap before
assuming a bug — part payment expires after `CREDIT_TIMEOUT_MS`. `w0` disables
the timeout entirely and isolates the question in one step.

### 7. The drop sensor — `i`

Wave a pad through the chute. Want `BLOCKED` / `clear` flipping cleanly. If
they are the wrong way round, flip `IR_DETECTED_IS_LOW`.

*The KY-032 uses an NE555 and may be unreliable at 3.3 V. If it misbehaves,
move it to 5 V with a 10 k / 20 k divider on its output — do not feed 5 V into
a GPIO.*

### 8. The relay, motor disconnected — `m500`

One click on, one click off. Confirm it does **not** click on boot or reset.

### 9. The inhibit relay — `n1`, `n0`, `n`

`n1` holds the pin HIGH, `n0` holds it LOW, `n` releases and clicks it three
times.

| What you see | What it means |
|---|---|
| The relay follows `n0` / `n1` | driven correctly. Whichever level **energises** it is the module's active level — set `INHIBIT_RELAY_ACTIVE_HIGH` to match |
| Nothing moves either way | check the pin, the module's supply, and that its ground is **common with the ESP32's**. A relay module with no shared ground does nothing at all |
| Energises but never releases | it needs more than the pin can supply. Drive it through a small NPN |

### 10. The stock switch — `k`

Work the lever. Want `PADS PRESENT` with the coil loaded and `EMPTY` with it
empty. Flip `PADS_PRESENT_IS_LOW` if reversed, then set
`PAD_SWITCH_FITTED = true` and reflash.

**Until that flag is true there is no empty detection at all.**

---

## Finding the dispense timeout

`DISPENSE_TIMEOUT_MS` ships at **7000 ms as a placeholder**. It is not measured.

You cannot get the number from a few vends, and certainly not from a finger at
the sensor — that measures how fast a hand moves. It needs real pads down a
real chute, enough of them to show the tail, because the timeout has to survive
the machine's **worst** pad and not its average one.

### Load the coil and soak it

```
T25
```

Twenty-five vends back to back, no coins, nothing charged. Every drop timed.

Each cycle waits up to **15 s** — far longer than the real timeout, and
deliberately so. **A stopwatch that stops before the event cannot measure it:**
a 6 s drop under a 5 s timeout is recorded as "no pad" and the number you are
hunting never appears.

```
SOAK RESULT
  delivered    : 24 of 25
  no pad       : 1   (4%)
  fastest      : 890 ms
  average      : 1230 ms
  SLOWEST      : 2140 ms   <- this is the one that matters

  SET DISPENSE_TIMEOUT_MS TO 4500 ms
  Try it now:  o4500
```

Then write that number into SECTION 5 and reflash — the console does not save
it.

**Twice the slowest, not the average.** A timeout set on the average turns
every slow pad into a paying customer being told the machine is broken.

It stops early on a keypress, an empty coil, a blocked chute, or three misses
in a row.

### The miss percentage is the real result

`24 of 25` is the answer to the only question the client actually asked, and no
amount of firmware improves it. That number is the coil and the pads. If it is
poor, the fix is mechanical: coil pitch, chute angle, pad thickness.

---

## Testing the behaviours

Each needs a specific setup, so they are easy to get wrong by accident.

### Jam retry and the customer button

Needs **real credit on the meter**, or you get the `OUT OF ORDER` screen
instead of the customer one.

1. Insert a real KSh 20
2. Let the vend fail — an empty coil is enough
3. Want `PAID - NO PAD` / `HOLD THE BUTTON FOR 1 SECOND TO TRY AGAIN`
4. Hold the button → motor rolls again, **credit still on the meter**
5. Three times, then `JAMMED` and the service number

`x` clears it and forgives the count whenever you need to reset the test.

### Power-cut recovery and the held pad

1. Insert KSh 20
2. Pull power mid-vend
3. Power back up

Want `RECOVERED KSH 20` and **`HOLDING it until somebody presses the button`** —
not the motor starting. Then press the button, or insert a coin; either
releases it.

### Out of stock

Disconnect the stock switch (or open it). Want the machine to go to
`SORRY - NO PADS`, inhibit coins, and come back **by itself** when reconnected —
no reset needed.

### Refill clears a latched jam

With the machine latched in `JAMMED`, cycle the stock switch away and back.
Want the fault to clear on its own and the retry budget restored.

### The soak command itself

Without a chute, using a finger, to prove the tool before trusting it:

- `T3` — touch the sensor each cycle. Want three timings and a result block
- `T25` then press a key — want `Stopped by keypress` and a partial result
- `T5` and touch nothing — want three full 15 s waits then
  `Three in a row with nothing`. Takes ~50 s; that is the guard working

---

## What good looks like

| Check | Want |
|---|---|
| Buck output | 5.0 V |
| Coin pulse swing | ≤ 3.3 V |
| Reset with acceptor connected | no relay click |
| Self-test | `SELF-TEST: PASS` |
| Coin pulses | 1 / 5 / 10 / 20, every gap ~155 ms, same count every time |
| Two fast coins | still read as two |
| Accumulation | 5→10→15→20, vend |
| `motor noise` in `s` | **0** — anything above means suppression is unfinished |
| Soak | ≥ 95% delivered, and a slowest drop you can set the timeout from |
