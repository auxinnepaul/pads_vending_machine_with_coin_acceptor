# Design decisions

Every decision that is not obvious, with the reasoning and what was rejected.

**Read this before changing anything that touches `credit`.**

---

## The fact everything follows from

> **The coin is in the cash box before the firmware knows it exists.**

The acceptor banks a coin and only *then* sends its pulses. There is no eject,
no return chute and no hopper. Nothing in this machine can give anybody their
money back — not a fault handler, not a timeout, not a reboot.

So the firmware does not protect the cash. It protects **the claim**: its own
record that it owes somebody a pad. Every rule below is an answer to *"who
loses if this goes wrong"*.

---

## MONEY

### 1. The customer pays on delivery, not when the motor starts

**Decision.** Credit comes off the meter at the moment the beam confirms a pad
fell, and nowhere else. There is exactly one line in the firmware that spends
credit.

**Why.** The machine cannot refund. The only way it can be honest about a vend
that fails is to never have taken the money in the first place. A jam, an empty
coil, a mis-aimed sensor or a missed beam all leave the credit on the meter
where the customer can see it, and it buys a pad as soon as the machine works
again.

**Rejected:** charging at motor start, which is what shipped first. It meant
taking money for a pad that never came out. Found in review.

**Known race, accepted.** There are a few milliseconds between the pad landing
and the new balance reaching flash. A power cut in exactly that gap makes the
machine boot owing a pad it already delivered, and vend one more. Closing it
means writing flash *before* confirming delivery, which reopens the far worse
failure above. Given a choice of which way to be wrong in a window that small,
**err toward the customer**.

### 2. A coin landing mid-vend is banked, not discarded

**Decision.** Coins arriving in `DISPENSING` or `DELIVERED` go on the meter and
buy the next pad. Coins arriving in `EMPTY` or `FAULT` are refused.

**Why.** Inhibiting the acceptor stops the *next* coin; one already rolling
through the mechanism still completes and lands mid-state. The machine
physically has that money. Those two states end in `IDLE` within a second or
two, so it **can** deliver — throwing the coin away would mean keeping the money
and giving nothing back. `EMPTY` and `FAULT` genuinely cannot deliver, so there
the money must not go on the meter.

### 3. A full pad's worth of credit is never timed out

**Decision.** Part payment expires. Credit that already covers a pad does not,
in any state, ever.

**Why.** That is a debt the machine has already been paid for. It waits —
through `IDLE`, through `EMPTY`, through a refill. Wiping it would put money in
the box with no pad out of it, which is the exact failure rule 1 exists to
prevent; it would be pointless to fix that at the vend and undo it with a
timeout twenty seconds later.

**This was a real bug.** `ST_EMPTY` wiped *all* credit while `ST_IDLE` wiped
only part payment, so a customer who paid a full KSh 20 into a just-emptied
machine lost the claim 20 seconds later with her coin already in the box.

### 4. Part payment expires, and that is a policy choice

**Decision.** Two minutes by default (`CREDIT_TIMEOUT_MS`), with a countdown on
the screen. `0` waits forever.

**Why it is a choice and not a default.** No setting returns the money. All the
number decides is **who ends up with it**:

| Setting | Who gets abandoned part-payment |
|---|---|
| Shorter | the business keeps it |
| Longer | she gets a better chance to find the second coin |
| `0` | it always eventually buys a pad — but for whoever walks up next |

Twenty seconds was the original value and was too short for the one situation
it exists to handle: someone digging in a bag, in a washroom, in a hurry.

### 5. The meter survives a power cut

**Decision.** The balance is written to flash whenever it changes. It is the
**only** thing this firmware stores.

**Why.** Without it, a cut with KSh 5 on the meter is a clean swallow: the coin
is in the box, the firmware forgets it ever arrived, and nobody — not the
customer, not the log, not the takings — has any record. Mains cuts are not
hypothetical at a Kenyan site.

**What it does not fix.** It restores the *claim*, not the cash.

**Cost.** One write per coin plus one per vend — five writes for a KSh 20 pad
paid in KSh 5 coins. The smallest coin in the table sets the worst case, so
re-check this if a smaller one is added. About 90,000 writes a year at fifty
sales a day, which NVS wear-levelling absorbs comfortably.

**Removable.** `REMEMBER_CREDIT_ACROSS_POWER_LOSS = false` and the NVS
partition is never opened at all.

### 6. A recovered pad waits to be collected

**Decision.** A recovered balance covering a whole pad is **held**. The screen
says so. A button press *or a coin* releases it.

**Why.** Power comes back at three in the morning, the machine wakes owing a
pad, and drops one into an empty washroom. It lies in the tray until whoever
opens up finds it, or it walks. **Dispensing to nobody is not honouring the
debt, it is losing a pad on top of the money.** Held, the pad stays in the coil
where it is safe until a person is actually there.

A coin counts as "a person is there" because the next customer walks up and
pays without reading anything.

