# About this project — Coin-operated sanitary pad dispenser

*A low-cost machine that gives a woman a sanitary pad the moment she needs one:
a coin goes in, a pad comes out. Built for washrooms in schools, clinics and
public buildings in Kenya.*

**Client:** Rebecca Mpiima · **Ref:** `BT-PADVEND-2026-001` · Built by Bricapa
Technologies.

> **New here? This is the one page to read first.** It explains what the
> project is, how it works, and exactly where it stands. For operating the
> machine see [`OPERATING_MANUAL.md`](OPERATING_MANUAL.md); for the full
> engineering record see
> [`firmware_dev_paperwork/`](firmware_dev_paperwork/README.md).

---

## The problem

Across many schools, clinics and public washrooms in Kenya, a sanitary pad is
simply not available at the moment it is needed. A girl who is caught out
misses class or goes home; a woman in a public building has no option at all.
Stocking an open basket does not work — pads disappear, and nobody is
accountable for refilling or paying for them. What is missing is a way to make
a single pad available, on demand, for a small coin, that a caretaker can keep
stocked and that pays for itself.

## The solution

A small, wall- or stand-mounted machine. A customer inserts coins; when they
reach the price of one pad (currently **KSh 20**), the machine turns a coil
that pushes a single pad down a chute to her. It accepts **KSh 5, 10, 20 and
40** coins, shows clear instructions on a screen, and is looked after by a
caretaker who refills the pads and empties the coin box.

It is deliberately simple and **cheap to build from parts available in any
Nairobi electronics shop** — about **KSh 9,000 (≈ USD 65)** in components — so
it can be made and maintained locally, not imported.

## How it works

```
coin in  →  the machine reads which coin it was  →  adds it to the total on screen
         →  total reaches the price  →  a motor turns the coil
         →  the pad falls and breaks an infrared beam  →  the beam confirms delivery
         →  only now is the payment counted.
```

The last step is the heart of the design: **the customer pays on delivery.**
The machine takes the money only once an infrared sensor has confirmed a pad
actually fell. If a pad jams or the machine is empty, no money is taken, and
anything already paid is remembered and honoured when the machine is working
again. A customer is **never charged for a pad that does not come out.**

## Where it stands today

A complete, working prototype is **assembled and running**, with the following
proven on real hardware:

- **Coin handling:** all four coins decode with 100% accuracy; overpayment and
  part-payment are handled correctly; KSh 40 buys two pads.
- **Reliability:** a **six-day continuous run** with stable memory and **zero**
  false coin counts from motor electrical noise; a software **watchdog** that
  restarts the machine within five seconds if it ever hangs.
- **Safety of money:** the pay-on-delivery rule, power-cut recovery (a paid
  balance survives a power loss), and fail-safe behaviour (a dead or booting
  controller cannot take a coin).
- **The core mechanism:** the first real pads fed off the coil and dropped
  cleanly in 1–2 seconds each — the first proof that pads dispense without
  jamming.

### What is next

The one remaining step before a field pilot is a **full reliability soak**
(dispensing 25–50 pads in a row) to prove the coil over many cycles and set the
final timing. The 12-month plan then moves to a pilot at one site, a small
locally-built batch, and multi-site deployment (see the roadmap below).

## Why it is engineered the way it is

This was not a weekend build; it was made to survive a public washroom for
months unattended. Some of that rigour:

- **Three controller boards were destroyed early on.** We traced it to
  high-voltage spikes (24–36 V) thrown onto the signal wire by the coin
  mechanism's coil, and fixed it with opto-isolation. The machine has been
  stable since. The full investigation is recorded in
  [`firmware_dev_paperwork/LESSONS_LEARNED.md`](firmware_dev_paperwork/LESSONS_LEARNED.md).
- **Every design decision about money is written down and justified** in
  [`firmware_dev_paperwork/DESIGN_DECISIONS.md`](firmware_dev_paperwork/DESIGN_DECISIONS.md),
  because the one thing a vending machine must never do is take someone's money
  and give nothing back.
- **The machine explains itself** on its screen in plain language, and recovers
  from faults on its own wherever it safely can.

## What is in this repository

| Where | What |
|---|---|
| `pads_vending_machine_with_coin_acceptor.ino` | The complete firmware (one file, ESP32). |
| [`OPERATING_MANUAL.md`](OPERATING_MANUAL.md) / `.pdf` | A plain-language manual for the owner and caretaker. |
| [`README.md`](README.md) | The build guide: parts list, wiring, and assembly. |
| [`firmware_dev_paperwork/`](firmware_dev_paperwork/README.md) | The full engineering record: design decisions, how it works, bench-test procedures, lessons learned, and the dated progress log. |

## Roadmap — next 12 months

- **Months 1–3:** 50-cycle reliability soak with real pads; final dispense
  timing; a durable lockable enclosure.
- **Months 3–6:** Field pilot at one school or clinic washroom in Nairobi;
  collect usage, refill and fault data, and caretaker feedback.
- **Months 6–9:** Apply the pilot learnings; assemble a small batch (~10 units)
  from locally-sourced parts; establish restocking and maintenance.
- **Months 9–12:** Deploy to 5–10 sites; measure pads dispensed and uptime;
  prepare a costed plan to scale.

---

*Prototype firmware v2.0. The machine demonstrates the core promise — a coin
goes in and a pad comes out — honestly, cheaply, and reliably enough to take to
a real washroom.*
