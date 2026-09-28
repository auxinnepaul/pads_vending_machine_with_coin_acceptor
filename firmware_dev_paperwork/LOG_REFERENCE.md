# Log reference

Every line the machine prints, what it means, and whether it is normal.

Symptom-first debugging is in [`TROUBLESHOOTING.md`](TROUBLESHOOTING.md). This
is output-first: you have a line in front of you and want to know if it matters.

---

# A healthy boot, annotated

```
ets Jul 29 2019 12:21:46
rst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)
...
E (101) psram: PSRAM ID read error: 0xffffffff
```
**Normal.** ESP32 ROM bootloader. The PSRAM error is the chip looking for
optional memory this board does not have. Nothing to do with our firmware.

```
  BRICAPA PAD DISPENSER  -  prototype firmware v2.0
  Type ? for commands. Console changes are not saved; the
  meter balance is the only thing kept through a power cut.
```
**Normal.** Silenced by `SERIAL_LOG_BOOT = false`.

### [1] POWER / RESET

```
  last reset  : power-on
  free heap   : 303748 bytes
  CPU         : 240 MHz
```

| `last reset` | Means | Do |
|---|---|---|
| `power-on` | Normal. Plugged in or reset button | — |
| `software restart` | Normal after a reflash | — |
| **`BROWNOUT — the 12 V rail sagged`** | The motor is dragging the supply down hard enough to reset the board | Fit the 1000 µF. Not optional |
| **`PANIC / exception`** | Firmware crashed | Report it with the log |
| `task watchdog` | Something blocked too long | Report it |

**Free heap** should sit around 300 KB and not fall between boots.

### [2] COIN TABLE

```
  2 pulses = KSH 5  (KSH 5)
  20 pulses = KSH 20  (KSH 20)   <- exact price
  price       : KSH 20
  change      : kept on the meter
```
**Normal.** `<- exact price` marks a coin that buys a pad outright.

**Fatal:** duplicate pulse counts, a 0-pulse entry, an empty table. Two coins
on one channel is money you could never account for.

### [3] PULSE DECODING

```
  coin line   : GPIO 18 reads HIGH (idle, correct)
```
**HIGH is correct** — the acceptor pulls it low to signal. `LOW` at rest means
no coin will ever register a falling edge; check the wiring first.

### [4] SCREEN

```
  I2C bus scan:
    device at 0x27   <- LCD_I2C_ADDR
  LCD at 0x27  : present
```
**Normal.**

```
    nothing responded. Check SDA/SCL and the LCD's 5 V.
  LCD at 0x27  : NOT FOUND
```
**Not a wrong address — nothing on the bus at all.** Wiring, the LCD's 5 V, or
a missing shared ground. The machine runs headless and still vends.

If the scan finds a device at a *different* address, put that address in
SECTION 4.

### [5] IR DROP SENSOR

```
  chute reads : clear    (correct when empty)
```
**Normal.** `BLOCKED` is **fatal** at boot — either something is in the chute,
the sensor is mis-aimed, or `IR_DETECTED_IS_LOW` is the wrong way round. Use
`i` to see it live.

### [6] RELAY / MOTOR

```
  dispense timeout : 7000 ms
  hard motor limit : 9000 ms
```
The hard limit always sits above the dispense timeout. It is a backstop against
the state machine failing, not a second timeout.

### [7] COIN INHIBIT / CLEAR BUTTON

```
  contact     : NC — fail-safe, inhibited when unpowered
  coins now   : inhibited
  pin reads   : HIGH  <- compare this with what the relay is doing
  clear button: GPIO 23 reads HIGH (released, correct)
```
**`pin reads` is the line that matters.** Compare it with what the relay is
physically doing. If they disagree, the config is wrong, not the firmware —
use `n0` / `n1` to settle it.

`contact : NO` warns loudly: a dead board would accept coins and deliver
nothing.

### [8] PAD STOCK SWITCH

```
  stock reads : PADS PRESENT
```
Or `BYPASSED — PAD_SWITCH_FITTED is false` — **there is then no empty detection
at all.**

### [8b] STORED BALANCE

```
  store       : open, writing normally
  balance in flash: KSH 0
```
A non-zero balance here after a clean run means a power cut left money on the
meter. Expect a `RECOVERED` line shortly after.

```
  store       : WILL NOT OPEN  <- cannot see or clear an old balance
  store       : open but NOT WRITING  <- power-cut protection is gone
```
Two different faults needing different fixes. The machine keeps trading and
keeps saying so.

### Then

```
SELF-TEST: PASS
Coin interrupt attached.
Ready. Waiting for a coin.
  [GPIO 19 driven LOW -> expect the relay to be ENERGISED, contact OPEN = slot live]
Coin acceptor ENABLED
```