### 7. Never leave the stored balance *above* the real one

**Decision.** If a flash write fails, the stored value is overwritten with zero
**only if it is higher** than the meter. A stored value at or below the meter is
left exactly where it is.

**Why — the two error directions are not equally bad:**

| Stored | Next boot | |
|---|---|---|
| **Above** the meter | over-credits — a full pad's worth makes the machine wake up, believe it owes a pad, and vend into an empty washroom | **dangerous** |
| **Below** the meter | under-credits — she recovers part of her claim. Worse than a working machine, better than nothing, and it can never produce a free pad | **safe** |

**Rejected:** clearing on any write failure, which destroyed a claim that had
saved successfully earlier. Someone who put in KSh 10, had it stored, then
added a second KSh 10 whose write failed would lose **both**.

### 8. A failing store keeps trading, loudly

**Decision.** If flash stops accepting writes, or never opens, the machine says
so once and keeps selling. `s` and `d` keep reporting it.

**Why.** Refusing sales because flash is unhappy turns a degraded machine into
a dead one. The degradation must be visible, not silent.

A store that would not open at boot is **retried every 30 seconds**, because
until it opens we cannot see whether an old balance is sitting in NVS waiting
to be restored on some later boot. The danger was never the failure; it was the
failure being invisible.

---

## SERVICE AND FAULTS

### 9. The retry button is a customer control

**Decision.** On the front, where she can reach it. Held for one second it
clears the fault, **keeps her credit**, and dispenses on the next pass.

**Why.** By the time a vend fails her coin is already in the cash box and the
machine owes her a pad. Making her find a member of staff to collect what she
has paid for is not a reasonable thing to ask in a washroom.

**Rejected:** mounting it inside the locked door as an attendant control, which
is what shipped first — along with `attemptClear()` zeroing the credit, which
would have thrown away the KSh 20 at the exact moment she was standing there
asking for her pad.

### 10. Three retries, then it is a staff problem

**Decision.** `MAX_BUTTON_RETRIES = 3`. After that the screen stops telling her
to try and starts telling her who to call. The console `x` still works and
forgives the count. Any successful delivery restores the budget.

**Why.** A jam that survives three tries will not clear itself, and carrying on
just works the coil against whatever is stuck.

### 11. A refill clears a latched jam

**Decision.** The stock switch going away and coming back is treated as a
service visit: fault cleared, retry budget restored, back to work.

**Why.** Nothing the machine can sense tells it a jam has been *fixed* — the
beam was already clear while it was jammed. But emptying and refilling the coil
is something only a person with the door open can do, and it is exactly what
fixing a jam involves. Without this, a jam that spent its three retries stayed
dead until somebody thought to bring a laptop.

### 12. Out of stock is not a fault

**Decision.** `ST_EMPTY` does not latch. Coins inhibited, screen says
`SORRY - NO PADS`, and it returns to service on its own the moment the switch
says pads are back.

**Why.** Latching on "empty" would leave a site dead until somebody drove
there, when all it needed was the refill that was already scheduled.

### 13. A dispense timeout asks the stock switch which it was

**Decision.** Timeout with the switch reading empty → `EMPTY` (self-recovers).
Switch reading full → it cannot be empty, so it is a jam: latches `JAMMED`. No
switch fitted → cannot tell, stays cautious: `EMPTY OR JAMMED`.

**Why.** Running dry and jamming want opposite handling, and the switch is the
only thing that can tell them apart.

### 14. Faults latch, and there is no reverse

**Decision.** The machine stops and reports rather than reversing or retrying
by itself.

**Why.** The relay drives the coil one way only — a deliberate cost decision
agreed with the client, and permanent once the motor is bought.

---

## HARDWARE AND SAFETY

### 15. The stock switch is closed when full

**Decision.** Closed to GND (pin LOW) = pads present. Open (HIGH) = empty.

**Why.** A broken wire, a pulled connector or a corroded contact all leave the
pin floating HIGH, which reads EMPTY — so the machine stops taking money. Wired
the other way, those same faults read FULL and it cheerfully sells pads it does
not have. **A failure should cost a sale, never a customer's money.**

`PAD_SWITCH_FITTED` bypasses the check entirely for a machine without the
switch, because an unwired pin floats HIGH and would otherwise freeze it.

### 16. The inhibit relay uses the NC contact

**Decision.** Normally-closed, so the contact is closed — acceptor inhibited —
whenever the board is unpowered, booting, halted or crashed.

**Why.** A dead controller refuses money instead of eating it.

**Cost, stated honestly.** With NC, *accepting* requires the relay to be held
energised, which is most of the time. `INHIBIT_CONTACT_IS_NC = false` swaps
that at the price of the fail-safe.

### 17. A hard motor limit above the dispense timeout

**Decision.** `DISPENSE_TIMEOUT_MS + MOTOR_LIMIT_MARGIN_MS`, checked every pass
of `loop()` in every state, trusting nothing else to stop the motor.

