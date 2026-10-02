# Overview

What this machine is, what it proves, and a tour of the firmware.

---

## What it is

A working prototype of a coin-operated sanitary pad dispenser, built to answer
one question: **does a coin go in and a pad come out?**

Everything not needed to answer that has been left out on purpose.

A coin goes into an electronic acceptor, the acceptor sends pulses, the
firmware decides which coin it was and what it is worth, and when the credit
reaches the price a relay turns a coil until a pad falls through an infrared
beam. **The beam is what proves delivery** — turning the coil is not the same as
delivering a pad, and the whole design turns on that distinction.

## Where it came from

A feasibility study costed four payment routes and recommended an electronic
coin acceptor, then specified something deliberately smaller as the first
build, on the grounds that the expensive parts should wait until the cheap risk
is retired.

| Full design | This build | Why |
|---|---|---|
| L298N motor driver | a relay | Simpler, wastes no voltage, shrugs off a stalling motor |
| Cam and home switch to count one turn | the drop sensor stops the motor | Fewer parts, and it stops when the job is done rather than guessing |
| Status light and screen | screen and buzzer | The light said nothing the other two did not |
| Battery, sleep, wake button | wall adaptor | Not needed to prove the mechanism |
| Steel cabinet 45 × 20 cm | open bench build | The cabinet is the most expensive item. It waits |

## What the client agreed to

- **KSh 20 per pad**
- **Multiple coins totalled.** Four KSh 5, two KSh 10, or one KSh 20 buys a
  pad. One KSh 40 buys two, one after the other
- **No change given** — there is no hopper. Overpayment stays on the meter
- **No reverse.** A relay drives the coil one way only. A jam is detected and
  reported, not backed out of. Permanent once the motor is bought
- **Speed fixed at purchase.** The relay has no PWM
- **95% coin recognition** is the acceptor's rating. Roughly one good coin in
  twenty is refused and must be re-inserted. Not a fault, cannot be tuned away

## What it does not demonstrate

- Not the finished size or appearance — an open rig on a board
- Not the battery or the no-mains promise — it runs from a wall adaptor
- Not capacity or refill interval — a short bench coil is not 17–22 pads
- Not every pad — the coil is wound to one pitch

---

# Tour of the firmware

One file, `pads_vending_machine_with_coin_acceptor.ino`, about 3900 lines of
which roughly a third is comment. It is laid out in two halves separated by a
banner: **CONFIG** above, **machinery** below. Nothing below the banner needs
changing to commission a machine.

## CONFIG — the part you edit

| Section | Holds |
|---|---|
| 1 | Pin map |
| 2 | **The coin table** — pulses → shillings, per coin |
| 3 | Price, change policy, persistence, hold-recovered-pad |
| 4 | IR / relay / stock-switch polarity, LCD address, service contact |
| 5 | **All timing.** Three values adjustable live from the console |
| 6 | Buzzer tones |
| 7 | Console behaviour, logging switches, self-test strictness |

Every timing constant in SECTION 5 is written up the same way: what it
controls, what goes wrong if too short, what goes wrong if too long, how to
find the right value on the bench, and its live console command if it has one.

## The six states

| State | Coins | Means |
|---|---|---|
| `ST_IDLE` | **accepted** while short of a pad | Waiting for money |
| `ST_PAID` | inhibited | Price met: `KSH 20 - PAID!` for 2 s, waiting for the coin line to go quiet |
| `ST_DISPENSING` | inhibited | Motor turning, watching the beam |
| `ST_DELIVERED` | inhibited | Pad fell, `PLEASE COLLECT YOUR PAD` holding |
| `ST_EMPTY` | inhibited | Stock switch says no pads. **Not a fault**, not latched — returns to service on a refill |
| `ST_FAULT` | inhibited | Needs a person. Latched |

**The rule the design rests on:** the coin slot is live only in `IDLE`, while
the credit is short of a pad. It is derived from the state in **one line** of
`loop()` and written nowhere else, so the slot can never disagree with what the
machine is doing.

It shuts during a sale because the coin line is muted while the motor turns,
and a coin counted then would be lost. A coin already inside the acceptor when
the slot shuts is still counted: `PAID` waits for the coin line to go quiet
before the motor starts (`DESIGN_DECISIONS.md` decision 2).

## How a coin is read

A multi-coin acceptor does not say "that was twenty shillings". It says "that
coin was channel 3" by sending three pulses. Reading it is two jobs, split on
purpose:

- **`onCoinPulse()`** runs in an interrupt on every falling edge. It does the
  minimum — count, timestamp, return — because an interrupt that dawdles loses
  the next pulse. It also debounces so ringing is not counted twice.
- **`takeCoin()`** runs in `loop()`. It watches for the line to go quiet for
  `COIN_GAP_MS`, which is what "that coin is finished" looks like, and only
  then hands the count over to be priced.

