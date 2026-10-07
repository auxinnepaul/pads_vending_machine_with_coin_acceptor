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

### 2. The slot shuts from "price met" until the pad is collected, and a coin already inside is still counted

**Decision (revised 2026-10-02).** The acceptor is live only in `IDLE` while
the credit is short of a pad. The instant the price is met it shuts, and it
stays shut through `PAID`, `DISPENSING` and `DELIVERED`. Before the motor
starts, `PAID` holds for `PAID_HOLD_MS` **and** waits for the coin line to go
quiet (up to `PAID_MAX_WAIT_MS`). Any coin that lands in those states (one
already inside the acceptor when the slot shut) goes on the meter and buys the
next pad. Coins arriving in `EMPTY` or `FAULT` are refused.

**Why.** The original decision kept the slot open during a vend and banked
mid-vend coins. That stopped being safe a day later, when decision 18 muted the
coin line while the motor turns: a coin inserted then has its pulses binned, so
the money is in the box and nothing is credited. The two decisions were never
reconciled until the builder noticed coins could go in mid-vend (2026-10-02).

Shutting the slot stops new coins. It cannot stop one already rolling through
the mechanism, which still pulses after the slot shuts; waiting for the coin
line to go quiet before the motor starts is what makes sure that coin is
counted instead of muted. A KSh 40 is about three seconds of pulses, hence the
8 s ceiling.

**Cost:** two relay operations per sale. A relay is rated for 100,000+.

**Rejected: shutting the slot for a couple of seconds after every coin.**
Counting is not the problem: two coins as fast as the acceptor will take them
read as two. A coin dropped in while the slot is shut is rejected back out,
which reads as a broken machine, and it would work the relay twice per coin.
The screen acknowledges each coin without needing the slot shut.

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

### 10b. A machine out of retries stays inhibited, and that is not configurable

**Decision.** When the retries are spent the coin slot stays shut. There is no
setting to let it keep accepting coins.

**Why not a client preference.** Because the machine genuinely cannot tell
these three apart — the beam is its only evidence and all three look identical
from inside:

1. a pad wedged in the coil
2. the coil empty, if no stock switch is fitted
3. **the IR sensor blind** — pads may be dropping perfectly and the machine
   simply cannot see them

**A machine that cannot tell whether pads are coming out must not keep taking
money for them.** That is the whole principle the payment model rests on, and
a flag that turns it off is not a preference — it is an option to build the
one failure this design exists to prevent, and at scale: every customer who
walks up, for as long as nobody notices.

**And the cost of being wrong is asymmetric.** If the sensor is blind and pads
really are dropping, the customer who paid already has her pad; refusing the
next coin costs a **sale**. If it is a real jam and the slot stayed open, every
coin after it is money taken for nothing. Same asymmetry as the stock switch
polarity: a failure should cost a sale, never a customer's money.

**What the code does say now.** The screen says `JAMMED` because it is the
short word that fits sixteen columns, but the console lists all three causes
and says plainly that the sensor is one of them — *check it with `i` before
assuming the mechanism is at fault*. The machine was over-claiming a diagnosis
it had not earned.

The refusal is also rate-limited to once every 15 seconds. She will keep
pressing, and forty identical lines bury whatever comes next in the log.

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

### 22b. The sale says what it understood, step by step

**Decision (2026-10-02).** Each step of a sale gets its own screen, held long
enough to read, instead of jumping from a coin straight to a running motor:

| Moment | Top line | Bottom line | Held |
|---|---|---|---|
| A coin, still short | `THAT IS KSH 5!` | `TOTAL KSH 5` | 1.5 s (`COIN_ACK_MS`) |
| ...then | `ADD KSH 15 MORE` | `TIME LEFT 1:58`, counting down | until the next coin |
| Price met | `KSH 20 - PAID!` | `A FULL PAD!` | 2 s (`PAID_HOLD_MS`), slot shut |
| KSh 40 | `KSH 40 - PAID!` | `2 PADS COMING!` | 2 s |
| Motor running | `HERE IT COMES...` | `PAD 1 OF 2` when there are two | until the pad drops |
| Delivered | `PLEASE COLLECT` | `YOUR PAD :)` | 2.5 s (`MESSAGE_HOLD_MS`) |
| Second of two | `ONE MORE COMING` | `PAD 2 OF 2` | 1 s (`NEXT_PAD_HOLD_MS`) |
| Done | `THANK YOU!` | `HAVE A GOOD DAY`, or `CREDIT KSH 5` if change is left | 2 s (`THANKS_MS`), slot open again |

**Why.** The builder's words: it should feel like the machine "knows what it
is doing and understands itself", and "TAKE YOUR PAD" read as an order. The
pause on `PAID!` is also the money rule in decision 2.

**None of the holds blocks.** They are deadlines in `loop()`, so the beam and
the coin line are watched throughout. A coin that arrives during a held screen
simply updates it.

### 22c. Every page of a paged message must make sense on its own

**Decision (2026-10-08).** Customer messages mark their page breaks with `|`
instead of leaving the pager to fill 16-character pages word by word, and no
screen says `SEE SERIAL`; it says `PLEASE CALL THE` / `NUMBER ABOVE`, with the
number held on the top line.

**Why.** Word-by-word paging split `... - NO COINS ARE BEING TAKEN` as
`SEE SERIAL - NO` / `COINS ARE BEING` / `TAKEN`. The builder read the last
two pages and reported that the machine said coins **were** being taken. A
passer-by may only catch one page, so a negation that lands on the previous
page reverses the message. "No coins are being taken" is now
`COIN SLOT CLOSED`, a single page with no negation to lose.

### 22d. A blocked beam at rest is a sensor fault that clears itself

**Decision (2026-10-08).** The beam is read on every pass of `loop()`. If it
reads BLOCKED for `IR_STUCK_WARN_MS` (5 s) while the machine is idle, the
machine goes to `FAULT` `CHECK SENSOR` and the slot shuts. That fault, the
`CHECK SENSOR` refusal before a vend, and a boot halt for `CHUTE BLOCKED`
**clear themselves** once the beam reads clear for `BEAM_CLEAR_RECOVER_MS`
(3 s): a boot halt re-runs the whole self-test first.

**Why.** An unplugged sensor (with the 100 kΩ pull-down) or a pad stuck in the
chute both read BLOCKED, and a machine that cannot see a pad drop must not
take money. Before, the beam was only read during a vend, so unplugging the
sensor on a running machine went unnoticed, and fixing it at boot needed the
button even though the machine could see the fix.

**This is a narrow exception to decision 14 (faults latch).** It applies only
to faults caused by the beam reading blocked, where a clear beam is direct
evidence that the cause is gone. A **jam** (no pad within the dispense
timeout) still latches: the beam is clear during a jam, so a clear beam proves
nothing there. If an automatic clear fails for any other reason, the machine
stops trying and waits for a person.

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