**Why.** It is a backstop against the state machine itself failing, not a
second timeout. Setting it *at* the dispense timeout would let a slow loop trip
it during a legitimate vend and report a firmware bug that is not there. If it
ever fires, the console says so and calls it a bug.

`m<ms>` and `T<n>` block, so the backstop is not running during them — their
own ceilings are the limit. Fine for a command an operator types while
watching; wrong for anything automatic, which is why nothing automatic uses it.

### 18. The coin line is muted around motor runs

**Decision.** The coin interrupt counts nothing while the motor runs, nor for
`MOTOR_NOISE_SETTLE_MS` after it stops. Any burst in progress is binned.

**Why.** Switching the coil induces edges on the coin pin indistinguishable
from real pulses, arriving in ones and twos — the pulse count of the cheapest
coin. Measured on this machine: up to **472 induced pulses in one 5 s run**.
Counted, they become credit nobody paid, and pads nobody paid for.

**Inhibiting the acceptor does not help here** and it is worth being clear why:
inhibit stops the mechanism taking a coin, it does nothing about induced
voltage on a signal wire. Muting the counter is the only firmware answer.

This is a **plaster over a hardware fault**. Discarded pulses are counted and
reported so the suppression can be driven to zero.

---

## SCREEN

### 19. Long messages are paged, not abbreviated

**Decision.** Sentences are broken into 16-character pages on word boundaries
and cycled, with the top line held still as an anchor.

**Why.** `HOLD BTN 1 SEC` is what you get from squeezing a sentence into the
hardware. `HOLD THE BUTTON / FOR 1 SECOND / TO TRY AGAIN` is the same hardware
used properly, and a stranger in a hurry can read it.

The accepted-coin list is built from `COIN_TABLE` at boot, so editing SECTION 2
changes what the screen says without anybody keeping a second copy in step.

### 20. The fault screen says different things to different people

| Situation | Screen |
|---|---|
| Credit on the meter, retries left | `PAID - NO PAD` / how to retry |
| Retries spent | `JAMMED` / the number to call |
| Nothing owed | `OUT OF ORDER` |

`OUT OF ORDER` tells a paying customer nothing she can act on.

### 21. A missing LCD degrades to headless

**Decision.** Presence is probed over I2C. If absent, `say()` silently does
nothing and the machine still vends.

**Why.** A dead screen should not take the whole machine down.

### 22. The backlight sleeps, and waking is a full rebuild

**Decision.** After two idle minutes the backlight goes out. The **first coin
pulse** brings it back — not the finished coin — and waking always
re-initialises the display.

**Why.** The backlight is nearly all of what the display consumes (~20 mA
against ~2 mA) and it is the only part that wears. And waking-as-rebuild means
whatever corrupted the screen while nobody was watching is gone before the
first person sees it.

Only plain idle sleeps. `OUT OF ORDER` and `SORRY - NO PADS` stay lit — that
message is the whole reason the screen is there.

---

## CONSOLE

### 23. Nothing typed at the console is saved

**Decision.** Live tunables (`p`, `g`, `o`, `w`) are lost on reset, by design.

**Why.** The console is for *finding* the number; SECTION 5 is where it lives
once you have it. A machine whose behaviour depends on what somebody typed
last month is not a machine you can reason about.

### 24. A typed command always answers, however quiet the machine

**Decision.** `handleCommand()` forces logging on for its own duration and puts
it back after.

**Why.** A service visit must be able to plug in a laptop and press `s` on a
machine installed silent.

### 25. Two logging switches, and a failing self-test still shouts

**Decision.** `SERIAL_LOG_DEFAULT` silences routine chatter;
`SERIAL_LOG_BOOT` silences the banner and self-test. Both `false` and the
machine says nothing unless spoken to. But if the self-test **fails** on a
quiet boot it is re-run with the console forced on.

**Why.** Silent when healthy, loud when broken. A machine that refuses to work
and will not say why is worse than one that chatters.

### 26. Commands that block refuse while the coil is turning

**Decision.** `d`, `l`, `m` and `T` refuse in `DISPENSING`/`DELIVERED`.

**Why.** While the motor runs, `loop()` is the only thing watching the beam and
enforcing both timeouts. A command that sits in its own loop for a second or
two stops all of that — the pad can fall unnoticed while the coil keeps
turning.

### 27. The dispense timeout is measured, not chosen

**Decision.** `T<n>` soaks the machine, times every drop, and reports twice the
slowest. Status tracks the slowest drop of the session and warns when the
timeout falls under twice it.

**Why.** It was set three times from whatever happened to be in the last log,
and once from a *finger at the sensor* — which measures how fast a hand moves.
The soak waits far longer than the real timeout on purpose: a stopwatch that
stops before the event cannot measure it.

**The current 7000 ms is a placeholder and must be re-measured** once pads fall
down a real chute.