Measured on this machine: **1 pulse = KSh 5, 5 = KSh 10, 10 = KSh 20,
20 = KSh 40**. Each pulse is 50 ms LOW then 105 ms HIGH, so 155 ms apart.
An earlier table of 2 / 10 / 20 / 40 was exactly double: an ESP32 FALLING
interrupt was also firing on each rising edge. The 75 ms debounce filters it
(see `COIN_PULSE_DEBOUNCE_MS` and `FIRMWARE_PROGRESS.md`, 2026-10-01).

**The coin line is muted while the motor runs** and for `MOTOR_NOISE_SETTLE_MS`
after it stops. Switching the coil induces edges on the coin pin that are
indistinguishable from real pulses, and they arrive in ones and twos — which is
the pulse count of the cheapest coin. See `TROUBLESHOOTING.md`.

## The loop, and why its order is load-bearing

```
1. Motor backstop        first, trusting nothing below it
2. Serial + clear button so a person can always get in
3. Watches               live sensor views, if one is running
4. Coins                 decode any finished coin and bank it
5. Stock                 empty → ST_EMPTY, refilled → ST_IDLE
6. Re-enable the slot    only if still IDLE after step 5
7. The state machine     which may start a vend
```

Two of those orderings are not arbitrary:

- **Stock (5) before the acceptor re-enable (6)**, so returning to IDLE over an
  empty coil never clicks the relay on and straight back off.
- **Stock (5) before the state machine (7)**, so credit arriving in the same
  pass as the coil running dry cannot start a vend with no pad to give.

Stock is deliberately **not** checked during a vend. Once the motor is turning,
the IR beam is the only authority on whether a pad fell — not a switch sitting
under a stack that is in the middle of moving.

## The timing trap

`loop()` reads `now` once at the top. Anything that stamps a timestamp with
`millis()` **later in that same pass** produces a value *ahead* of `now`, and
the unsigned subtraction `(now - stamp)` wraps to about 4.29 billion. Every
"has enough time passed?" test built on it then fires instantly.

That is not hypothetical — it shipped, and it cleared part payment on the very
pass it was banked. Two helpers exist to prevent it:

- **`reached(now, stamp + interval)`** to decide
- **`sinceMs(now, stamp)`** to measure

Never write `(now - stamp)` directly against the loop's `now`. Both helpers are
also correct across the `millis()` rollover at 49.7 days.

## Money

Five rules, each a deliberate answer to *"who loses if this goes wrong"*. They
are in the file header and in full in [`DESIGN_DECISIONS.md`](DESIGN_DECISIONS.md).

1. **Pay on delivery**, not on motor start
2. **The slot shuts from "price met" until the pad is collected**, and a coin already inside is still counted
3. **A full pad's worth of credit is never timed out**
4. **Part payment expires** — a policy choice, with a countdown on screen
5. **The meter survives a power cut** — the only thing written to flash

## Persistence

The meter balance is the **only** thing in flash. `setCredit()` is the single
writer; every read goes through `reached()`/`sinceMs()`. Writes are checked,
a stale stored value is never left *above* the real one, and the store is
retried if it refused to open at boot.

`REMEMBER_CREDIT_ACROSS_POWER_LOSS = false` removes it entirely — the NVS
partition is then never opened.

## Display

Sixteen columns is not a reason to write in abbreviations. Long messages are
**paged across frames on word boundaries** rather than shortened, with the top
line held still as an anchor. The accepted-coin list is built from `COIN_TABLE`
at boot so editing SECTION 2 changes what the screen says.

The display also self-heals: anything that dips the rail (motor, either relay,
every coin) flags it for a rebuild, and it rebuilds once a minute regardless.
The backlight sleeps after two idle minutes and **waking is always a full
rebuild**, so the screen is at its freshest exactly when somebody starts using
it.

## Self-test

Nine checks at every boot, repeatable with `d`. A **fatal** result halts before
the coin interrupt is attached, so a machine that would mis-price coins never
counts one.

| # | Checks | Fatal if |
|---|---|---|
| 1 | Reset reason, heap, CPU | — (names BROWNOUT and PANIC) |
| 2 | Coin table, price, change policy | empty table, 0-pulse entry, duplicate pulse counts |
| 3 | Pulse timing, coin line at rest | gap too close to the debounce |
| 4 | I2C scan, LCD present | — (runs headless) |
| 5 | IR polarity, chute clear | **chute blocked at startup** |
| 6 | Relay pin, timeouts | — |
| 7 | Inhibit contact, clear button | — (warns if wired NO) |
| 8 | Stock switch | — (says if bypassed) |
| 8b | Stored balance, service contact | — |
| 9 | Buzzer chirp | — |
