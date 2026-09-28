# CLAUDE.md — Pad Dispenser

Guidance for AI-assisted sessions in this repo. Read this first; it front-loads
the architecture and the hard-won gotchas so you are productive from message
one.

## What this is

Firmware for a coin-operated sanitary pad dispenser, built to prove one thing:
**a coin goes in and a pad comes out.** One file,
`pads_vending_machine_with_coin_acceptor.ino`, ESP32, Arduino core 2.0.11.

Client: Rebecca Mpiima · Ref `BT-PADVEND-2026-001`

## The source of truth

**`firmware_dev_paperwork/FIRMWARE_PROGRESS.md` is the living memory.** Its top
**▶ RESUME HERE** block is always the current state. **Read it at the start of a
session**, and **update it every time the work moves on** — the RESUME block
plus a dated `##` section saying what changed, why, the behaviour change, and
the verification status.

Mark **✅ verified on hardware** vs **⏳ pending** explicitly. A thing that
compiles is not a thing that works, and this project has been bitten by that
difference repeatedly.

Doc index: `firmware_dev_paperwork/README.md`.

---

## The one fact that explains the whole design

> **The coin is in the cash box before the firmware knows it exists.**

The acceptor banks a coin and only then sends its pulses. There is no eject, no
return chute and no hopper. Nothing here can give anybody their money back.

So the firmware does not protect the cash. It protects **the claim** — its own
record that it owes somebody a pad. **Every money rule follows from this**, and
none of them make sense without it. Read
`firmware_dev_paperwork/DESIGN_DECISIONS.md` before touching anything that
touches `credit`.

---

## Gotchas that have already cost a bench session each

### 1. `loop()` reads `now` once at the top

Anything that stamps a timestamp with `millis()` **later in the same pass** is
*ahead* of `now`, and `(now - stamp)` wraps to ~4.29 billion. Every "has enough
time passed?" test built on it then fires instantly.

This shipped, and it cleared part payment on the very pass it was banked.

**Never write `(now - stamp)` against the loop's `now`.** Use:
- `reached(now, stamp + interval)` to decide
- `sinceMs(now, stamp)` to measure

Both are also correct across the 49.7-day `millis()` rollover.

### 2. The motor induces coin pulses

Switching the coil puts edges on GPIO 18 indistinguishable from real coin
pulses, arriving in **ones and twos** — the pulse count of the cheapest coin.
Up to 472 in one 5-second run.

The coin interrupt is muted while the motor runs and for
`MOTOR_NOISE_SETTLE_MS` after. **Do not remove that.** Inhibiting the acceptor
does not help: inhibit stops the mechanism taking a coin, it does nothing about
induced voltage on a signal wire.

### 3. The LCD is 16 columns and silently truncates

Anything longer is cut mid-word, which looks like a broken machine. **Render
and measure every screen before committing** — counting characters by eye has
been wrong twice here. Long messages are paged with `sayPaged()` /
`sayAllPages()`, never abbreviated.

### 4. Arduino inserts auto-prototypes above the first function

A struct used in any function signature must be declared above that insertion
point or the sketch will not compile. That is why `CoinBurst` sits at the very
top of the file.

### 5. `prefs.begin()` returns **false** when the store is already open

So "is the store open" and "are writes landing" have to be separate flags.
Retrying `begin()` on a healthy store otherwise looks like a failure and
latches the alarm permanently.

### 6. The loop order is load-bearing

Stock is checked **before** the acceptor re-enable (so the relay never clicks on
and straight back off) and **before** the state machine (so credit arriving as
the coil runs dry cannot start a vend with no pad). Stock is deliberately *not*
checked during a vend — the IR beam is the only authority on whether a pad fell.

---

## Working practice for this repo

**Verify claims against the source, not from memory.** Three defects here came
from asserting how a library behaves without reading it — that `putUShort()`
compares before writing, the write-count arithmetic, and the safety of clearing
a stale balance. Each fix was not a better comment but **removing the dependency
on the claim**.

**Comments drift.** Every time the logic changed here, a comment somewhere else
kept describing the old design — the inhibit rule, the state table, the flash
claim. After changing behaviour, grep for the *subject*, not for the wording you
happen to remember using.

**Compile every change.** The toolchain is on this machine:

```
arduino-builder -compile -fqbn=esp32:esp32:esp32 \
  -hardware "C:/Program Files (x86)/Arduino/hardware" \
  -hardware "$HOME/AppData/Local/Arduino15/packages" \
  -tools "C:/Program Files (x86)/Arduino/tools-builder" \
  -tools "$HOME/AppData/Local/Arduino15/packages" \
  -libraries "$HOME/Documents/Arduino/libraries" \
  -build-path <tmp> pads_vending_machine_with_coin_acceptor.ino
```

A byte-identical binary is the check that a comment-only pass really was
comment-only.

**The `LiquidCrystal_I2C` architecture warning is cosmetic** — its
`library.properties` still says `architectures=avr`. Not an error.

**One canonical copy.** The sketch in this repo is the one that gets flashed.
Diagnosing a copy in `Documents/Arduino/` that has drifted has already cost two
rounds.

---

## What is and is not proven

Everything about coins, credit, persistence and faults is **verified on
hardware**. Everything about *pads physically falling* is not — there is no
chute yet, and every drop time so far came from a finger at the IR sensor.

`DISPENSE_TIMEOUT_MS = 7000` is a **placeholder**, not a measurement. The `T<n>`
soak command exists to replace it with a real number.

**The number one risk in the whole project — do pads feed off the coil without
jamming — is still entirely open**, and no firmware change touches it.
