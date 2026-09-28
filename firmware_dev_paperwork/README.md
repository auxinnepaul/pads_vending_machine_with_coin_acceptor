# Pad Dispenser — Paperwork Index

*Build. Connect. Automate.*

The documentation set for `pads_vending_machine_with_coin_acceptor.ino`.
**Start here** — this page says which doc to read for what.

Client: Rebecca Mpiima · Ref `BT-PADVEND-2026-001` · Firmware v2.0

---

## Which doc do I need?

| I want to… | Read | Audience |
|---|---|---|
| **Understand the whole machine** (what it is, what it proves, what it deliberately does not, a tour of the firmware) | [`OVERVIEW.md`](OVERVIEW.md) | **Everyone — start here** |
| **See what's been built, decided, verified on hardware, and what's next** | [`FIRMWARE_PROGRESS.md`](FIRMWARE_PROGRESS.md) | Anyone picking up the work |
| **Know why the money is handled the way it is** (pay-on-delivery, the claim vs the cash, why part payment expires, why a recovered pad waits) | [`DESIGN_DECISIONS.md`](DESIGN_DECISIONS.md) | **Read before changing anything that touches `credit`** |
| **Bring a machine up, or find the dispense timeout** (the order, what "good" looks like at each step, the soak procedure) | [`BENCH_TESTING.md`](BENCH_TESTING.md) | Whoever is at the bench |
| **Look up a console command** (all of them, what each is actually for) | [`CONSOLE_COMMANDS.md`](CONSOLE_COMMANDS.md) | Developer / field |
| **Debug something that is misbehaving** (LCD gibberish, phantom coins, a relay that will not switch, credit that vanishes) | [`TROUBLESHOOTING.md`](TROUBLESHOOTING.md) | Whoever is cursing at the machine |
| **Pick the next thing to build** (what is parked, what stage two needs) | [`IMPROVEMENTS_BACKLOG.md`](IMPROVEMENTS_BACKLOG.md) | Whoever is planning the next session |

The [top-level README](../README.md) is the **build guide**: bill of materials,
wiring, pin map, suppression, and the three things that kill hardware. Start
there if you are assembling a machine. Start here if you are changing one.

---

## The rule everything else follows from

**The coin is in the cash box before the firmware knows it exists.**

The acceptor banks a coin and only then sends its pulses. There is no eject, no
return chute and no hopper anywhere in this machine. Nothing here can give
anybody their money back — not a fault handler, not a timeout, not a reboot.

So the firmware does not try to protect the cash. It protects **the claim**:
its own record that it owes somebody a pad. Every money decision in
[`DESIGN_DECISIONS.md`](DESIGN_DECISIONS.md) follows from that one fact, and
none of them make sense without it.

---

## Status at a glance

| | |
|---|---|
| Firmware | Complete, compiles clean on ESP32 core 2.0.11 |
| Size | ~333 KB flash (25%), 22 KB RAM |
| Coin handling | ✅ verified on hardware — all four coins, accumulation, change |
| Power-cut recovery | ✅ verified on hardware |
| Jam retry / customer button | ✅ verified on hardware |
| Drop times | ⏳ **not measured** — no chute yet, every timing so far is a finger at the sensor |
| Pads feeding off the coil | ⏳ **untested** — the number one risk in the whole project |
| LCD | ⏳ not wired |
| Coin inhibit relay | ⏳ polarity unconfirmed against the real acceptor |

`FIRMWARE_PROGRESS.md` is the living detail behind this table.
