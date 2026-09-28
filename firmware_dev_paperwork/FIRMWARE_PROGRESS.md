# Firmware progress

The living memory for this project. **Read the RESUME block first.** Update it
every time the work moves on, and add a dated section saying what changed, why,
what the behaviour change is, and whether it is verified on hardware.

Mark **✅ verified on hardware** vs **⏳ pending** explicitly. A thing that
compiles is not a thing that works.

---

# ▶ RESUME HERE

**State as of 2026-09-28.**

The firmware is functionally complete and every coin path is verified on real
hardware. What remains is **mechanical and electrical**, not code.

### Verified on hardware ✅

- All four coins decode: 2 / 10 / 20 / 40 pulses → KSh 5 / 10 / 20 / 40
- Accumulation: 4 × KSh 5 and 2 × KSh 10 both reach KSh 20 and vend
- KSh 40 vends two pads back to back
- Change carries across a vend and lands on the correct screen
- Meter survives a power cut; recovered pad is held and released by button or coin
- Jam retry, the three-attempt cap, and the customer button
- Coin-noise muting (the machine no longer spends induced pulses)

### Pending ⏳ — all of it physical

| | Blocked on |
|---|---|
| **Real drop times** | No chute or panel. Every timing so far is a finger at the IR sensor, which measures how fast a hand moves |
| **`DISPENSE_TIMEOUT_MS`** | Currently **7000 ms placeholder**. Run `T25` once pads fall down a real chute and set twice the slowest |
| **Do pads feed off the coil without jamming** | The number one risk in the whole project, still entirely open |
| **Motor noise → zero** | Down from 70/s to 5–46/s. Needs 100 nF across the motor terminals |
| **Inhibit relay polarity** | Run `n0` / `n1` and set `INHIBIT_RELAY_ACTIVE_HIGH` from what the relay does |
| **LCD** | Not wired. I2C scan finds nothing |

### Next session should

1. Wire the LCD — everything the customer sees is currently invisible
2. `n0` / `n1`, settle the relay polarity, fix the pull-up/pull-down note
3. 100 nF across the motor terminals, watch `motor noise` in `s` fall
4. Build the chute, load the coil, run `T25`, bake the timeout in
5. Then the 50-cycle soak — that number is what the client actually asked for

---

## 2026-09-28 — Paperwork, soak command, silent boot

**What changed.** Added `T<n>`, a soak that runs *n* vends back to back, times
every drop, and reports twice the slowest as the timeout to set. Added
`SERIAL_LOG_BOOT` so a field machine can boot completely silently. Created this
paperwork set.

**Why.** `DISPENSE_TIMEOUT_MS` had been set three times from whatever happened
to be in the last log, and once from a finger at the sensor. That is not a
measurement. The soak makes it one — and its ceiling is deliberately far longer
than the real timeout, because a stopwatch that stops before the event cannot
measure it.

`SERIAL_LOG_DEFAULT` did not cover the boot: the banner and self-test were
forced on regardless, so a unit set quiet still printed a second of text at
every power-up. "Off" was not off.

**Behaviour change.** A failing self-test on a quiet boot is re-run with the
console forced on. Silent when healthy, loud when broken.

**Verified.** ⏳ Compiles clean. Soak not yet run against real pads.

---

## 2026-09-28 — A recovered pad waits to be collected

**What changed.** A recovered balance covering a whole pad is now **held**
rather than dispensed on boot. A button press or a coin releases it.

**Why.** Power comes back at three in the morning, the machine wakes owing a
pad, and drops one into an empty washroom. Dispensing to nobody is not
honouring the debt — it is losing a pad on top of the money.

**Also fixed.** `REMEMBER_CREDIT_ACROSS_POWER_LOSS = false` did not fully
remove persistence: `setup()` still called `prefs.begin()` unconditionally. The
NVS partition is now never opened when the feature is off.

**Verified.** ✅ On hardware — both release routes seen in the log.

---

## 2026-09-27 — The unsigned underflow that broke KSh 5 and 10

**The bug.** Part payment was cleared on the very pass it was banked. Signature:

```
cleared after an actual wait of 4294967 s (limit 120 s)
```

2³²−1 ms. `lastCoinMs` was *ahead* of the loop's `now`, so `(now - lastCoinMs)`
wrapped to 4.29 billion and every timeout test built on it fired instantly.

**Cause — one word, a regression.** `loop()` reads `now` once at the top. The
original code stamped `lastCoinMs = now`. When banking moved into `bankCoin()`
it became `lastCoinMs = millis()`, which runs later in the same pass and is
therefore larger than `now`.

**Why only 5 and 10.** KSh 20 and 40 meet the price and vend before the timeout
check is ever reached. Only partial credit goes down that path.

**Fix.** Added `sinceMs()` to measure, used the existing wrap-safe `reached()`
to decide, and converted **all eight** comparisons of the stale `now` against a
stamp written later in the same pass. Two were live bugs: this one, and the
backlight sleeping on the first coin pulse.

**Verified.** ✅ On hardware.

---

## 2026-09-26 — Motor noise was being spent as money

**The bug.** KSh 40 vended three pads. Phantom 2-pulse "KSh 5" coins appeared
immediately after every `Motor on`.

**Cause.** Switching the coil induces edges on GPIO 18 indistinguishable from
coin pulses, arriving in ones and twos — the pulse count of the cheapest coin.
Up to **472 induced pulses in one 5-second run**.

**It was a regression of mine.** The older sketch *rejected* mid-vend coins, so
the noise was invisible. Changing to bank them turned it into money.

**Fix.** The coin interrupt counts nothing while the motor runs, nor for
`MOTOR_NOISE_SETTLE_MS` after. Discarded pulses are counted and reported with
their **rate**, because a fixed burst per run and a steady rate scaling with run
time are different faults needing different parts.

**Later finding.** The rate was 70/second steady — **brush arcing**, not the
switching spike. A flyback diode alone does nothing for it; it needs 100 nF
across the motor terminals. The earlier advice was wrong.

**Verified.** ✅ Muting works. ⏳ Suppression not finished — rate is 5–46/s.

---

## 2026-09-25 — The money model

A run of review rounds reshaped how money is handled. The full reasoning is in
[`DESIGN_DECISIONS.md`](DESIGN_DECISIONS.md); the headlines:

- **Pay on delivery, not on motor start.** Charging at motor start meant taking
  money for a pad that never came out
- **The meter survives a power cut.** Without it, a cut with KSh 5 on the meter
  was a clean swallow with no record anywhere
- **Never leave the stored balance above the real one** — that direction can
  vend a pad nobody paid for
- **A full pad's worth of credit never times out**, in any state
- **The retry button is a customer control**, on the front. `attemptClear()`
  was zeroing the credit, which threw away her money at the exact moment she
  was standing there asking for the pad

**Verified.** ✅ All on hardware.

---

## Recurring lesson

Three separate defects in this project came from **claims made without
checking**: that `putUShort()` compares before writing, that it was "three
writes per sale", that clearing a stale balance was strictly safe. Each was
fixed not by writing a more careful comment but by **removing the dependency on
the claim** — a shadow copy makes "identical writes are skipped" true by
construction; a `>` comparison makes the safety argument hold without a caveat.

A second pattern: every time the logic changed, a comment somewhere else kept
describing the old design. The inhibit rule, the state table, and the flash
claim were all the same failure. **A wrong comment is worse than none**, because
the next person reasons from it.