**`Coin interrupt attached`** is the moment the machine becomes able to take
money. If you never see it, the self-test failed and the machine halted.

---

# Event lines

## A coin

```
AT           : 412 s since boot,  9 s after the previous coin
COIN IN      : 20 pulses
PULSE TIMING : 1447 ms burst, gaps 49-106 ms
RECOGNISED   : KSH 20  (worth KSH 20)
CREDIT NOW   : KSH 20 of KSH 20
MEANING      : price met
WILL VEND    : 1 pad
LEFT OVER    : KSH 0
```

**Expected on this machine:** 2 / 10 / 20 / 40 pulses, gaps 49–106 ms, bursts
of 50 ms (KSh 5) to 3000 ms (KSh 40).

**`AT`** — check this before believing credit vanished for no reason.

**Warnings under `PULSE TIMING`:**

```
  !! Widest gap is over half of COIN_GAP_MS (250 ms).
```
This coin is close to being read as two, which would short-change the customer.
Raise it with `g<ms>`, but stay under 600 ms or two coins merge.

```
  !! Tightest gap is near the debounce floor.
```
Pulses may be being merged. If counts read **low**, lower
`COIN_PULSE_DEBOUNCE_MS`.

**`RECOGNISED : NO`** prints the exact table line to add. The customer has paid
and is owed nothing until SECTION 2 is fixed.

**`MEANING : machine is DISPENSING — banked toward the next pad`** is normal: a
coin arriving mid-vend buys the next pad.

**`MEANING : machine is FAULT — coin logged, NOT banked`** — the machine cannot
promise a pad, so it must not take the money onto the meter. That coin is lost;
it was mid-flight when things went wrong.

## A vend

```
Motor on — credit met
PAD DELIVERED in 1447 ms
  session vends : 3
  charged       : KSH 20
  credit left   : KSH 0
```

**`charged`** is the only line that reports money leaving the meter, and it
prints *after* delivery. That is the whole payment model in one line.

```
  NOTE: that was very fast. Either the beam was
  already breaking, or the sensor sees the coil.
```
Under 150 ms. Re-aim the sensor.

## Motor noise

```
Discarded 149 pulses induced by the motor over 1636 ms of running.
  That is about 91 per second of motor run.
  A steady rate like that is brush arcing, not the switching
  spike, so a flyback diode alone will not fix it.
```

**Expected: zero.** Anything above means suppression is unfinished. The **rate**
is the diagnosis:

| | Cause | Cure |
|---|---|---|
| Fixed burst per run | relay coil / motor field collapsing | flyback diode |
| Steady rate scaling with run time | brush arcing | 100 nF across the motor terminals |

Measured on this machine before suppression: **70/second, steady.** After:
5–46/second. Drive it to zero.

## A failed vend

```
No pad in 7000 ms.
  KSH 20 is still on the meter — NOT taken for this attempt.
*** FAULT: JAMMED
*** The customer is owed a pad and has KSH 20 on the meter.
*** The button will retry and dispense it. 3 attempts left.
```

**`still on the meter — NOT taken`** is the payment model working. She has not
been charged.

`JAMMED` vs `EMPTY OR JAMMED` — the first means the stock switch says there
*are* pads, so it cannot be empty. The second means no switch is fitted and the
machine cannot tell.

## Part payment

```
Part payment of KSH 10 cleared after an actual wait of 118 s (limit 120 s).
```

**Both numbers matter.** The first is measured, the second is configured. They
should be close; a measured value far below the limit means something else
cleared it.

## Power-cut recovery

```
RECOVERED KSH 20 left on the meter when the power went.
  That is a full pad, already paid for. HOLDING it until
  somebody presses the button or puts a coin in.
```

Then one of:

```
Button pressed — releasing the pad that was paid for before the power went.
Somebody is here — releasing the pad held from before the power cut as well.
```

For part payment instead:

```
  Part payment. The countdown restarts now, so it expires 120 s
  from this boot rather than sitting here indefinitely.
```

## The inhibit relay

```
  [GPIO 19 driven LOW -> expect the relay to be ENERGISED, contact OPEN = slot live]
Coin acceptor ENABLED
```

Prints only on a change. **Compare the bracketed line with the relay.** The
firmware saying "inhibited" and the relay actually moving are two different
claims, and only the first is visible from a console.

## Lines that mean a firmware bug

```
!! Hard motor limit hit. The state machine did not stop it.
!! That is a firmware bug, not a jam. Report it.
```

```
!! CREDIT CEILING HIT. Coin NOT banked.
!! Credit is not being spent when it should be.
```

Neither should ever appear. If one does, send the log.
