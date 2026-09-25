/*
  Pads Vending Machine — Coin Acceptor Prototype
  Bricapa Technologies · Client: Rebecca Mpiima
  Ref: BT-PADVEND-2026-001          Firmware v2.0

  ---------------------------------------------------------------------------
  WHAT THIS DOES
  ---------------------------------------------------------------------------
  Coin in  ->  acceptor pulses  ->  firmware decodes which coin it was
           ->  credit reaches the price  ->  relay closes  ->  motor turns
           ->  pad falls through the chute and breaks the IR beam
           ->  motor stops.  LCD and buzzer report each step.

  If no pad breaks the beam within DISPENSE_TIMEOUT_MS the motor stops anyway
  and the machine latches into a fault. It does NOT reverse and does NOT retry
  by itself — the relay drives one direction only, which is a deliberate design
  decision agreed with the client.

  ---------------------------------------------------------------------------
  THE SERIAL CONSOLE IS THE POINT OF THIS VERSION
  ---------------------------------------------------------------------------
  Open the Serial Monitor at 115200. Everything you need to bring the machine
  up, calibrate it and diagnose it is there. Type ? for the command list.

  Set the Serial Monitor line ending to "Newline" if you can. If you leave it
  at "No line ending" the console still works — it takes a command after a
  short pause in typing.

  Nothing is stored in flash. Anything you change over the console is lost on
  reset, and that is on purpose: the console is for finding the right number,
  then you write that number into CONFIG below and flash it.

  ---------------------------------------------------------------------------
  WIRING
  ---------------------------------------------------------------------------
    12 V adaptor --+-- LM2596 buck -> 5.0 V -> Schottky -> ESP32 VIN
                   |                        +- ESP32 3V3 pin -> KY-032 VCC
                   +-- coin acceptor  (12 V)
                   +-- relay COM/NO   -> motor  (flyback diode across motor)

    GPIO 21  LCD SDA
    GPIO 22  LCD SCL
    GPIO 18  Coin acceptor pulse   (INPUT_PULLUP, acceptor pulls LOW)
    GPIO 26  Relay control         (active HIGH, 10k pull-DOWN to GND)
    GPIO 27  KY-032 IR output
    GPIO 25  Passive buzzer

  ---------------------------------------------------------------------------
  BEFORE FIRST POWER-UP — three things that kill hardware
  ---------------------------------------------------------------------------
    1. Set the LM2596 to 5.0 V with a meter BEFORE connecting the ESP32.
    2. Measure the coin acceptor's pulse output. If it swings to 12 V it must
       go through a divider or optocoupler — it will destroy a GPIO directly.
    3. Fit the 10k pull-down on GPIO 26 so the motor stays still through boot
       and through every reset while you are flashing.

  ---------------------------------------------------------------------------
  MAP OF THIS FILE — what lives where
  ---------------------------------------------------------------------------
  Everything you need to change to commission a machine is in CONFIG, in nine
  numbered sections. Below the END OF CONFIG banner is machinery you should be
  able to read but rarely need to touch.

    CONFIG
      SECTION 1   PIN MAP            which GPIO does what
      SECTION 2   COIN TABLE         pulses -> shillings, per coin
      SECTION 3   PRICE AND CHANGE   what a pad costs, what happens to change
      SECTION 4   POLARITY           IR, relays, stock switch, LCD address
      SECTION 5   TIMING             every delay and timeout, explained
      SECTION 6   SOUND              buzzer tones
      SECTION 7   CONSOLE / FIELD    logging switch, self-test behaviour

    MACHINERY
      STATE               the five states and what each one means
      ISR                 counting coin pulses without missing any
      HARDWARE HELPERS    motor, inhibit relay, buzzer, LCD, sensors
      FAULT / EMPTY       how the machine goes out of service and comes back
      SERIAL REPORTING    the per-coin report
      SELF-TEST           the nine checks run at every boot
      SERIAL CONSOLE      the commands, and what each one is for
      SETUP               boot order, and why it is that order
      LOOP                the main cycle, and why its order matters

  ---------------------------------------------------------------------------
  HOW THIS MACHINE HANDLES MONEY, AND WHY
  ---------------------------------------------------------------------------
  Read this before changing anything that touches `credit`. Every rule below
  exists because of one physical fact:

      THE COIN IS IN THE CASH BOX BEFORE THIS FIRMWARE KNOWS IT EXISTS.

  The acceptor banks a coin and only then sends its pulses. There is no eject,
  no return chute and no hopper anywhere in this machine. Nothing here can give
  anybody their money back — not a fault handler, not a timeout, not a reboot.

  So the firmware cannot protect the cash. What it protects instead is THE
  CLAIM: its own record that it owes somebody a pad. Five rules follow from
  that, and each one is a deliberate answer to "who loses if this goes wrong".

   1. PAY ON DELIVERY, NOT ON MOTOR START.
      Credit comes off the meter at the moment the beam confirms a pad fell,
      and nowhere else. A jam, an empty coil or a missed beam therefore costs
      the customer nothing — the claim is still on the meter and is honoured
      as soon as the machine can serve it. Charging at motor start would have
      meant taking money for a pad that never came out.
      -> ST_DISPENSING, the one line that calls setCredit() on delivery.

   2. A COIN THAT LANDS MID-VEND IS BANKED, NOT DISCARDED.
      Inhibiting the acceptor stops the NEXT coin; one already rolling through
      the mechanism still completes. Those states end in IDLE within a second
      or two, so the machine CAN honour it — it buys the next pad. Throwing it
      away would mean keeping the money and giving nothing back.
      EMPTY and FAULT still refuse, because there the machine genuinely cannot
      deliver and must not take the claim on.

   3. A FULL PAD'S WORTH OF CREDIT IS NEVER TIMED OUT.
      Part payment expires (rule 4). Credit that already covers a pad does not,
      in any state, because that is a debt the machine has already been paid
      for. It waits — through IDLE, through EMPTY, through a refill.

   4. PART PAYMENT EXPIRES, AND THAT IS A POLICY CHOICE, NOT A DEFAULT.
      No setting returns the money, so all CREDIT_TIMEOUT_MS decides is whether
      abandoned part-payment stays with the business or eventually buys a pad
      for whoever walks up next. Two minutes, with a countdown on the screen so
      she knows how long she has. Set it to 0 to wait forever.

   5. THE METER SURVIVES A POWER CUT.
      Without this, a cut with KSh 5 on the meter is a clean swallow: coin in
      the box, no record anywhere. The balance is the only thing this firmware
      writes to flash. The countdown restarts from boot rather than resuming,
      so a short cut protects the customer still standing there and a long one
      does not leave a stranger's balance up all night.

  Two places the money still goes nowhere, stated so nobody thinks otherwise:
  a coin already mid-flight when the machine faults or runs out, and part
  payment that nobody comes back for. Both are logged, never silent.

  ---------------------------------------------------------------------------
  THE FIVE STATES
  ---------------------------------------------------------------------------
    IDLE        Waiting for money. Coins accepted.
    DISPENSING  Motor turning, watching the beam. Coins refused.
    DELIVERED   Pad fell, "TAKE YOUR PAD" on screen. Coins refused.
    EMPTY       Stock switch says no pads. Coins refused. NOT a fault: it
                comes back on its own the moment the coil is refilled.
    FAULT       Something is wrong that needs a person. Coins refused.
                Latched until the clear button is held, or  x  is typed.

  ---------------------------------------------------------------------------
  THE FOUR TIMINGS YOU WILL ACTUALLY TUNE
  ---------------------------------------------------------------------------
    DISPENSE_TIMEOUT_MS   how long to wait for a pad before deciding that
                          nothing dropped.  <- the one you asked about
    COIN_GAP_MS           how long a silence means "that coin is finished"
    CREDIT_TIMEOUT_MS     how long part-payment sits before it is forgotten
    MESSAGE_HOLD_MS       how long "TAKE YOUR PAD" stays on the screen

  The first three can be changed live from the console, so you can find the
  right number without reflashing, then write it into SECTION 5. See the
  detailed notes on each one there.
*/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <esp_system.h>
#include <Preferences.h>

#define FW_VERSION "2.0"

// Declared up here, above everything else, because the Arduino build inserts
// its auto-generated function prototypes ahead of the first function in the
// file — and takeCoin() takes one of these. A struct used in any signature has
// to be visible before that insertion point or the sketch will not compile.
struct CoinBurst
{
  uint16_t pulses;
  uint16_t gapMin; // ms between pulses, smallest seen in this coin
  uint16_t gapMax; // ms between pulses, largest seen in this coin
  uint32_t durMs;  // first pulse to last pulse
};

// ---------------------------------------------------------------------------
//  Logging.  Every Serial.print in this sketch goes through these, so the one
//  switch in SECTION 7 silences the lot for a field install.
//
//  logEvents is the setting. logOn is the momentary gate: handleCommand()
//  forces it true for the duration of a typed command, so the console always
//  answers a human even on a machine installed with logging off, then puts it
//  back. Serial.begin/available/read are never gated — input always works.
// ---------------------------------------------------------------------------
bool logEvents = true; // set from SERIAL_LOG_DEFAULT in setup()
bool logOn = true;     // momentary gate, see above

#define LOG(...) do { if (logOn) Serial.print(__VA_ARGS__); } while (0)
#define LOGLN(...) do { if (logOn) Serial.println(__VA_ARGS__); } while (0)


// ###########################################################################
// #                                                                         #
// #   CONFIG — THIS IS THE PART YOU EDIT.  NOTHING BELOW SECTION 7 NEEDS    #
// #   CHANGING TO COMMISSION A MACHINE.                                     #
// #                                                                         #
// ###########################################################################

// ===========================================================================
//  SECTION 1 — PIN MAP
// ===========================================================================
//
//  These are not arbitrary. On an ESP32 most pins have a second job and only
//  some are safe for general use:
//
//    0, 2, 5, 12, 15   strapping pins. The chip reads them at boot to decide
//                      how to start. Pull one the wrong way and the board will
//                      not boot, or will boot into the wrong mode.
//    1, 3              the USB serial port. Using them kills the console.
//    6 - 11            wired to the flash chip. Touching them bricks the run.
//    34 - 39           INPUT ONLY, and with NO internal pull-up. Fine for an
//                      analogue sensor, useless for a switch to GND — which is
//                      why the stock switch is on 33 and not 34.
//
//  Everything below is outside all of that. If you move a pin, check it
//  against this list first.
//
//  Three of these need an external resistor. See the header block at the top
//  of this file — a floating pin during the boot window is a motor that twitches
//  and a coin slot that is live.
//
const uint8_t PIN_COIN = 18;
const uint8_t PIN_RELAY = 26;
const uint8_t PIN_IR = 27;
const uint8_t PIN_BUZZER = 25;
const uint8_t PIN_SDA = 21;
const uint8_t PIN_SCL = 22;
const uint8_t PIN_INHIBIT = 19; // relay 2 -> HX-916 2-pin inhibit port
const uint8_t PIN_CLEAR = 23;   // jam-clear button to GND (INPUT_PULLUP)
const uint8_t PIN_PADS = 33;    // stock limit switch to GND (INPUT_PULLUP)

// ===========================================================================
//  SECTION 2 — THE COIN TABLE.  WHAT EACH COIN IS WORTH.
// ===========================================================================
//
//  A multi-coin acceptor does not say "that was twenty shillings". It says
//  "that coin was channel 3", and it says it by sending three pulses in quick
//  succession. This table is where you write down what each channel is worth.
//
//  HOW TO FILL THIS IN
//    1. Put the acceptor in learn mode and teach it your coins. Give each coin
//       type a DIFFERENT pulse count. One pulse for the cheapest is the usual
//       convention. Write down which channel you taught which coin to.
//    2. Flash this sketch, open the Serial Monitor, and drop a real coin in.
//       The console prints the pulse count it saw, whether it recognised it,
//       and what it did with it. Unknown coins are reported loudly, never
//       silently swallowed.
//    3. Come back here and make the table agree with what you saw.
//
//  Pulse counts must be UNIQUE. Self-test refuses to run a table with
//  duplicates, because two coins on one channel is money you cannot account
//  for. Order does not matter.
//
struct CoinDef
{
  uint16_t pulses;  // how many pulses the acceptor sends for this coin
  uint16_t value;   // what it is worth, in whole shillings
  const char *name; // what the console calls it. Keep it short.
};

const CoinDef COIN_TABLE[] = {
    //  pulses   value   name
    {2, 5, "KSH 5"},
    {10, 10, "KSH 10"},
    {20, 20, "KSH 20"}, // two 20s taught as one channel, or delete
    {40, 40, "KSH 40"}, // delete any line you have not taught
};
const uint8_t COIN_TABLE_LEN = sizeof(COIN_TABLE) / sizeof(COIN_TABLE[0]);

// ===========================================================================
//  SECTION 3 — PRICE AND CHANGE
// ===========================================================================

// What one pad costs, in whole shillings.
uint16_t PAD_PRICE = 20;

// THE MACHINE GIVES NO CHANGE. It has no hopper and no second motor, so it
// physically cannot. This flag decides what happens to money above the price:
//
//   true  — overpayment stays on the meter. KSh 50 at a KSh 20 price buys two
//           pads back to back and leaves KSh 10 on screen, which the next
//           customer gets or which times out. Fairer, and what a real machine
//           does.
//   false — one pad per transaction, the rest is forfeit. Simpler to watch on
//           the bench. Do not ship it this way.
//
const bool KEEP_CHANGE_AS_CREDIT = true;

// --- SURVIVING A POWER CUT --------------------------------------------------
//
//  true  = the amount on the meter is written to flash whenever it changes, so
//          a power cut does not swallow part payment. Somebody who has put in
//          KSh 5 of KSh 20 when the power goes still has her KSh 5 when it
//          comes back.
//  false = nothing is written to flash at all. A power cut with money on the
//          meter loses it silently.
//
//  WHY THIS IS WORTH THE FLASH WRITES. Without it, a cut at the wrong moment
//  is a clean swallow: the coin is in the box, the firmware forgets it ever
//  arrived, and nobody — not the customer, not the log, not the takings — has
//  any record. That is the one failure this machine should never have, and
//  mains cuts are not hypothetical at a Kenyan site.
//
//  WHAT IT DOES NOT FIX. It restores the CLAIM, not the cash; nothing can
//  return the coin. And if the outage is long, the person who comes back to it
//  is not the person who paid. That is bounded on purpose: the countdown
//  restarts from boot, so a restored balance expires CREDIT_TIMEOUT_MS after
//  the power returns rather than sitting on the screen all night. A short cut
//  protects the customer standing there; a long one does not leave a standing
//  balance for a stranger.
//
//  COST. Three small writes per sale at most, and only when the value actually
//  changes. NVS is wear-levelled and rated in the tens of thousands of cycles
//  per sector, so at fifty sales a day this is years of life, not months. It
//  is not free, and it is the only thing this firmware writes to flash.
const bool REMEMBER_CREDIT_ACROSS_POWER_LOSS = true;

// ===========================================================================
//  SECTION 4 — SENSOR AND RELAY POLARITY
// ===========================================================================

// true  = sensor output goes LOW when it sees the pad  (usual for KY-032)
// false = sensor output goes HIGH when it sees the pad
// If you use the "aim across the chute at the far wall" trick, this inverts.
// Console command  i  shows the live reading so you can settle this in ten
// seconds with your hand.
const bool IR_DETECTED_IS_LOW = true;

// Relay module polarity. This build uses an active-high module.
const bool RELAY_ACTIVE_HIGH = true;

// I2C address of the LCD backpack. Almost always 0x27, sometimes 0x3F.
// Console command  z  scans the bus and tells you.
const uint8_t LCD_I2C_ADDR = 0x27;

// --- COIN ACCEPTOR INHIBIT -------------------------------------------------
// The HX-916's 2-pin port: upper pin floating = accepting, upper pin shorted
// to the lower pin (its GND) = inhibited. Driven by a dry relay contact, never
// by a GPIO — on some units that pin sits at 12 V.
//
// false = the relay module energises when its input is pulled LOW. Most
//         opto-isolated Chinese relay boards are this way round.
// IMPORTANT: with an active-LOW module the pin must be HIGH to keep the relay
// OFF, so GPIO 19 needs a 10k pull-UP to 3V3 — not a pull-down. Get this
// backwards and the acceptor is live for the whole boot window.
const bool INHIBIT_RELAY_ACTIVE_HIGH = false;

// Which contact of the relay you wired across the 2-pin port.
//   true  = NC. Closed with the coil off, so the acceptor is inhibited
//           whenever this board is unpowered, booting, halted or crashed.
//           Fail-safe. Ship it this way.
//   false = NO. A dead controller accepts coins and delivers nothing.
//           Bench use only.
const bool INHIBIT_CONTACT_IS_NC = true;

// --- PAD STOCK SWITCH ------------------------------------------------------
// A mechanical limit switch that tells the machine whether there are pads left
// to dispense, so it can stop taking money BEFORE somebody pays for nothing.
//
// WIRE IT: one side to PIN_PADS, the other to GND. Nothing else. The ESP32's
// internal pull-up does the rest.
//
// CLOSED (pin LOW) = pads present.  OPEN (pin HIGH) = empty.
//
// That direction is deliberate and it is the safe one. A broken wire, a pulled
// connector or a corroded contact all leave the pin floating HIGH, which reads
// EMPTY — so the machine refuses money. Wired the other way round, the same
// faults read FULL and the machine cheerfully takes coins over an empty coil.
// A failure should always cost you a sale, never a customer's money.
const bool PADS_PRESENT_IS_LOW = true;

// Set this true ONLY once the switch is actually wired. While it is false the
// stock check is bypassed entirely and the machine behaves as it did before —
// otherwise an unwired pin floats HIGH, reads EMPTY, and the machine sits
// there refusing to vend while you wonder why.
const bool PAD_SWITCH_FITTED = false;

// --- JAM-CLEAR BUTTON ------------------------------------------------------
// Momentary NO button between PIN_CLEAR and GND. Mount it INSIDE the locked
// door — it is an attendant control, not a customer control.
const uint32_t CLEAR_BTN_DEBOUNCE_MS = 50;
const uint32_t CLEAR_BTN_HOLD_MS = 1000; // must be held, not brushed
const uint32_t FAULT_ROTATE_MS = 2000;   // LCD line 2 rotation

// ===========================================================================
//  SECTION 5 — TIMING
// ===========================================================================
//
//  Every delay and timeout in the machine, in one place.
//
//  Each one is written up the same way:
//
//    WHAT        what the number actually controls
//    TOO SHORT   what goes wrong if you set it too low
//    TOO LONG    what goes wrong if you set it too high
//    HOW TO SET  how to find the right value on the bench
//    CONSOLE     the live command, if it has one
//
//  Three can be changed from the console while the machine is running, so you
//  can hunt for the right number without reflashing every time. Those changes
//  are NOT saved — once you have the number, write it in here and flash it.
//
//  Quick index:
//    COIN_PULSE_DEBOUNCE_MS   noise filter on each individual coin pulse
//    COIN_GAP_MS              silence that means "that coin is finished"   [g]
//    DISPENSE_TIMEOUT_MS      give up waiting for the pad to drop          [o]
//    CREDIT_TIMEOUT_MS        forget part-payment nobody came back for
//    IR_DEBOUNCE_MS           noise filter on the drop beam
//    IR_SETTLE_MS             prove the chute is clear before starting
//    MESSAGE_HOLD_MS          how long "TAKE YOUR PAD" stays up
//    MOTOR_LIMIT_MARGIN_MS    the backstop that can never be reached normally
//    PAD_SWITCH_DEBOUNCE_MS   noise filter on the stock switch
//    IR_STUCK_WARN_MS         nag about something sitting in the chute
//

// ---------------------------------------------------------------------------
//  COIN_PULSE_DEBOUNCE_MS — noise filter on each individual coin pulse
// ---------------------------------------------------------------------------
//  WHAT        The acceptor's output can ring on each edge. Two edges closer
//              together than this are treated as one pulse. This is NOT the
//              gap between coins — that is COIN_GAP_MS below.
//  TOO SHORT   Ringing gets counted as extra pulses. A KSh 20 coin (20 pulses)
//              reads as 23, matches nothing in the table, and the customer's
//              money is taken for a coin the machine cannot price.
//  TOO LONG    Real pulses get swallowed. The same coin reads LOW, maybe 14
//              pulses, which again matches nothing — or worse, matches a
//              CHEAPER coin and short-changes the customer.
//  HOW TO SET  Leave it at 20 unless coin counts come out wrong. Every coin
//              prints a PULSE TIMING line showing the tightest gap it saw; if
//              that number gets close to this one, lower this.
//  CONSOLE     none — needs a reflash
const uint32_t COIN_PULSE_DEBOUNCE_MS = 20;

// ---------------------------------------------------------------------------
//  COIN_GAP_MS — the silence that means "that coin is finished, count it"
// ---------------------------------------------------------------------------
//  WHAT        One coin is a burst of pulses. This is how long the line must
//              stay quiet before the firmware decides the burst has ended and
//              adds up what it counted. A KSh 40 coin is 40 pulses and its
//              burst runs for seconds, so this matters more here than on a
//              machine that only takes one coin type.
//  TOO SHORT   A single coin gets split in two. A KSh 20 (20 pulses) reads as
//              2 + 18: the first half is priced as KSh 5, the second half is
//              unknown, and the customer is short-changed by 15 shillings.
//  TOO LONG    Two coins inserted quickly get merged into one burst. Two KSh 5
//              coins become one 4-pulse "coin" that is in no table, and the
//              money is gone. The acceptor cannot physically deliver two coins
//              closer than about 600 ms, so that is your hard ceiling.
//  HOW TO SET  Drop one of each coin and read the PULSE TIMING line. Take the
//              WIDEST gap you see and roughly double it — that is your floor.
//              Then insert two coins as fast as the acceptor will take them
//              and confirm they still read as two. Anywhere comfortably
//              between those results is right. 250 ms suits most units.
//  CONSOLE     g<ms>   e.g.  g350
uint32_t COIN_GAP_MS = 250;

// ---------------------------------------------------------------------------
//  DISPENSE_TIMEOUT_MS — how long to wait before deciding NOTHING DROPPED
// ---------------------------------------------------------------------------
//  This is the one to get right. It decides whether a perfectly good machine
//  starts throwing faults at people.
//
//  WHAT        The motor starts and the firmware watches the IR beam across
//              the chute. If a pad breaks that beam the motor stops and the
//              screen says TAKE YOUR PAD. If nothing breaks it within this
//              time, the firmware concludes no pad came out. What happens then
//              depends on the stock switch:
//
//                stock switch says EMPTY  ->  goes to EMPTY, which recovers by
//                                             itself the moment you refill
//                stock switch says FULL   ->  it cannot be empty, so it is a
//                                             jam. Latches FAULT "JAMMED"
//                no stock switch fitted   ->  cannot tell, so it stays
//                                             cautious: FAULT "EMPTY OR
//                                             JAMMED"
//
//              Either way the motor stops and the console says plainly that
//              the customer paid for that pad and did not get it.
//  TOO SHORT   The machine gives up while the pad is still on its way out. You
//              get faults on a machine that works perfectly, and every one of
//              them takes a person with a key to clear. This is by far the
//              more likely mistake.
//  TOO LONG    The coil keeps turning after a real jam, which can chew a pad
//              or stall the motor, and the customer stands there waiting.
//  HOW TO SET  With the coil loaded, run the motor in fixed bursts until you
//              have timed ONE full revolution:
//
//                  m500     m1000     m1500     m2000   ...
//
//              The console prints the exact moment the beam broke, so you get
//              the real drop time rather than a guess. Set this to about
//              DOUBLE the slowest drop you saw. Margin here is cheap; a
//              phantom fault in a washroom is not.
//  CONSOLE     o<ms>   e.g.  o3000
uint32_t DISPENSE_TIMEOUT_MS = 3000;

// ---------------------------------------------------------------------------
//  CREDIT_TIMEOUT_MS — how long to keep asking for the rest of the money
// ---------------------------------------------------------------------------
//  WHAT        Somebody puts KSh 10 into a KSh 20 machine. For this long the
//              screen keeps telling her what to add and counts down the time
//              she has left, once a second. If the countdown reaches zero the
//              credit is cleared and the machine goes back to INSERT COIN.
//
//              >>> SET THIS TO 0 TO WAIT FOREVER. <<<
//              Zero means part payment never expires: the screen keeps asking
//              for the rest indefinitely, until somebody tops it up. Nothing
//              else changes.
//
//  THE HONEST  There is no refund path. The coin banks into the housing before
//  TRADE-OFF   this firmware hears a single pulse, so no value here gets the
//              money back to the person who paid it. All this number decides
//              is who ends up with it:
//
//                shorter   the business keeps abandoned part-payment
//                longer    she gets a better chance to find the second coin
//                0         the money always eventually buys a pad — but for
//                          whoever walks up next, not for her
//
//  TOO SHORT   A customer digging in her bag loses the coin she already put
//              in. Twenty seconds was not enough for that; two minutes is.
//  TOO LONG    A standing balance sits on the screen, and the next person
//              tops up a stranger's money and gets a discounted pad. With 0
//              that is permanent, which may be exactly what you want in a
//              school washroom and is certainly not what you want on a
//              machine that has to pay for itself.
//  HOW TO SET  Two minutes is the default because it is long enough to find a
//              coin, ask a friend, or give up honestly. Watch real users.
//  CONSOLE     w<ms>   e.g.  w120000,  or  w0  to wait forever
//              (shorten it to something like w15000 to test the countdown
//              without standing there for two minutes)
uint32_t CREDIT_TIMEOUT_MS = 120000; // 2 minutes. 0 = never expire.

// ---------------------------------------------------------------------------
//  IR_DEBOUNCE_MS — noise filter on the drop beam
// ---------------------------------------------------------------------------
//  WHAT        The beam must read the same for this long before the reading is
//              believed. Stops dust, a flickering sensor or the edge of the
//              coil from registering as a delivered pad.
//  TOO SHORT   Noise counts as a pad. The motor stops early and the customer
//              gets nothing while the screen cheerfully says TAKE YOUR PAD.
//  TOO LONG    A fast-falling pad can clear the beam before the filter accepts
//              it, so a real delivery is missed and you get a phantom jam.
//  HOW TO SET  15 ms is right for a pad falling under gravity. If the console
//              reports a delivery under 150 ms it warns you, because that
//              usually means the sensor is seeing the coil rather than a pad —
//              re-aim the sensor rather than changing this.
//  CONSOLE     none — needs a reflash
const uint32_t IR_DEBOUNCE_MS = 15;

// ---------------------------------------------------------------------------
//  IR_SETTLE_MS — prove the chute is clear BEFORE the motor starts
// ---------------------------------------------------------------------------
//  WHAT        Before every vend the firmware watches the beam for this long
//              and refuses to start if anything is in the way. It will not run
//              the motor blind into a chute that already has something in it.
//              Nothing is charged if this check refuses: the credit stays on
//              the meter, because that is not the customer's fault.
//  TOO SHORT   A brief clear moment lets a vend start over a real blockage.
//  TOO LONG    A visible pause between paying and the motor starting, which
//              makes the machine feel broken.
//  HOW TO SET  150 ms is long enough to be sure and short enough to feel
//              instant. No reason to change it.
//  CONSOLE     none — needs a reflash
const uint32_t IR_SETTLE_MS = 150;

// ---------------------------------------------------------------------------
//  MESSAGE_HOLD_MS — how long "TAKE YOUR PAD" stays on the screen
// ---------------------------------------------------------------------------
//  WHAT        After a successful vend, how long the machine holds that
//              message before going back to INSERT COIN.
//  TOO SHORT   The customer looks up and the message is already gone, so she
//              is not sure anything happened.
//  TOO LONG    The next customer waits at a screen that is not ready for her.
//  HOW TO SET  Purely how it feels. 2.5 s reads comfortably at arm's length.
//  CONSOLE     none — needs a reflash
const uint32_t MESSAGE_HOLD_MS = 2500;

// ---------------------------------------------------------------------------
//  MOTOR_LIMIT_MARGIN_MS — the backstop, on top of DISPENSE_TIMEOUT_MS
// ---------------------------------------------------------------------------
//  WHAT        A hard ceiling on motor run time: DISPENSE_TIMEOUT_MS + this.
//              Checked on EVERY pass of loop(), in EVERY state, before
//              anything else runs, and it does not trust the state machine to
//              have stopped the motor. In normal operation it is unreachable,
//              because the dispense timeout always fires first.
//  TOO SHORT   It could fire during a slow but legitimate vend and report a
//              firmware bug that is not there.
//  TOO LONG    A genuinely runaway motor runs longer before anything catches
//              it.
//  HOW TO SET  Leave it. If this ever fires, the console says so and calls it
//              a firmware bug rather than a jam, because that is what it is.
//              Report it rather than raising this number.
//  CONSOLE     follows  o<ms>  automatically
const uint32_t MOTOR_LIMIT_MARGIN_MS = 2000;

// ---------------------------------------------------------------------------
//  PAD_SWITCH_DEBOUNCE_MS — noise filter on the pad stock switch
// ---------------------------------------------------------------------------
//  WHAT        How long the stock switch must read the same before the machine
//              believes it. A mechanical switch under a stack of pads chatters
//              badly as the stack shifts and settles, so this is debounced far
//              harder than any electronic sensor here.
//  TOO SHORT   The machine flips between in-service and OUT OF STOCK as the
//              stack settles, clicking the inhibit relay every time.
//  TOO LONG    A refill takes a noticeable moment to be noticed. Harmless.
//  HOW TO SET  Type  k  to watch the switch live, then work the lever and load
//              a stack. If the reading dances, raise this. 400 ms is a
//              deliberately generous starting point.
//  CONSOLE     none — needs a reflash
const uint32_t PAD_SWITCH_DEBOUNCE_MS = 400;

// ---------------------------------------------------------------------------
//  IR_STUCK_WARN_MS — nag about something sitting in the chute
// ---------------------------------------------------------------------------
//  WHAT        If the beam reads BLOCKED for this long while the machine sits
//              idle, the console says so once. Warning only: it does not
//              fault, does not stop anything, and does not repeat.
//  TOO SHORT   Console chatter every time a hand passes the chute.
//  TOO LONG    A blockage sits unreported until a customer finds it for you.
//  HOW TO SET  Leave it. It exists to put a heads-up in the log before the
//              next paid vend turns it into a fault.
//  CONSOLE     none — needs a reflash
const uint32_t IR_STUCK_WARN_MS = 5000;

// ===========================================================================
//  SECTION 6 — SOUND
// ===========================================================================
//
//  This machine lives in a public washroom and its customer may well be
//  embarrassed to be standing at it. Sound is there to confirm that something
//  happened, not to announce it to the room. Keep every tone SHORT.
//
//  Each is a frequency in Hz and a duration in ms. The four are deliberately
//  distinguishable with your back turned:
//
//    COIN    high and very short   — "that went in"
//    REJECT  low and flat          — "no, try again" (unknown coin, or the
//                                    machine is busy, faulted or empty)
//    VEND    mid and brief         — "your pad is there"
//    FAULT   low and long          — "something is wrong"
//
//  The buzzer is non-blocking: beep() starts a note and returns immediately,
//  and buzzerService() in loop() stops it when the time is up. A vending
//  machine must never be sitting in delay() while a coin is going in.
//
const uint16_t TONE_COIN_HZ = 2200;
const uint16_t TONE_COIN_MS = 60;
const uint16_t TONE_REJECT_HZ = 600;
const uint16_t TONE_REJECT_MS = 200;
const uint16_t TONE_VEND_HZ = 1800;
const uint16_t TONE_VEND_MS = 150;
const uint16_t TONE_FAULT_HZ = 350;
const uint16_t TONE_FAULT_MS = 500;

// ===========================================================================
//  SECTION 7 — WHAT THE CONSOLE DOES ON ITS OWN
// ===========================================================================

// Click the relay once during the boot self-test to prove it is alive.
// LEAVE THIS false UNTIL THE MOTOR IS DISCONNECTED. It jerks the coil.
const bool SELFTEST_CLICKS_RELAY = false;

// Print a one-line health report every so often. Useful for leaving a soak
// test running. Toggle it live with  h.
bool heartbeatOn = false;
const uint32_t HEARTBEAT_MS = 10000;

// --- FIELD LOGGING SWITCH --------------------------------------------------
// true  = the console narrates every coin, vend, warning and fault. What you
//         want on the bench and during commissioning.
// false = SHIP IT THIS WAY. Routine event chatter is silenced once the machine
//         is installed. Nothing else changes:
//           - the boot banner and the full self-test still print, always, so a
//             reset in the field still tells a service visit what it needs;
//           - every typed console command still answers in full. Plug a laptop
//             in, press s, and you get the whole status whatever this is set to.
//         Toggle it live with  L  if you want the chatter back for a minute.
const bool SERIAL_LOG_DEFAULT = true;

// Refuse to leave setup() if the self-test found something fatal. The machine
// shows the fault on the LCD and waits rather than pretending to work.
const bool HALT_ON_SELFTEST_FAIL = true;

// ###########################################################################
// #                 END OF CONFIG.  Below here is machinery.                #
// ###########################################################################

// ===========================================================================
//  STATE
// ===========================================================================
//
//  The machine is always in exactly one of five states, and the state decides
//  two things: what is on the screen, and whether the coin acceptor is live.
//
//    ST_IDLE        Waiting for money. THE ONLY STATE THAT ACCEPTS COINS.
//                   Leaves here when credit reaches the price, or when the
//                   stock switch says the coil is empty.
//
//    ST_DISPENSING  Motor turning, watching the beam. Coins inhibited. Ends
//                   when a pad breaks the beam (-> DELIVERED) or when
//                   DISPENSE_TIMEOUT_MS runs out (-> EMPTY or FAULT).
//
//    ST_DELIVERED   Pad fell. "TAKE YOUR PAD" holds for MESSAGE_HOLD_MS, then
//                   back to IDLE. Coins stay inhibited through the hold so a
//                   coin cannot arrive against a screen that is not ready.
//
//    ST_EMPTY       Stock switch says no pads. Coins inhibited. NOT a fault
//                   and NOT latched: it returns to IDLE on its own the moment
//                   the switch says pads are back, so a refill is all it takes
//                   and nobody has to drive out with a key.
//
//    ST_FAULT       Something is wrong that needs a person: a jam, a sensor
//                   reading wrong, or a failed self-test. Coins inhibited.
//                   LATCHED — it stays here until the clear button is held for
//                   CLEAR_BTN_HOLD_MS or  x  is typed, and even then it
//                   refuses while the beam still reads blocked.
//
//  The rule the whole design rests on: EVERY state except ST_IDLE inhibits the
//  coin acceptor. The machine would rather refuse a sale than take money for a
//  pad it cannot promise to deliver.
//
enum State
{
  ST_IDLE,
  ST_DISPENSING,
  ST_DELIVERED,
  ST_EMPTY,
  ST_FAULT
};
State state = ST_IDLE;

const char *stateName(State s)
{
  switch (s)
  {
  case ST_IDLE:
    return "IDLE";
  case ST_DISPENSING:
    return "DISPENSING";
  case ST_DELIVERED:
    return "DELIVERED";
  case ST_EMPTY:
    return "EMPTY";
  case ST_FAULT:
    return "FAULT";
  }
  return "?";
}

LiquidCrystal_I2C lcd(LCD_I2C_ADDR, 16, 2);
bool lcdOk = false; // false = run headless, console only

// Set whenever the motor runs; cleared by the next say().
//
//  WHY THIS EXISTS. The LCD is driven over I2C in 4-BIT mode — every byte
//  goes across as two nibbles. If the display misses a single nibble, every
//  byte after it is shifted by four bits, and what comes out is not wrong
//  letters but gibberish: the HD44780 font has Japanese katakana at 0xA0-0xDF,
//  so mangled bytes land there and the screen fills with what look like
//  Chinese characters. It stays that way until the display is re-initialised,
//  because nothing about the LCD knows it lost sync.
//
//  The thing that makes it miss a nibble is the motor. Switching the coil
//  drops the rail for a few milliseconds, the LCD controller browns out while
//  the ESP32 — which has a much better supply — carries on talking to it.
//
//  So: after every motor run, rebuild the display from scratch before writing
//  to it again. It costs about 50 ms once per vend and it makes the symptom
//  self-clear instead of persisting until somebody power-cycles the machine.
//
//  This is a belt-and-braces measure, not a substitute for fixing the supply.
//  See README, "Funny symbols on the screen".
bool lcdNeedsReinit = false;

// Coin pulses are counted in an ISR and decoded into whole coins in loop().
volatile uint16_t isrBurst = 0;
volatile uint32_t isrLastMs = 0;
volatile bool isrSeenOne = false;
volatile uint32_t isrTotal = 0; // every pulse ever, for diagnostics
portMUX_TYPE coinMux = portMUX_INITIALIZER_UNLOCKED;

// Gap statistics for the burst being counted right now. With a 40-pulse coin
// in the table these are the numbers that decide whether COIN_GAP_MS is safe,
// and there is no other way to see them. Reported with every coin.
volatile uint16_t isrGapMin = 0xFFFF;
volatile uint16_t isrGapMax = 0;
volatile uint32_t isrBurstStartMs = 0;

// The coin interrupt is attached only once the self-test has passed. A machine
// that halted at boot must never return to service with this false, or the
// acceptor takes money the firmware cannot count.
bool coinIsrAttached = false;

Preferences prefs; // opened once in setup(), holds only the meter balance

uint16_t credit = 0; // shillings on the meter
uint32_t lastCoinMs = 0;
uint32_t creditTickMs = 0; // next countdown repaint, see ST_IDLE
uint32_t dispenseStartMs = 0;
uint32_t messageUntilMs = 0;

// Debounced IR state
bool irRaw = false;
bool irStable = false;
uint32_t irChangedMs = 0;
uint32_t irBlockedSince = 0;
bool irStuckWarned = false;
bool irWatch = false; // console command  i

// Motor backstop
bool motorRunning = false;
uint32_t motorOnSince = 0;

// Counters. RAM only, and reset on every boot. The meter balance is the
// only thing that survives a power cut — see setCredit().
uint32_t vendCount = 0;
uint32_t coinsAccepted = 0;
uint32_t coinsUnknown = 0;
uint32_t faultCount = 0;
uint32_t shillingsIn = 0;
uint32_t bootMs = 0;

// Loop health
uint32_t loopMaxUs = 0;
uint32_t lastBeatMs = 0;

// Self-test verdict
bool selfTestPassed = false;
char haltReason[24] = "";

// Serial console line buffer
char cmdBuf[32];
uint8_t cmdLen = 0;
uint32_t cmdLastMs = 0;
const uint32_t CMD_IDLE_FLUSH_MS = 80; // makes "No line ending" monitors work

// millis() wraps after ~49.7 days and this machine is meant to run for months,
// so never compare two timestamps directly. Signed difference is wrap-safe.
static inline bool reached(uint32_t now, uint32_t deadline)
{
  return (int32_t)(now - deadline) >= 0;
}

static inline uint32_t motorHardLimitMs()
{
  return DISPENSE_TIMEOUT_MS + MOTOR_LIMIT_MARGIN_MS;
}

// attemptClear() is defined above both of these but has to call them.
bool selfTest(bool verbose);
void attachCoinIsr();

// Debounced pad stock switch
bool padsRaw = true;
bool padsStable = true;
uint32_t padsChangedMs = 0;
bool stockWatch = false; // console command  k
uint32_t emptyRotateMs = 0;
uint8_t emptyMsgIdx = 0;

// added section on purpose of disabling the coin acceptor when jammed
bool coinsAllowed = false; // starts inhibited, on purpose
bool clearBtnRaw = false;
bool clearBtnStable = false;
uint32_t clearBtnChangedMs = 0;
uint32_t clearBtnDownMs = 0;
bool clearBtnHandled = false;
char faultLine2[17] = "";
uint8_t faultMsgIdx = 0;
uint32_t faultRotateMs = 0;

// ===========================================================================
//  ISR — counting coin pulses without missing any
// ===========================================================================
//
//  HOW A MULTI-COIN ACCEPTOR TALKS
//  It does not say "that was twenty shillings". It pulls one line LOW a fixed
//  number of times, quickly, and the NUMBER OF PULSES identifies which coin it
//  was. On this machine: 2 pulses = KSh 5, 10 = KSh 10, 20 = KSh 20,
//  40 = KSh 40. See SECTION 2.
//
//  So reading a coin is two jobs, and they are split in two on purpose:
//
//    onCoinPulse()  runs in an interrupt, on the falling edge of every pulse.
//                   It does the absolute minimum — count, timestamp, return —
//                   because an interrupt that dawdles loses the next pulse.
//                   It also debounces, so electrical ringing on one edge is
//                   not counted twice (COIN_PULSE_DEBOUNCE_MS).
//
//    takeCoin()     runs in loop(). It watches for the line to go quiet for
//                   COIN_GAP_MS, which is what "that coin is finished" looks
//                   like, and only then hands the finished count over to be
//                   priced.
//
//  It also measures the gaps between pulses as it goes, because with a
//  40-pulse coin those numbers are the only way to know whether COIN_GAP_MS
//  has enough margin on both sides. They print with every coin.
//
void IRAM_ATTR onCoinPulse()
{
  uint32_t now = millis();
  portENTER_CRITICAL_ISR(&coinMux);
  // isrLastMs starts at 0, so a plain debounce guard would swallow a coin
  // dropped in the first COIN_PULSE_DEBOUNCE_MS after boot. isrSeenOne makes
  // the first pulse unconditional and debounces every one after it.
  if (!isrSeenOne || (now - isrLastMs) >= COIN_PULSE_DEBOUNCE_MS)
  {
    if (isrBurst == 0)
    {
      isrBurstStartMs = now;
      isrGapMin = 0xFFFF;
      isrGapMax = 0;
    }
    else
    {
      uint32_t g = now - isrLastMs;
      if (g > 0xFFFF)
        g = 0xFFFF;
      if (g < isrGapMin)
        isrGapMin = (uint16_t)g;
      if (g > isrGapMax)
        isrGapMax = (uint16_t)g;
    }
    isrSeenOne = true;
    // Saturate rather than wrap. A noisy or oscillating coin line could
    // otherwise count past 65535 and come back round onto a number that
    // matches a real coin in the table — free pads, from electrical noise.
    // Stopped here, a runaway burst stays an absurd number, matches nothing,
    // and is refused.
    if (isrBurst < 1000)
      isrBurst++;
    isrTotal++;
    isrLastMs = now;
  }
  portEXIT_CRITICAL_ISR(&coinMux);
}

// A coin is "finished" once the acceptor has been quiet for COIN_GAP_MS.
// Returns true and hands back the pulse count exactly once per coin.
bool takeCoin(CoinBurst &b)
{
  bool ready = false;
  portENTER_CRITICAL(&coinMux);
  if (isrBurst > 0 && (millis() - isrLastMs) >= COIN_GAP_MS)
  {
    b.pulses = isrBurst;
    b.gapMin = (isrGapMin == 0xFFFF) ? 0 : isrGapMin;
    b.gapMax = isrGapMax;
    b.durMs = isrLastMs - isrBurstStartMs;
    isrBurst = 0;
    ready = true;
  }
  portEXIT_CRITICAL(&coinMux);
  return ready;
}

// Ceiling on the meter, as a hard stop rather than an assumption.
//
//  In normal operation credit never climbs anywhere near this: the machine
//  vends the instant credit reaches the price, so it cannot exceed PAD_PRICE
//  plus one coin before dropping again. Coins are refused outright in EMPTY
//  and FAULT, so it cannot pile up there either.
//
//  It exists because "cannot happen" is a bad thing to rely on in the one
//  variable that represents somebody's money. If a future state ever holds
//  credit without spending it, this stops a 16-bit counter wrapping and
//  handing the next customer a fortune. If it ever trips, that is a firmware
//  bug and the console says so.
const uint16_t MAX_CREDIT = 10000;

// THE ONLY PLACE THE METER IS EVER WRITTEN.
//
//  Everything that moves money goes through here so the stored copy can never
//  drift from the live one. There is no way to change credit somewhere else
//  and forget to persist it, because nothing else assigns to it.
//
//  putUShort() compares before it writes, so setting the same value again
//  costs nothing and does not touch the flash.
void setCredit(uint16_t value)
{
  credit = value;
  if (REMEMBER_CREDIT_ACROSS_POWER_LOSS)
    prefs.putUShort("credit", value);
}

// The only place money is added to the meter. Returns false if it refused.
bool bankCoin(const CoinDef *coin)
{
  if ((uint32_t)credit + coin->value > MAX_CREDIT)
  {
    LOGLN(F("!! CREDIT CEILING HIT. Coin NOT banked."));
    LOGLN(F("!! Credit is not being spent when it should be. That is a"));
    LOGLN(F("!! firmware bug, not a coin problem. Report it."));
    return false;
  }
  setCredit(credit + coin->value);
  shillingsIn += coin->value;
  coinsAccepted++;
  lastCoinMs = millis();
  return true;
}

const CoinDef *lookupCoin(uint16_t pulses)
{
  for (uint8_t i = 0; i < COIN_TABLE_LEN; i++)
  {
    if (COIN_TABLE[i].pulses == pulses)
      return &COIN_TABLE[i];
  }
  return nullptr;
}

// The only place the coin interrupt is ever attached. Everything that puts the
// machine into service goes through here, so there is one thing to check when
// asking "can this machine actually count a coin right now?".
void attachCoinIsr()
{
  if (coinIsrAttached)
    return;
  attachInterrupt(digitalPinToInterrupt(PIN_COIN), onCoinPulse, FALLING);
  coinIsrAttached = true;
  LOGLN(F("Coin interrupt attached."));
}

// ===========================================================================
//  HARDWARE HELPERS
// ===========================================================================
void motorOn()
{
  digitalWrite(PIN_RELAY, RELAY_ACTIVE_HIGH ? HIGH : LOW);
  motorOnSince = millis();
  motorRunning = true;
  lcdNeedsReinit = true; // the coil kick is what corrupts the display
}

void motorOff()
{
  digitalWrite(PIN_RELAY, RELAY_ACTIVE_HIGH ? LOW : HIGH);
  motorRunning = false;
}

// Turn the coin slot on or off, through the second relay.
//
//  Called from exactly two places: once in setup() before any state exists,
//  and once per pass of loop() from the single rule that derives the slot from
//  the machine's state. Nothing else touches it, so the slot can never
//  disagree with what the machine is doing.
//
//  Writing the same level repeatedly costs nothing and does not chatter the
//  relay — the pin simply stays where it is. Only a real change is logged.
//
//  There are two independent polarities to get right here, which is why this
//  is a function and not a digitalWrite:
//
//    INHIBIT_CONTACT_IS_NC       which relay contact you wired to the acceptor
//    INHIBIT_RELAY_ACTIVE_HIGH   which way the relay module's input works
//
//  With the shipped settings (NC contact, active-LOW module) it works out as:
//
//      want        contact      relay      pin
//      ----------  -----------  ---------  ------
//      inhibit     closed       off        HIGH
//      accept      open         on         LOW
//
//  The NC contact is the important half. Closed means inhibited, and a relay
//  with no power sits closed — so an unpowered, booting, halted or crashed
//  controller refuses money instead of eating it. Change INHIBIT_CONTACT_IS_NC
//  only if you also move the wire.
//
//  WHAT THIS CANNOT DO. Inhibit stops the acceptor taking the NEXT coin. It
//  cannot give back one already taken. The acceptor banks a coin into the cash
//  box and only then sends its pulses, so by the time this firmware knows any
//  money arrived, that money is physically in the housing with no path back
//  out. There is no eject, no return chute and no hopper anywhere in this
//  machine.
//
//  That single fact shapes the whole money design. The firmware cannot refund,
//  so it must instead never lose track of what it owes: credit is taken off
//  the meter only when a pad is confirmed delivered, and a full pad's worth of
//  credit is never quietly timed out. See ST_DISPENSING and ST_EMPTY.
void coinAcceptorAllow(bool allow)
{
  bool contactClosed = !allow; // closed == inhibited
  bool energised = INHIBIT_CONTACT_IS_NC ? !contactClosed : contactClosed;
  digitalWrite(PIN_INHIBIT, (energised == INHIBIT_RELAY_ACTIVE_HIGH) ? HIGH : LOW);
  if (allow != coinsAllowed)
  {
    coinsAllowed = allow;
    LOG(F("Coin acceptor "));
    LOGLN(allow ? F("ENABLED") : F("INHIBITED — coins will be returned"));
  }
}

void armClearButtonForNewFault()
{
  // Snapshot the clear button's raw level right now and mark it already
  // handled. A hold already in progress — or a line stuck/shorted low —
  // can't ride straight into clearing THIS fault. It has to be seen
  // released, then pressed and held fresh, same as a human would do it.
  clearBtnRaw = clearBtnStable = (digitalRead(PIN_CLEAR) == LOW);
  clearBtnHandled = true;
}

// ---------------------------------------------------------------------------
//  Buzzer.  tone() only exists from ESP32 core 3.0 onward; before that the
//  buzzer has to be driven through the LEDC peripheral. This works on both, so
//  the sketch compiles whichever core is installed.
//
//  It is also non-blocking on both: beep() starts the note and returns, and
//  buzzerService() silences it when the time is up. A vending machine must not
//  sit in delay() while a coin is going in.
// ---------------------------------------------------------------------------
#ifndef ESP_ARDUINO_VERSION_MAJOR
#define ESP_ARDUINO_VERSION_MAJOR 2
#endif

const uint8_t BUZZER_LEDC_CH = 0;
uint32_t buzzUntilMs = 0;
bool buzzing = false;

void buzzerInit()
{
#if ESP_ARDUINO_VERSION_MAJOR < 3
  ledcSetup(BUZZER_LEDC_CH, 2000, 8);
  ledcAttachPin(PIN_BUZZER, BUZZER_LEDC_CH);
  ledcWriteTone(BUZZER_LEDC_CH, 0);
#else
  pinMode(PIN_BUZZER, OUTPUT);
#endif
}

void beep(uint16_t hz, uint16_t ms)
{
#if ESP_ARDUINO_VERSION_MAJOR < 3
  ledcWriteTone(BUZZER_LEDC_CH, hz);
#else
  tone(PIN_BUZZER, hz);
#endif
  buzzUntilMs = millis() + ms;
  buzzing = true;
}

void buzzerService()
{
  if (!buzzing || !reached(millis(), buzzUntilMs))
    return;
  buzzing = false;
#if ESP_ARDUINO_VERSION_MAJOR < 3
  ledcWriteTone(BUZZER_LEDC_CH, 0);
#else
  noTone(PIN_BUZZER);
#endif
}

// A delay that still lets the buzzer stop on time. Use this, never delay(),
// anywhere a note might still be sounding.
void holdFor(uint32_t ms)
{
  uint32_t t0 = millis();
  while (millis() - t0 < ms)
  {
    buzzerService();
    delay(1);
  }
  buzzerService();
}

bool i2cPresent(uint8_t addr)
{
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

// Every message must fit 16 characters. Silently does nothing if there is no
// screen, so a dead LCD degrades the machine to console-only instead of
// taking the whole thing down.
void say(const char *l1, const char *l2 = "")
{
  if (!lcdOk)
    return;
  if (lcdNeedsReinit)
  {
    lcdNeedsReinit = false;
    lcd.init(); // re-sync the 4-bit interface after a motor-induced dip
    lcd.backlight();
  }
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(l1);
  lcd.setCursor(0, 1);
  lcd.print(l2);
}

// Rewrite line 2 only, without clearing the screen first.
//
//  say() calls lcd.clear(), which blanks the whole display for a moment. Doing
//  that once a second for a countdown makes the screen visibly flicker and
//  look broken. This pads to a full 16 characters and writes over the old line
//  in one go instead, so the display never blanks.
void sayLine2(const char *l2)
{
  if (!lcdOk)
    return;
  char buf[17];
  snprintf(buf, sizeof(buf), "%-16s", l2);
  lcd.setCursor(0, 1);
  lcd.print(buf);
}

// Build the "what to add, and how long you have" line for part payment.
//
//  With a timeout:     ADD 10 IN 1:35
//  Waiting forever:    ADD KSH 10 MORE
void creditPromptLine(char *buf, size_t n, uint32_t now)
{
  uint16_t owed = (PAD_PRICE > credit) ? (PAD_PRICE - credit) : 0;

  if (CREDIT_TIMEOUT_MS == 0)
  {
    snprintf(buf, n, "ADD KSH %u MORE", owed);
    return;
  }

  uint32_t elapsed = now - lastCoinMs;
  uint32_t leftMs = (elapsed >= CREDIT_TIMEOUT_MS) ? 0 : (CREDIT_TIMEOUT_MS - elapsed);
  uint32_t secs = (leftMs + 999) / 1000; // round up, so it never shows 0:00 early
  snprintf(buf, n, "ADD %u IN %lu:%02lu",
           owed, (unsigned long)(secs / 60), (unsigned long)(secs % 60));
}

// THE DROP SENSOR — the machine's only proof that a pad actually came out.
//
//  Turning the coil is not the same as delivering a pad, so the firmware never
//  assumes. An IR beam crosses the chute; a falling pad breaks it. That break
//  is what stops the motor and what counts as a sale.
//
//  This returns true only once the reading has held steady for IR_DEBOUNCE_MS,
//  so dust, a flickering sensor or the edge of the coil passing cannot be
//  mistaken for a pad. Call it often — it does its own timing internally and
//  only updates when something has actually settled.
//
//  It is called in three quite different jobs:
//    - before a vend, to prove the chute is EMPTY (startVend)
//    - during a vend, to catch the pad falling (ST_DISPENSING)
//    - before clearing a fault, to prove the jam is really gone (attemptClear)
bool padSeen()
{
  bool level = (digitalRead(PIN_IR) == LOW);
  bool seen = IR_DETECTED_IS_LOW ? level : !level;
  uint32_t now = millis();

  if (seen != irRaw)
  {
    irRaw = seen;
    irChangedMs = now;
  }
  else if (irRaw != irStable && (now - irChangedMs) >= IR_DEBOUNCE_MS)
  {
    irStable = irRaw;
    if (irStable)
    {
      irBlockedSince = now;
      irStuckWarned = false;
    }
  }
  return irStable;
}

// True when there are pads left to dispense. Bypassed entirely, and always
// true, until PAD_SWITCH_FITTED says the switch actually exists.
bool padsPresent()
{
  if (!PAD_SWITCH_FITTED)
    return true;

  bool level = (digitalRead(PIN_PADS) == LOW);
  bool present = PADS_PRESENT_IS_LOW ? level : !level;
  uint32_t now = millis();

  if (present != padsRaw)
  {
    padsRaw = present;
    padsChangedMs = now;
  }
  else if (padsRaw != padsStable && (now - padsChangedMs) >= PAD_SWITCH_DEBOUNCE_MS)
  {
    padsStable = padsRaw;
  }
  return padsStable;
}

void rule()
{
  LOGLN(F("-----------------------------------------------------"));
}

// ===========================================================================
//  GOING OUT OF SERVICE, AND COMING BACK
// ===========================================================================
//
//  Two different ways for the machine to stop selling, and they are kept
//  deliberately distinct because they need opposite handling:
//
//    enterFault()   Something is wrong that a person has to look at: a jam, a
//                   sensor reading wrong, a failed self-test. LATCHES. Coins
//                   inhibited. Screen says OUT OF ORDER. Only attemptClear()
//                   gets you out, and it refuses while the chute is still
//                   blocked.
//
//    enterEmpty()   The machine simply ran out. Routine, not broken. DOES NOT
//                   LATCH. Coins inhibited, screen says SORRY - NO PADS, and
//                   loop() puts it back in service by itself the moment the
//                   stock switch says pads are loaded.
//
//  Getting this split wrong is expensive in the real world: latching on
//  "empty" would leave a site dead until somebody drove there, when all it
//  needed was the refill that was already scheduled.
//
void enterFault(const char *line2)
{
  motorOff();
  armClearButtonForNewFault();
  faultCount++;
  strncpy(faultLine2, line2, sizeof(faultLine2) - 1);
  faultLine2[sizeof(faultLine2) - 1] = 0;
  faultMsgIdx = 0;
  faultRotateMs = 0; // force an immediate repaint
  beep(TONE_FAULT_HZ, TONE_FAULT_MS);
  state = ST_FAULT;
  LOGLN();
  LOG(F("*** FAULT: "));
  LOGLN(line2);
  LOGLN(F("*** Coins inhibited. Clear the chute, then hold the"));
  LOGLN(F("*** front-panel button for 1 s, or type  x."));
}

// The idle screen, in its two forms. One function owns both so they cannot
// drift apart.
//
//  Nothing on the meter:       PAD = KSH 20
//                              INSERT COIN
//
//  Part payment outstanding:   CREDIT KSH 5
//                              ADD 15 IN 1:55
//
//  The second form matters more than it looks. Leftover change is reached by
//  two quite different routes — she paid part of the price, or she overpaid
//  and a vend handed the remainder back onto the meter — and both must land on
//  the SAME screen. A customer with KSh 5 showing needs to be told to add 15
//  and how long she has, and it cannot depend on which way she got there.
//
//  Before this, the change-after-a-vend route showed "PAD = KSH 20 / CREDIT
//  KSH 5" for a second until the countdown tick overwrote line 2, leaving a
//  half-updated screen that never said what to do.
void showIdle()
{
  char l1[17], l2[17];
  if (credit > 0 && credit < PAD_PRICE)
  {
    snprintf(l1, sizeof(l1), "CREDIT KSH %u", credit);
    creditPromptLine(l2, sizeof(l2), millis());
    creditTickMs = millis() + 1000; // next countdown repaint is a second away
  }
  else
  {
    snprintf(l1, sizeof(l1), "PAD = KSH %u", PAD_PRICE);
    snprintf(l2, sizeof(l2), "INSERT COIN");
  }
  say(l1, l2);
}

// Out of stock is NOT a fault. It is a normal condition with an obvious cure,
// so it does not latch: the moment the switch says pads are back, the machine
// returns to service on its own. Nobody drives out to reset it after a refill.
void enterEmpty()
{
  motorOff();
  state = ST_EMPTY;
  emptyRotateMs = 0; // force an immediate repaint
  emptyMsgIdx = 0;
  beep(TONE_REJECT_HZ, TONE_REJECT_MS);
  LOGLN();
  LOGLN(F("*** OUT OF STOCK. The stock switch says the coil is empty."));
  LOGLN(F("*** Coins inhibited. Refill and it comes back by itself."));
  if (credit > 0)
  {
    LOG(F("*** KSH "));
    LOG(credit);
    LOGLN(F(" is still on the meter."));
    if (credit >= PAD_PRICE)
      LOGLN(F("*** A full pad, already paid for. It waits here for a refill."));
    else
      LOGLN(F("*** That is part payment. It will time out like any other."));
  }
}

void attemptClear(const char *who)
{
  if (state != ST_FAULT)
  {
    LOG(F("Clear requested by "));
    LOG(who);
    LOGLN(F(" but the machine is not faulted."));
    return;
  }

  // Let the beam settle before believing it. Clearing a fault while a pad is
  // still wedged just faults again on the next coin, after taking the money.
  uint32_t t0 = millis();
  while (millis() - t0 < IR_SETTLE_MS)
  {
    padSeen();
    buzzerService();
    delay(1);
  }

  if (irStable)
  {
    say("CLEAR THE CHUTE", "THEN HOLD AGAIN");
    beep(TONE_REJECT_HZ, TONE_REJECT_MS);
    LOG(F("Refusing to clear ("));
    LOG(who);
    LOGLN(F("): the beam still reads BLOCKED."));
    faultRotateMs = millis() + MESSAGE_HOLD_MS; // let the message stand
    return;
  }

  // If the machine halted during setup, the coin interrupt was never attached.
  // Going back into service now would put a LIVE acceptor in front of firmware
  // that cannot count a single pulse: it would take money and deliver nothing,
  // silently, for as long as the coin box holds out. Re-run the self-test and
  // only return to service if it now passes.
  if (!coinIsrAttached)
  {
    LOGLN(F("Halted at boot — re-running the self-test before"));
    LOGLN(F("returning this machine to service."));
    selfTestPassed = selfTest(true);
    if (!selfTestPassed)
    {
      say("STILL FAULTY", haltReason[0] ? haltReason : "SEE SERIAL");
      beep(TONE_REJECT_HZ, TONE_REJECT_MS);
      LOG(F("Refusing to clear ("));
      LOG(who);
      LOGLN(F("): the self-test still fails."));
      LOGLN(F("Coins stay inhibited. Fix it, then hold again."));
      armClearButtonForNewFault();
      faultRotateMs = millis() + MESSAGE_HOLD_MS;
      return;
    }
    attachCoinIsr();
  }

  motorOff();
  LOG(F("Fault cleared by "));
  LOGLN(who);
  if (credit > 0)
  {
    // Worth stating out loud. By the time somebody unlocks the door the
    // customer is long gone, so carrying this forward would just hand the next
    // person a free pad. It is cleared on purpose, not lost by accident.
    LOG(F("Clearing KSH "));
    LOG(credit);
    LOGLN(F(" of credit that was on the meter."));
  }
  setCredit(0);

  // Clearing a jam on an empty coil must not flick the machine into service
  // for the one pass it takes the stock check to notice — that is an audible
  // double-click of the inhibit relay and a moment when the slot is live.
  // Go straight to OUT OF STOCK and say so.
  if (!padsPresent())
  {
    LOGLN(F("The coil is empty though, so it stays out of service until"));
    LOGLN(F("someone refills it. Coins stay inhibited."));
    enterEmpty();
    return;
  }

  state = ST_IDLE;
  beep(TONE_VEND_HZ, TONE_VEND_MS);
  showIdle();
}

// THE JAM-CLEAR BUTTON — an attendant control, mounted INSIDE the locked door.
//
//  Held for CLEAR_BTN_HOLD_MS, it puts a faulted machine back in service. It
//  is a hold rather than a press so that a brush past it in a dark cabinet
//  cannot return a broken machine to service.
//
//  Two things guard it, both deliberate:
//
//    A hold already in progress cannot clear a fault that arrives DURING that
//    hold. enterFault() calls armClearButtonForNewFault(), which marks the
//    current press as already dealt with. The button must be seen released and
//    then pressed again — the way a person would actually do it. Without this,
//    a shorted or jammed-down button would clear every fault the instant it
//    happened, forever, and the machine would loop through jam after jam
//    taking money the whole way.
//
//    attemptClear() re-checks the beam before it agrees. Clearing a fault with
//    a pad still wedged in the chute only faults again on the next coin —
//    after taking that customer's money.
void clearButtonService()
{
  uint32_t now = millis();
  bool down = (digitalRead(PIN_CLEAR) == LOW);

  if (down != clearBtnRaw)
  {
    clearBtnRaw = down;
    clearBtnChangedMs = now;
  }
  else if (clearBtnRaw != clearBtnStable &&
           (now - clearBtnChangedMs) >= CLEAR_BTN_DEBOUNCE_MS)
  {
    clearBtnStable = clearBtnRaw;
    if (clearBtnStable)
    {
      clearBtnDownMs = now;
      clearBtnHandled = false;
    }
  }

  if (clearBtnStable && !clearBtnHandled &&
      (now - clearBtnDownMs) >= CLEAR_BTN_HOLD_MS)
  {
    clearBtnHandled = true;
    attemptClear("front panel button");
  }
}

// ===========================================================================
//  SERIAL REPORTING — "what went in, what it means, what comes out"
// ===========================================================================
void reportCoin(uint16_t pulses, const CoinDef *coin, const CoinBurst *b = nullptr)
{
  rule();
  LOG(F("COIN IN      : "));
  LOG(pulses);
  LOGLN(pulses == 1 ? F(" pulse") : F(" pulses"));

  // With a 40-pulse coin in the table, these two numbers are what decide
  // whether COIN_GAP_MS is safe, and there is no other way to see them.
  if (b && b->pulses > 1)
  {
    LOG(F("PULSE TIMING : "));
    LOG(b->durMs);
    LOG(F(" ms burst, gaps "));
    LOG(b->gapMin);
    LOG(F("-"));
    LOG(b->gapMax);
    LOGLN(F(" ms"));

    if ((uint32_t)b->gapMax * 2 > COIN_GAP_MS)
    {
      LOG(F("  !! Widest gap is over half of COIN_GAP_MS ("));
      LOG(COIN_GAP_MS);
      LOGLN(F(" ms)."));
      LOGLN(F("  !! This coin is close to being read as two, which"));
      LOGLN(F("  !! would short-change the customer. Raise it with"));
      LOGLN(F("  !! g<ms> — but stay under 600 ms or two coins in a"));
      LOGLN(F("  !! row will merge into one unknown coin."));
    }
    if (b->gapMin < COIN_PULSE_DEBOUNCE_MS * 2)
    {
      LOGLN(F("  !! Tightest gap is near the debounce floor. Pulses"));
      LOGLN(F("  !! may be being merged. If counts read LOW, lower"));
      LOGLN(F("  !! COIN_PULSE_DEBOUNCE_MS in SECTION 5."));
    }
  }

  if (!coin)
  {
    LOGLN(F("RECOGNISED   : NO — this pulse count is not in the table"));
    LOGLN(F("MEANING      : the acceptor took a coin this firmware"));
    LOGLN(F("               cannot price. The customer has paid and"));
    LOGLN(F("               is owed nothing until you fix SECTION 2."));
    LOG(F("WILL VEND    : nothing. Add  { "));
    LOG(pulses);
    LOGLN(F(", <value>, \"<name>\" }  to COIN_TABLE."));
    rule();
    return;
  }

  LOG(F("RECOGNISED   : "));
  LOG(coin->name);
  LOG(F("  (worth KSH "));
  LOG(coin->value);
  LOGLN(F(")"));

  LOG(F("CREDIT NOW   : KSH "));
  LOG(credit);
  LOG(F(" of KSH "));
  LOGLN(PAD_PRICE);

  if (state == ST_DISPENSING || state == ST_DELIVERED)
  {
    LOG(F("MEANING      : machine is "));
    LOG(stateName(state));
    LOGLN(F(" — banked toward the next pad"));
    LOGLN(F("WILL VEND    : another pad as soon as this one finishes."));
  }
  else if (state != ST_IDLE)
  {
    LOG(F("MEANING      : machine is "));
    LOG(stateName(state));
    LOGLN(F(" — coin logged, NOT banked"));
    LOGLN(F("WILL VEND    : nothing. It never owes a pad it cannot"));
    LOGLN(F("               deliver. This coin is not refundable."));
  }
  else if (credit < PAD_PRICE)
  {
    LOG(F("MEANING      : short by KSH "));
    LOGLN(PAD_PRICE - credit);
    LOGLN(F("WILL VEND    : nothing yet — waiting for more"));
  }
  else
  {
    uint16_t pads = KEEP_CHANGE_AS_CREDIT ? (credit / PAD_PRICE) : 1;
    uint16_t left = KEEP_CHANGE_AS_CREDIT ? (credit % PAD_PRICE) : 0;
    LOGLN(F("MEANING      : price met"));
    LOG(F("WILL VEND    : "));
    LOG(pads);
    LOGLN(pads == 1 ? F(" pad") : F(" pads, one after the other"));
    LOG(F("LEFT OVER    : KSH "));
    LOG(left);
    if (left > 0)
      LOGLN(F("  (stays on the meter, no change given)"));
    else
      LOGLN();
  }
  rule();
}

// ===========================================================================
//  SELF-TEST — nine checks, at every boot and on demand with  d
// ===========================================================================
//
//  The job here is to catch, before a single coin is counted, anything that
//  would make the machine take money and do the wrong thing with it.
//
//    [1] POWER / RESET     why we rebooted. Names BROWNOUT (the motor is
//                          dragging the rail down) and PANIC (firmware crash)
//                          specifically, because those two are what actually
//                          happen to this machine in the field.
//    [2] COIN TABLE        FATAL on an empty table, a 0-pulse entry, or two
//                          coins sharing a pulse count — that last one is
//                          money you could never account for.
//    [3] PULSE DECODING    FATAL if COIN_GAP_MS is too close to the debounce,
//                          which would split one coin into several.
//    [4] SCREEN            scans the whole I2C bus and prints what answered,
//                          so a blank LCD is never a guessing game. Missing
//                          screen is not fatal: it runs headless.
//    [5] IR DROP SENSOR    FATAL if the chute reads blocked at rest.
//    [6] RELAY / MOTOR     pin, polarity, and the two timeouts.
//    [7] INHIBIT / BUTTON  contact type, and the pull-up warning that goes
//                          with an active-LOW relay module.
//    [8] PAD STOCK SWITCH  reading, polarity, or a reminder that it is
//                          bypassed because PAD_SWITCH_FITTED is false.
//    [9] BUZZER            one chirp. Silence means check the wiring.
//
//  A FATAL result stops setup() before attachCoinIsr(), so the coin interrupt
//  is never attached and the acceptor stays inhibited. The console stays alive
//  so you can find out why. HALT_ON_SELFTEST_FAIL in SECTION 7 turns that off.
//
const char *resetReasonName()
{
  switch (esp_reset_reason())
  {
  case ESP_RST_POWERON:
    return "power-on";
  case ESP_RST_EXT:
    return "external reset pin";
  case ESP_RST_SW:
    return "software restart";
  case ESP_RST_PANIC:
    return "PANIC / exception";
  case ESP_RST_INT_WDT:
    return "interrupt watchdog";
  case ESP_RST_TASK_WDT:
    return "task watchdog";
  case ESP_RST_WDT:
    return "other watchdog";
  case ESP_RST_BROWNOUT:
    return "BROWNOUT — the 12 V rail sagged";
  case ESP_RST_DEEPSLEEP:
    return "deep sleep wake";
  default:
    return "unknown";
  }
}

void i2cScan()
{
  LOGLN(F("  I2C bus scan:"));
  uint8_t found = 0;
  for (uint8_t a = 1; a < 127; a++)
  {
    if (i2cPresent(a))
    {
      LOG(F("    device at 0x"));
      if (a < 16)
        LOG('0');
      LOG(a, HEX);
      if (a == LCD_I2C_ADDR)
        LOG(F("   <- LCD_I2C_ADDR"));
      else if (a == 0x27 || a == 0x3F)
        LOG(F("   <- looks like an LCD backpack"));
      LOGLN();
      found++;
    }
  }
  if (!found)
    LOGLN(F("    nothing responded. Check SDA/SCL and the LCD's 5 V."));
}

// Returns false if something fatal was found.
bool selfTest(bool verbose)
{
  bool ok = true;
  haltReason[0] = 0;

  LOGLN();
  rule();
  LOGLN(F("SELF-TEST"));
  rule();

  // --- 1. Power, and why we are here ---
  LOGLN(F("[1] POWER / RESET"));
  LOG(F("  last reset  : "));
  LOGLN(resetReasonName());
  if (esp_reset_reason() == ESP_RST_BROWNOUT)
  {
    LOGLN(F("  !! A brownout means the motor is dragging the rail down."));
    LOGLN(F("  !! That is what the 1000 uF across 12 V is for. Fit it."));
  }
  if (esp_reset_reason() == ESP_RST_PANIC || esp_reset_reason() == ESP_RST_TASK_WDT)
  {
    LOGLN(F("  !! The firmware crashed last run. Say so when reporting it."));
  }
  LOG(F("  free heap   : "));
  LOG(ESP.getFreeHeap());
  LOGLN(F(" bytes"));
  LOG(F("  CPU         : "));
  LOG(ESP.getCpuFreqMHz());
  LOGLN(F(" MHz"));

  // --- 2. Coin table sanity ---
  LOGLN(F("[2] COIN TABLE"));
  if (COIN_TABLE_LEN == 0)
  {
    LOGLN(F("  FAIL: table is empty. Nothing can ever be paid for."));
    strncpy(haltReason, "COIN TABLE EMPTY", sizeof(haltReason) - 1);
    ok = false;
  }
  for (uint8_t i = 0; i < COIN_TABLE_LEN; i++)
  {
    if (verbose)
    {
      LOG(F("  "));
      LOG(COIN_TABLE[i].pulses);
      LOG(COIN_TABLE[i].pulses == 1 ? F(" pulse  = ") : F(" pulses = "));
      LOG(COIN_TABLE[i].name);
      LOG(F("  (KSH "));
      LOG(COIN_TABLE[i].value);
      LOG(F(")"));
      if (COIN_TABLE[i].value == PAD_PRICE)
        LOG(F("   <- exact price"));
      LOGLN();
    }
    if (COIN_TABLE[i].pulses == 0)
    {
      LOGLN(F("  FAIL: a coin with 0 pulses can never be detected."));
      strncpy(haltReason, "BAD COIN TABLE", sizeof(haltReason) - 1);
      ok = false;
    }
    if (COIN_TABLE[i].value == 0)
    {
      LOG(F("  WARN: "));
      LOG(COIN_TABLE[i].name);
      LOGLN(F(" is worth nothing. Deliberate?"));
    }
    for (uint8_t j = i + 1; j < COIN_TABLE_LEN; j++)
    {
      if (COIN_TABLE[i].pulses == COIN_TABLE[j].pulses)
      {
        LOG(F("  FAIL: two coins share "));
        LOG(COIN_TABLE[i].pulses);
        LOGLN(F(" pulses. They are indistinguishable."));
        strncpy(haltReason, "DUPLICATE COINS", sizeof(haltReason) - 1);
        ok = false;
      }
    }
  }
  // Can any coin, alone or repeated, land exactly on the price?
  bool reachable = false;
  for (uint8_t i = 0; i < COIN_TABLE_LEN; i++)
  {
    if (COIN_TABLE[i].value > 0 && (PAD_PRICE % COIN_TABLE[i].value) == 0)
      reachable = true;
    if (COIN_TABLE[i].value >= PAD_PRICE)
      reachable = true;
  }
  if (!reachable)
  {
    LOG(F("  WARN: no coin divides KSH "));
    LOG(PAD_PRICE);
    LOGLN(F(" exactly. Customers will always overpay."));
  }
  LOG(F("  price       : KSH "));
  LOGLN(PAD_PRICE);
  LOG(F("  change      : "));
  LOGLN(KEEP_CHANGE_AS_CREDIT ? F("kept on the meter")
                                       : F("FORFEIT — bench setting only"));

  // --- 3. Pulse decoding sanity ---
  LOGLN(F("[3] PULSE DECODING"));
  LOG(F("  debounce    : "));
  LOG(COIN_PULSE_DEBOUNCE_MS);
  LOGLN(F(" ms"));
  LOG(F("  coin gap    : "));
  LOG(COIN_GAP_MS);
  LOGLN(F(" ms"));
  if (COIN_GAP_MS <= COIN_PULSE_DEBOUNCE_MS * 2)
  {
    LOGLN(F("  FAIL: gap is too close to the debounce. One coin will be"));
    LOGLN(F("        read as several. Raise COIN_GAP_MS."));
    strncpy(haltReason, "BAD PULSE TIMING", sizeof(haltReason) - 1);
    ok = false;
  }
  LOG(F("  coin line   : GPIO "));
  LOG(PIN_COIN);
  LOG(F(" reads "));
  LOGLN(digitalRead(PIN_COIN) == HIGH ? F("HIGH (idle, correct)")
                                               : F("LOW  <- stuck, or acceptor is pulling it"));
  if (digitalRead(PIN_COIN) == LOW)
  {
    LOGLN(F("  !! A permanently LOW coin line means no coin will ever"));
    LOGLN(F("  !! register a falling edge. Check the wiring first."));
  }

  // --- 4. I2C and the screen ---
  LOGLN(F("[4] SCREEN"));
  i2cScan();
  lcdOk = i2cPresent(LCD_I2C_ADDR);
  LOG(F("  LCD at 0x"));
  LOG(LCD_I2C_ADDR, HEX);
  LOGLN(lcdOk ? F("  : present") : F("  : NOT FOUND"));
  if (!lcdOk)
  {
    LOGLN(F("  Running headless. The machine still vends, but the"));
    LOGLN(F("  customer is told nothing. Fix before any demo."));
    LOGLN(F("  Try the other address the scan found, in SECTION 4."));
  }

  // --- 5. IR beam ---
  LOGLN(F("[5] IR DROP SENSOR"));
  irRaw = irStable = false;
  uint32_t t0 = millis();
  while (millis() - t0 < 200)
  {
    padSeen();
    buzzerService();
    delay(1); // yield, same as everywhere else that waits on the beam
  }
  LOG(F("  GPIO "));
  LOG(PIN_IR);
  LOG(F(" raw   : "));
  LOGLN(digitalRead(PIN_IR) == LOW ? F("LOW") : F("HIGH"));
  LOG(F("  polarity    : detected = "));
  LOGLN(IR_DETECTED_IS_LOW ? F("LOW") : F("HIGH"));
  LOG(F("  chute reads : "));
  LOGLN(irStable ? F("BLOCKED  <- problem") : F("clear    (correct when empty)"));
  // if (irStable)
  // {
  //   LOGLN(F("  A blocked chute at rest is one of three things:"));
  //   LOGLN(F("    - IR_DETECTED_IS_LOW is the wrong way round"));
  //   LOGLN(F("    - the sensor is aimed at nothing, or at the far wall"));
  //   LOGLN(F("    - there is genuinely a pad stuck in the chute"));
  //   LOGLN(F("  Use  i  to watch it live and wave your hand through."));
  // }

  if (irStable)
  {
    LOGLN(F("  FAIL: chute is BLOCKED at startup."));
    LOGLN(F("  Clear the chute and verify IR polarity/aiming."));
    strncpy(haltReason, "CHUTE BLOCKED", sizeof(haltReason) - 1);
    ok = false;
  }

  // --- 6. Relay and motor ---
  LOGLN(F("[6] RELAY / MOTOR"));
  LOG(F("  GPIO "));
  LOG(PIN_RELAY);
  LOG(F(", active "));
  LOGLN(RELAY_ACTIVE_HIGH ? F("HIGH") : F("LOW"));
  LOG(F("  dispense timeout : "));
  LOG(DISPENSE_TIMEOUT_MS);
  LOGLN(F(" ms"));
  LOG(F("  hard motor limit : "));
  LOG(motorHardLimitMs());
  LOGLN(F(" ms"));
  LOGLN(F("  Confirm the 10k pull-down on the relay pin is fitted."));
  if (SELFTEST_CLICKS_RELAY)
  {
    LOGLN(F("  Clicking the relay now — you should hear one click."));
    motorOn();
    delay(120);
    motorOff();
  }
  else
  {
    LOGLN(F("  Not clicking it. Set SELFTEST_CLICKS_RELAY once the"));
    LOGLN(F("  motor is disconnected, or just use  m500."));
  }

  // added during the additions of the jamming relay switch
  LOGLN(F("[7] COIN INHIBIT / CLEAR BUTTON"));
  LOG(F("  inhibit pin : GPIO "));
  LOG(PIN_INHIBIT);
  LOG(F(", active "));
  LOGLN(INHIBIT_RELAY_ACTIVE_HIGH ? F("HIGH") : F("LOW"));
  if (!INHIBIT_RELAY_ACTIVE_HIGH)
  {
    LOG(F("  Active LOW, so GPIO "));
    LOG(PIN_INHIBIT);
    LOGLN(F(" needs a 10k pull-UP to 3V3,"));
    LOGLN(F("  not a pull-down. The pin floats until setup() runs; held low"));
    LOGLN(F("  through that window the relay energises, the NC contact opens,"));
    LOGLN(F("  and the slot is live every time you reset. Watch for a click"));
    LOGLN(F("  through one reset with the acceptor connected before you ship."));
  }
  LOG(F("  contact     : "));
  if (INHIBIT_CONTACT_IS_NC)
  {
    LOGLN(F("NC — fail-safe, inhibited when unpowered"));
  }
  else
  {
    LOGLN(F("NO  <- NOT fail-safe"));
    LOGLN(F("  !! A dead board will accept coins and deliver nothing."));
    LOGLN(F("  !! Move to the NC contact before this leaves the bench."));
  }
  LOG(F("  coins now   : "));
  LOGLN(coinsAllowed ? F("enabled") : F("inhibited"));
  LOG(F("  clear button: GPIO "));
  LOG(PIN_CLEAR);
  LOG(F(" reads "));
  LOGLN(digitalRead(PIN_CLEAR) == HIGH ? F("HIGH (released, correct)")
                                                : F("LOW  <- held, shorted, or miswired"));
  if (digitalRead(PIN_CLEAR) == LOW)
  {
    LOGLN(F("  !! Held, shorted, or wired backwards. It will never"));
    LOGLN(F("  !! register a fresh press like this, so it cannot"));
    LOGLN(F("  !! clear a real fault. Use  x  until it's fixed."));
  }

  LOGLN(F("[8] PAD STOCK SWITCH"));
  if (!PAD_SWITCH_FITTED)
  {
    LOGLN(F("  BYPASSED — PAD_SWITCH_FITTED is false in SECTION 4."));
    LOGLN(F("  This machine can never report itself empty. Set it true"));
    LOGLN(F("  once the switch is wired, or it will keep taking money"));
    LOGLN(F("  over an empty coil."));
  }
  else
  {
    LOG(F("  GPIO "));
    LOG(PIN_PADS);
    LOG(F(" reads "));
    LOGLN(digitalRead(PIN_PADS) == LOW ? F("LOW") : F("HIGH"));
    LOG(F("  polarity    : pads present = "));
    LOGLN(PADS_PRESENT_IS_LOW ? F("LOW (switch closed to GND)")
                                       : F("HIGH (switch open)"));
    LOG(F("  stock reads : "));
    LOGLN(padsPresent() ? F("PADS PRESENT") : F("EMPTY  <- check this"));
    if (!padsPresent())
    {
      LOGLN(F("  The machine will boot straight into OUT OF STOCK and"));
      LOGLN(F("  refuse coins. If the coil is actually loaded then the"));
      LOGLN(F("  switch is open, miswired, or PADS_PRESENT_IS_LOW is"));
      LOGLN(F("  inverted. Use  k  to watch it while you work the lever."));
    }
  }

  LOGLN(F("[8b] STORED BALANCE"));
  if (!REMEMBER_CREDIT_ACROSS_POWER_LOSS)
  {
    LOGLN(F("  OFF — nothing is written to flash. A power cut with money on"));
    LOGLN(F("  the meter loses it silently. See SECTION 3."));
  }
  else
  {
    LOG(F("  on. Meter balance in flash: KSH "));
    LOGLN(prefs.getUShort("credit", 0));
    LOGLN(F("  This is the only thing this firmware stores."));
  }

  // --- 9. Buzzer ---
  LOGLN(F("[9] BUZZER"));
  LOGLN(F("  One short chirp now. Silence means check GPIO 25."));
  beep(TONE_COIN_HZ, TONE_COIN_MS);

  rule();
  LOGLN(ok ? F("SELF-TEST: PASS")
                    : F("SELF-TEST: FAIL — see the lines marked FAIL"));
  rule();
  return ok;
}

// ===========================================================================
//  SERIAL CONSOLE — 115200 baud, type ? for the list
// ===========================================================================
//
//  This is how the machine is commissioned, calibrated and diagnosed. It is
//  not a debug afterthought: the bring-up procedure in README.md is a sequence
//  of these commands.
//
//  The ones that matter most:
//
//    m<ms>   Run the motor for exactly <ms> and report when the beam broke.
//            This is how you time one revolution of the coil, which is the
//            only honest way to set DISPENSE_TIMEOUT_MS.
//    o<ms>   Set DISPENSE_TIMEOUT_MS live, so you can try a value before you
//            commit it to SECTION 5.
//    g<ms>   Same for COIN_GAP_MS.
//    i / k   Watch the IR beam / the stock switch live while you aim or mount
//            them. Any other command turns the watch off again.
//    d       Re-run the whole self-test without rebooting.
//    v<n>    Pretend an <n>-pulse coin went in, to exercise the logic with no
//            coins to hand.
//
//  Two behaviours worth knowing:
//
//    Nothing typed here is saved. There is no flash storage in this build, by
//    design. The console is for FINDING the number; SECTION 5 is where it
//    lives once you have it.
//
//    A typed command always answers in full, even on a machine installed with
//    SERIAL_LOG_DEFAULT false. handleCommand() forces logging on for its own
//    duration and puts it back afterwards, so a service visit can plug in a
//    laptop and press  s  on a machine that is otherwise silent.
//
void printHelp()
{
  rule();
  LOGLN(F("COMMANDS"));
  rule();
  LOGLN(F("  ?        this list"));
  LOGLN(F("  s        status — state, credit, counters, loop health"));
  LOGLN(F("  d        run the full self-test again"));
  LOGLN(F("  c        print the coin table"));
  LOGLN(F("  z        scan the I2C bus"));
  LOGLN();
  LOGLN(F("  CALIBRATION (lost on reset — write the value into CONFIG)"));
  LOGLN(F("  p<ksh>   set the pad price          e.g.  p20"));
  LOGLN(F("  g<ms>    set the coin gap           e.g.  g250"));
  LOGLN(F("  o<ms>    set the dispense timeout   e.g.  o3000"));
  LOGLN(F("  w<ms>    part-payment wait           e.g.  w120000"));
  LOGLN(F("           w0 = wait forever, never clear part payment"));
  LOGLN();
  LOGLN(F("  BENCH TESTS"));
  LOGLN(F("  m<ms>    run the motor for <ms> — time one revolution"));
  LOGLN(F("  t        full test vend, no coin needed"));
  LOGLN(F("  v<n>     pretend a coin of <n> pulses went in"));
  LOGLN(F("  i        watch the IR sensor live (any key stops it)"));
  LOGLN(F("  k        watch the pad stock switch live"));
  LOGLN(F("  l        LCD test pattern"));
  LOGLN(F("  b        buzzer test"));
  LOGLN();
  LOGLN(F("  x        clear a latched fault"));
  LOGLN(F("           (or hold the front-panel button for 1 s)"));
  LOGLN(F("  r        zero the session counters"));
  LOGLN(F("  h        toggle the heartbeat line"));
  LOGLN(F("  L        toggle event logging (off for field installs)"));
  rule();
}

void printCoinTable()
{
  rule();
  LOGLN(F("COIN TABLE — edit SECTION 2 to change it"));
  rule();
  for (uint8_t i = 0; i < COIN_TABLE_LEN; i++)
  {
    LOG(F("  "));
    LOG(COIN_TABLE[i].pulses);
    LOG(COIN_TABLE[i].pulses == 1 ? F(" pulse  -> ") : F(" pulses -> "));
    LOG(COIN_TABLE[i].name);
    LOG(F("  = KSH "));
    LOG(COIN_TABLE[i].value);
    if (COIN_TABLE[i].value >= PAD_PRICE)
    {
      LOG(F("   buys a pad outright"));
    }
    else if (COIN_TABLE[i].value > 0 && PAD_PRICE % COIN_TABLE[i].value == 0)
    {
      LOG(F("   x"));
      LOG(PAD_PRICE / COIN_TABLE[i].value);
      LOG(F(" buys a pad"));
    }
    LOGLN();
  }
  LOG(F("  PRICE: KSH "));
  LOGLN(PAD_PRICE);
  rule();
}

void printStatus()
{
  rule();
  LOGLN(F("STATUS"));
  rule();
  LOG(F("  firmware     : v"));
  LOGLN(F(FW_VERSION));
  LOG(F("  state        : "));
  LOGLN(stateName(state));
  LOG(F("  credit       : KSH "));
  LOG(credit);
  LOG(F(" of KSH "));
  LOGLN(PAD_PRICE);
  LOG(F("  part payment : "));
  if (CREDIT_TIMEOUT_MS == 0)
    LOGLN(F("waits forever, never cleared"));
  else
  {
    LOG(F("cleared after "));
    LOG(CREDIT_TIMEOUT_MS / 1000);
    LOGLN(F(" s"));
  }
  LOG(F("  motor        : "));
  LOGLN(motorRunning ? F("RUNNING") : F("off"));
  LOG(F("  chute beam   : "));
  LOGLN(irStable ? F("BLOCKED") : F("clear"));
  LOG(F("  pad stock    : "));
  if (!PAD_SWITCH_FITTED)
    LOGLN(F("switch not fitted — check bypassed"));
  else
    LOGLN(padsPresent() ? F("PADS PRESENT") : F("EMPTY"));
  LOG(F("  screen       : "));
  LOGLN(lcdOk ? F("ok") : F("MISSING — headless"));
  LOGLN();
  LOG(F("  vends        : "));
  LOGLN(vendCount);
  LOG(F("  coins ok     : "));
  LOGLN(coinsAccepted);
  LOG(F("  coins unknown: "));
  LOGLN(coinsUnknown);
  LOG(F("  taken        : KSH "));
  LOGLN(shillingsIn);
  LOG(F("  faults       : "));
  LOGLN(faultCount);
  LOG(F("  raw pulses   : "));
  LOGLN(isrTotal);
  LOGLN();
  LOG(F("  uptime       : "));
  LOG((millis() - bootMs) / 1000);
  LOGLN(F(" s"));
  LOG(F("  free heap    : "));
  LOGLN(ESP.getFreeHeap());
  LOG(F("  loop max     : "));
  LOG(loopMaxUs);
  LOGLN(F(" us"));
  LOGLN(F("  (loop max over ~50000 us means I2C is stalling)"));
  LOG(F("  coin acceptor: "));
  LOGLN(coinsAllowed ? F("enabled") : F("INHIBITED"));
  rule();
}

// Bench motor test: run the coil for a fixed time and report when the beam
// broke. This is how DISPENSE_TIMEOUT_MS gets its number.
//
//  NOTE: this deliberately steps outside the loop() motor backstop, because it
//  blocks rather than returning to loop(). Its own 10 s ceiling is the only
//  limit while it runs. That is fine for a command an operator types while
//  watching the machine, and wrong for anything automatic — which is why
//  nothing else in the firmware drives the motor this way.
void runMotorFor(uint32_t ms)
{
  if (ms == 0 || ms > 10000)
  {
    LOGLN(F("Refusing. Use 1 to 10000 ms."));
    return;
  }
  LOG(F("Motor ON for "));
  LOG(ms);
  LOGLN(F(" ms..."));
  say("MOTOR TEST", "");
  uint32_t t0 = millis();
  motorOn();
  bool broke = false;
  while (millis() - t0 < ms)
  {
    if (padSeen() && !broke)
    {
      broke = true;
      LOG(F("  beam broken at "));
      LOG(millis() - t0);
      LOGLN(F(" ms"));
    }
    buzzerService();
    delay(1); // this can run for ten seconds; give the watchdog its turn
  }
  motorOff();
  LOGLN(F("Motor OFF."));
  if (!broke)
    LOGLN(F("  beam never broke during the run"));
  LOGLN(F("Time one full revolution this way, then set"));
  LOGLN(F("DISPENSE_TIMEOUT_MS to about double it  (o<ms>)."));
  showIdle();
}

// Returns false if it refused to start. The caller must not take the money
// until this has returned true.
bool startVend(const char *why)
{
  // Refuse to run the motor blind. If the beam already reads "pad", the
  // sensor is mis-wired, mis-aimed, or something is stuck in the chute.
  padSeen();
  uint32_t t0 = millis();
  bool clear = true;
  while (millis() - t0 < IR_SETTLE_MS)
  {
    if (padSeen())
    {
      clear = false;
      break;
    }
    buzzerService();
    delay(1); // yield: 1 ms is well inside IR_DEBOUNCE_MS, so nothing is missed
  }
  if (!clear)
  {
    LOGLN(F("Refusing to start: the beam already reads BLOCKED."));
    enterFault("CHECK SENSOR");
    return false;
  }
  say("DISPENSING...", "");
  LOG(F("Motor on — "));
  LOGLN(why);
  motorOn();
  dispenseStartMs = millis();
  state = ST_DISPENSING;
  return true;
}

// True while a vend is in flight. Any console command that BLOCKS for more
// than a few milliseconds has to refuse when this is true.
//
//  While the motor is turning, loop() is the only thing watching the beam and
//  enforcing both the dispense timeout and the motor backstop. A command that
//  sits in its own loop for a second or two stops all of that: the pad can
//  fall unnoticed, and the coil keeps turning with nothing counting the time.
//
//  DELIVERED is included because the beam is still clearing and "TAKE YOUR
//  PAD" is still the thing the customer needs to be reading.
static bool vendInFlight()
{
  return state == ST_DISPENSING || state == ST_DELIVERED;
}

void handleCommand(char *s)
{
  char c = s[0];
  long n = (strlen(s) > 1) ? atol(s + 1) : -1;

  // A typed command always gets an answer, even on a machine installed with
  // event logging off. Put back to the configured level on the way out.
  logOn = true;

  // Any command other than the watch itself stops a running watch, so its
  // chatter never buries the answer you just asked for.
  if (irWatch && c != 'i')
  {
    irWatch = false;
    LOGLN(F("IR watch off."));
  }
  if (stockWatch && c != 'k')
  {
    stockWatch = false;
    LOGLN(F("Stock watch off."));
  }

  switch (c)
  {
  case '?':
    printHelp();
    break;

  case 'h':
    heartbeatOn = !heartbeatOn;
    LOG(F("Heartbeat "));
    LOGLN(heartbeatOn ? F("ON") : F("off"));
    break;

  case 's':
    printStatus();
    break;
  case 'c':
    printCoinTable();
    break;
  case 'z':
    i2cScan();
    break;
  case 'd':
    if (vendInFlight())
    {
      LOGLN(F("A vend is in progress. The self-test blocks for over a second"));
      LOGLN(F("and resets the beam debouncer, so it could miss the pad falling"));
      LOGLN(F("while the coil keeps turning. Wait for IDLE, then press d."));
      break;
    }
    selfTestPassed = selfTest(true);
    // Put the screen back to whatever the machine actually is. showIdle() on
    // its own used to paint INSERT COIN over a faulted or empty machine and
    // leave it there until the next rotation tick.
    faultRotateMs = 0;
    emptyRotateMs = 0;
    if (state == ST_IDLE)
      showIdle();
    break;
  case 'b':
    LOGLN(F("Chirp."));
    beep(TONE_VEND_HZ, TONE_VEND_MS);
    break;

  case 'l':
    if (!lcdOk)
    {
      LOGLN(F("No LCD found. Run  z  to scan."));
      break;
    }
    if (vendInFlight())
    {
      LOGLN(F("A vend is in progress. This holds the screen for two seconds"));
      LOGLN(F("and would stop the beam being watched. Wait for IDLE."));
      break;
    }
    LOGLN(F("LCD: re-initialised, both lines filled, then back to idle."));
    LOGLN(F("If the screen was showing garbage, this clears it. If it comes"));
    LOGLN(F("back after the next vend, the motor is browning out the display"));
    LOGLN(F("— see README, 'Funny symbols on the screen'."));
    lcdNeedsReinit = true; // force a full rebuild, that is the point of  l
    say("0123456789ABCDEF", "BRICAPA  v" FW_VERSION);
    holdFor(2000);
    faultRotateMs = 0;
    emptyRotateMs = 0;
    if (state == ST_IDLE)
      showIdle();
    break;

  case 'L':
    logEvents = !logEvents;
    LOG(F("Event logging "));
    LOGLN(logEvents ? F("ON")
                             : F("OFF - typed commands still answer in full"));
    LOGLN(F("Not saved. Set SERIAL_LOG_DEFAULT in SECTION 7 to keep it."));
    break;

  case 'k':
    if (!PAD_SWITCH_FITTED)
    {
      LOGLN(F("Stock switch is bypassed (PAD_SWITCH_FITTED is false)."));
      LOGLN(F("Set it true in SECTION 4 once the switch is wired."));
      break;
    }
    stockWatch = !stockWatch;
    LOGLN(stockWatch ? F("Stock watch ON - work the switch lever.")
                              : F("Stock watch off."));
    break;

  case 'i':
    irWatch = !irWatch;
    LOGLN(irWatch ? F("IR watch ON — wave a pad through the chute.")
                           : F("IR watch off."));
    break;

  case 'x':
    attemptClear("serial console");
    break;

  case 'r':
    vendCount = coinsAccepted = coinsUnknown = faultCount = shillingsIn = 0;
    isrTotal = 0;
    loopMaxUs = 0;
    LOGLN(F("Counters zeroed."));
    break;

  case 'p':
    if (n <= 0 || n > 1000)
    {
      LOGLN(F("Use p1 to p1000."));
      break;
    }
    PAD_PRICE = (uint16_t)n;
    LOG(F("Price is now KSH "));
    LOGLN(PAD_PRICE);
    LOGLN(F("Not saved. Put it in SECTION 3 to keep it."));
    showIdle();
    break;

  case 'g':
    if (n <= 0 || n > 3000)
    {
      LOGLN(F("Use g1 to g3000."));
      break;
    }
    COIN_GAP_MS = (uint32_t)n;
    LOG(F("Coin gap is now "));
    LOG(COIN_GAP_MS);
    LOGLN(F(" ms"));
    if (COIN_GAP_MS <= COIN_PULSE_DEBOUNCE_MS * 2)
      LOGLN(F("WARNING: that is too close to the debounce."));
    LOGLN(F("Not saved. Put it in SECTION 5 to keep it."));
    break;

  case 'w':
    if (n < 0 || n > 3600000L)
    {
      LOGLN(F("Use w0 (wait forever) to w3600000 (one hour)."));
      break;
    }
    CREDIT_TIMEOUT_MS = (uint32_t)n;
    if (CREDIT_TIMEOUT_MS == 0)
      LOGLN(F("Part payment will now WAIT FOREVER. It never expires."));
    else
    {
      LOG(F("Part payment now waits "));
      LOG(CREDIT_TIMEOUT_MS / 1000);
      LOGLN(F(" s before it is cleared."));
    }
    LOGLN(F("Not saved. Put it in SECTION 5 to keep it."));
    creditTickMs = 0; // repaint the countdown straight away
    break;

  case 'o':
    if (n <= 0 || n > 30000)
    {
      LOGLN(F("Use o1 to o30000."));
      break;
    }
    DISPENSE_TIMEOUT_MS = (uint32_t)n;
    LOG(F("Dispense timeout is now "));
    LOG(DISPENSE_TIMEOUT_MS);
    LOGLN(F(" ms"));
    LOG(F("Hard motor limit follows it at "));
    LOG(motorHardLimitMs());
    LOGLN(F(" ms"));
    LOGLN(F("Not saved. Put it in SECTION 5 to keep it."));
    break;

  case 'm':
    if (state == ST_FAULT)
    {
      LOGLN(F("Faulted. Clear with  x  first."));
      break;
    }
    // Deliberately allowed while EMPTY: this is a raw bench test for timing a
    // revolution, and an empty coil is exactly when you want to run it.
    if (state != ST_IDLE && state != ST_EMPTY)
    {
      LOGLN(F("Busy. Wait for IDLE."));
      break;
    }
    runMotorFor(n < 0 ? 500 : (uint32_t)n);
    break;

  case 't':
    if (state == ST_FAULT)
    {
      LOGLN(F("Faulted. Clear with  x  first."));
      break;
    }
    if (state != ST_IDLE)
    {
      LOGLN(F("Busy. Wait for IDLE."));
      break;
    }
    LOGLN(F("Test vend — no coin taken, no money counted."));
    startVend("test vend from console");
    break;

  case 'v':
  {
    if (n <= 0)
    {
      LOGLN(F("Use v1, v2, ... to fake a coin."));
      break;
    }
    LOG(F("Simulating a "));
    LOG(n);
    LOGLN(F("-pulse coin."));
    // Same rules as a real coin, so what this prints is what would actually
    // have happened. Banked while the machine can still deliver; refused in
    // EMPTY and FAULT, where it cannot.
    const CoinDef *coin = lookupCoin((uint16_t)n);
    if (!coin)
      coinsUnknown++;
    else if (state == ST_IDLE || state == ST_DISPENSING || state == ST_DELIVERED)
      bankCoin(coin);
    reportCoin((uint16_t)n, coin);
    if (state == ST_IDLE)
      showIdle();
    break;
  }

  default:
    LOG(F("Unknown command '"));
    LOG(s);
    LOGLN(F("'. Type ? for the list."));
  }

  logOn = logEvents;
}

// Read the console without ever blocking.
//
//  Commands normally arrive terminated by a newline. But the Arduino Serial
//  Monitor defaults to "No line ending", and somebody on a bench at midnight
//  should not have to know that — so if characters have arrived and then
//  stopped for CMD_IDLE_FLUSH_MS, the command is taken anyway. Both settings
//  work, and nobody has to be told which one to use.
void pollSerial()
{
  while (Serial.available())
  {
    char ch = (char)Serial.read();
    cmdLastMs = millis();
    if (ch == '\r')
      continue;
    if (ch == '\n')
    {
      cmdBuf[cmdLen] = 0;
      if (cmdLen)
        handleCommand(cmdBuf);
      cmdLen = 0;
      continue;
    }
    if (cmdLen < sizeof(cmdBuf) - 1)
      cmdBuf[cmdLen++] = ch;
  }

  // Monitors set to "No line ending" never send \n. Take the command anyway
  // once typing has paused.
  if (cmdLen > 0 && (millis() - cmdLastMs) > CMD_IDLE_FLUSH_MS)
  {
    cmdBuf[cmdLen] = 0;
    handleCommand(cmdBuf);
    cmdLen = 0;
  }
}

// ===========================================================================
//  SETUP — boot order, and why it is this order
// ===========================================================================
//
//    1. Relay pins to OUTPUT and both relays OFF. FIRST, before anything else,
//       including Serial. Until this line runs the pins are floating, and a
//       floating pin is a motor that can twitch and a coin slot that is live.
//    2. Serial up, banner out.
//    3. Remaining pins, and prime the stock debouncer from the real pin so the
//       machine does not announce a change it never made.
//    4. Self-test. Nine checks, all printed.
//    5. LCD up, but only if the self-test actually found it on the bus.
//    6. If the self-test was FATAL: show it, stay in ST_FAULT, and RETURN
//       WITHOUT ATTACHING THE COIN INTERRUPT. The console stays alive. The
//       acceptor is still inhibited from step 1, so no money can come in that
//       the firmware cannot account for.
//    7. Otherwise attach the coin interrupt — the single point at which this
//       machine becomes able to take money — and open for business, or go
//       straight to OUT OF STOCK if the coil is already empty.
//
void setup()
{
  // Relay first and before anything else, so the motor cannot twitch on boot.
  pinMode(PIN_RELAY, OUTPUT);
  pinMode(PIN_INHIBIT, OUTPUT);
  coinAcceptorAllow(false); // inhibited from the first instruction
  motorOff();

  Serial.begin(115200);
  delay(300);
  bootMs = millis();

  // The banner and the self-test always print, whatever SECTION 7 says. A
  // reset in the field has to tell a service visit what it needs to know.
  logEvents = SERIAL_LOG_DEFAULT;
  logOn = true;

  LOGLN();
  rule();
  LOGLN(F("  BRICAPA PAD DISPENSER  -  prototype firmware v" FW_VERSION));
  LOGLN(F("  Ref BT-PADVEND-2026-001"));
  rule();
  LOGLN(F("  Type ? for commands. Console changes are not saved; the"));
  LOGLN(F("  meter balance is the only thing kept through a power cut."));
  rule();

  pinMode(PIN_COIN, INPUT_PULLUP);
  pinMode(PIN_CLEAR, INPUT_PULLUP);
  pinMode(PIN_IR, INPUT);
  pinMode(PIN_PADS, INPUT_PULLUP);

  // Prime the stock debouncer from the real pin so the machine does not
  // announce a state change it never actually made on the first pass.
  delay(5);
  {
    bool lvl = (digitalRead(PIN_PADS) == LOW);
    padsRaw = padsStable = PAD_SWITCH_FITTED
                               ? (PADS_PRESENT_IS_LOW ? lvl : !lvl)
                               : true;
  }

  buzzerInit();

  // The meter balance is the only thing this firmware keeps in flash.
  prefs.begin("padvend", false);

  Wire.begin(PIN_SDA, PIN_SCL);
  // 100 kHz, stated rather than inherited. This is the slowest standard I2C
  // rate and the most tolerant of long unshielded runs to a display mounted on
  // a cabinet door. There is nothing on this bus that needs to go faster.
  Wire.setClock(100000);

  selfTestPassed = selfTest(true);

  if (lcdOk)
  {
    lcd.init();
    lcd.backlight();
  }

  if (!selfTestPassed && HALT_ON_SELFTEST_FAIL)
  {
    say("SETUP ERROR", haltReason[0] ? haltReason : "SEE SERIAL");
    armClearButtonForNewFault();
    LOGLN();
    LOGLN(F("HALTED. The self-test found something that would make this"));
    LOGLN(F("machine take money and do the wrong thing with it."));
    LOGLN(F("The coin input is left DETACHED, so it cannot bank money."));
    LOGLN(F("Fix CONFIG and reflash. The console stays alive."));
    state = ST_FAULT;
    return; // no attachInterrupt: coins cannot be counted in this condition
  }

  say("PAD DISPENSER", "BRICAPA  v" FW_VERSION);
  holdFor(1200);

  // ---- Restore a balance left on the meter by a power cut ----
  //
  //  The countdown deliberately restarts from here rather than resuming where
  //  it left off. A short cut protects the customer who is still standing
  //  there; a long one must not leave a stranger's balance on the screen all
  //  night for whoever opens up in the morning.
  if (REMEMBER_CREDIT_ACROSS_POWER_LOSS)
  {
    uint16_t saved = prefs.getUShort("credit", 0);
    if (saved > 0)
    {
      credit = saved; // not setCredit(): the stored value is already this
      lastCoinMs = millis();
      creditTickMs = 0;
      rule();
      LOG(F("RECOVERED KSH "));
      LOG(credit);
      LOGLN(F(" left on the meter when the power went."));
      if (credit >= PAD_PRICE)
        LOGLN(F("  That is a full pad, already paid for. Vending it now."));
      else
      {
        LOG(F("  Part payment. The countdown restarts now, so it expires "));
        LOG(CREDIT_TIMEOUT_MS / 1000);
        LOGLN(F(" s"));
        LOGLN(F("  from this boot rather than sitting here indefinitely."));
      }
      rule();
    }
  }

  attachCoinIsr();

  if (!padsPresent())
  {
    enterEmpty();
  }
  else
  {
    showIdle();
    LOGLN(F("Ready. Waiting for a coin."));
  }

  if (!logEvents)
  {
    LOGLN(F("Event logging is OFF for the field (SECTION 7). Typed"));
    LOGLN(F("console commands still answer in full. Press L for the"));
    LOGLN(F("running commentary, ? for the command list."));
  }
  rule();
}

// ===========================================================================
//  LOOP
// ===========================================================================
//  THE ORDER BELOW IS LOAD-BEARING. Roughly:
//
//    1. Motor backstop        first, and trusting nothing below it
//    2. Serial + clear button so a person can always get in
//    3. Watches               live sensor views, if one is running
//    4. Coins                 decode any finished coin and bank it
//    5. Stock                 empty -> ST_EMPTY, refilled -> ST_IDLE
//    6. Re-enable the slot    only if we are still IDLE after step 5
//    7. The state machine     which may start a vend
//
//  Two of those orderings are not arbitrary:
//
//    Stock (5) runs BEFORE the acceptor is re-enabled (6), so returning to
//    IDLE over an empty coil never clicks the relay on and straight back off.
//
//    Stock (5) runs BEFORE the state machine (7), so credit that arrives in
//    the same pass as the coil running dry cannot start a vend that has no pad
//    to give.
//
//  Stock is deliberately NOT checked during a vend. Once the motor is turning,
//  the IR beam is the only authority on whether a pad actually fell — not a
//  switch sitting under a stack that is in the middle of moving.
void loop()
{
  uint32_t loopStartUs = micros();
  uint32_t now = millis();

  // A live watch is an explicit request to see something, so it overrides the
  // field logging switch for as long as it is running.
  logOn = logEvents || irWatch || stockWatch;

  buzzerService();
  pollSerial();
  clearButtonService();

  // ---- Backstop. Checked in every state, before anything else can run. ----
  // Nothing below this line is trusted to stop the motor on its own.
  if (motorRunning && (now - motorOnSince) > motorHardLimitMs())
  {
    motorOff();
    LOGLN(F("!! Hard motor limit hit. The state machine did not stop it."));
    LOGLN(F("!! That is a firmware bug, not a jam. Report it."));
    enterFault("MOTOR RUNAWAY");
  }

  // ---- Live IR watch, for aiming the sensor ----
  if (irWatch)
  {
    static uint32_t lastWatch = 0;
    static bool lastVal = false;
    bool v = padSeen();
    if (v != lastVal || (now - lastWatch) > 1000)
    {
      lastVal = v;
      lastWatch = now;
      LOG(F("IR: raw="));
      LOG(digitalRead(PIN_IR) == LOW ? F("LOW ") : F("HIGH"));
      LOG(F("  debounced="));
      LOGLN(v ? F("BLOCKED") : F("clear"));
    }
  }

  // ---- A pad sitting in the beam while idle is worth saying out loud ----
  if (state == ST_IDLE && irStable && !irStuckWarned &&
      (now - irBlockedSince) > IR_STUCK_WARN_MS)
  {
    irStuckWarned = true;
    LOGLN(F("WARN: the chute has read BLOCKED for a while at rest."));
    LOGLN(F("      Something is in it, or the sensor needs re-aiming."));
    LOGLN(F("      The next paid vend will fault rather than run blind."));
  }

  // ---- Coins ----
  CoinBurst cb;
  if (takeCoin(cb))
  {
    const CoinDef *coin = lookupCoin(cb.pulses);

    if (!coin)
    {
      coinsUnknown++;
      beep(TONE_REJECT_HZ, TONE_REJECT_MS);
      if (state == ST_IDLE)
        say("COIN NOT KNOWN", "SEE SERIAL");
      reportCoin(cb.pulses, coin, &cb);
      if (state == ST_IDLE)
      {
        holdFor(1200);
        showIdle();
      }
    }
    else if (state == ST_EMPTY || state == ST_FAULT)
    {
      // The slot was inhibited, so this can only be a coin that was already
      // mid-flight when things went wrong. The machine cannot promise a pad,
      // so it must not go on the meter. It is lost — the coin banked into the
      // housing before the firmware heard a single pulse and there is no way
      // out of there — but it is reported rather than pocketed in silence.
      beep(TONE_REJECT_HZ, TONE_REJECT_MS);
      reportCoin(cb.pulses, coin, &cb);
    }
    else
    {
      // IDLE, DISPENSING or DELIVERED: the machine can honour this, so bank
      // it. Mid-vend is fine — those states end in IDLE within a second or
      // two, and the credit simply buys the next pad.
      if (bankCoin(coin))
        beep(TONE_COIN_HZ, TONE_COIN_MS);
      else
        beep(TONE_REJECT_HZ, TONE_REJECT_MS);
      reportCoin(cb.pulses, coin, &cb);

      if (state == ST_DISPENSING)
      {
        char l2[17];
        snprintf(l2, sizeof(l2), "CREDIT KSH %u", credit);
        say("PLEASE WAIT", l2);
      }
      else if (state == ST_IDLE && credit < PAD_PRICE)
      {
        showIdle(); // same screen as change left over from a vend
      }
      // DELIVERED is left alone on purpose: "TAKE YOUR PAD" must stay up.
      // IDLE with credit >= PAD_PRICE is left alone too: a vend starts on the
      // very next pass and paints DISPENSING over it.
    }
  }

  // ---- Stock. Only ever acted on between vends: a dispense already running
  // is never aborted, because the IR beam is the authority on whether a pad
  // actually fell, not a switch under a shifting stack. ----
  bool stocked = padsPresent();
  if (state == ST_IDLE && !stocked)
  {
    enterEmpty();
  }
  else if (state == ST_EMPTY && stocked)
  {
    LOGLN(F("Stock switch reports pads loaded. Back in service."));
    state = ST_IDLE;
    showIdle();
  }

  // ---- Live stock watch, for mounting the switch ----
  if (stockWatch)
  {
    static uint32_t lastStockWatch = 0;
    static bool lastStockVal = false;
    if (stocked != lastStockVal || (now - lastStockWatch) > 1000)
    {
      lastStockVal = stocked;
      lastStockWatch = now;
      LOG(F("STOCK: raw="));
      LOG(digitalRead(PIN_PADS) == LOW ? F("LOW ") : F("HIGH"));
      LOG(F("  debounced="));
      LOGLN(stocked ? F("PADS PRESENT") : F("EMPTY"));
    }
  }

  // ---- The coin slot. One rule, evaluated here and nowhere else. ----
  //
  //  The acceptor is LIVE whenever the machine can still honour a coin, and
  //  that is every state except the two where it cannot:
  //
  //      EMPTY  no pads to give
  //      FAULT  something is broken
  //
  //  It deliberately stays live during a vend. A coin that lands mid-vend is
  //  banked and buys the next pad, so there is nothing to protect against —
  //  and inhibiting for every sale would work the relay twice per vend for no
  //  benefit, which is thousands of needless operations over a machine's life.
  //
  //  Driving it from state in one place means the slot can never disagree with
  //  what the machine is actually doing. There is no way to enter a state and
  //  forget to open or close it, because nothing else touches it.
  coinAcceptorAllow(state != ST_EMPTY && state != ST_FAULT);

  switch (state)
  {

  // -----------------------------------------------------------------
  case ST_IDLE:
  {
    // ---- Part payment: keep asking, and show how long is left. ----
    if (credit > 0 && credit < PAD_PRICE)
    {
      // Repaint line 2 once a second. Line 1 already says what she has, and
      // does not change until another coin lands, so it is left alone.
      if (reached(now, creditTickMs))
      {
        creditTickMs = now + 1000;
        char l2[17];
        creditPromptLine(l2, sizeof(l2), now);
        sayLine2(l2);
      }

      // CREDIT_TIMEOUT_MS of 0 means wait forever, so the clock never runs
      // out. Without this guard a zero would make (now - lastCoinMs) > 0 true
      // almost immediately and wipe the credit on the very next pass.
      if (CREDIT_TIMEOUT_MS > 0 && (now - lastCoinMs) > CREDIT_TIMEOUT_MS)
      {
        LOG(F("Part payment of KSH "));
        LOG(credit);
        LOG(F(" timed out after "));
        LOG(CREDIT_TIMEOUT_MS / 1000);
        LOGLN(F(" s, cleared."));
        LOGLN(F("  Nobody came back for it. The coin is in the box and there"));
        LOGLN(F("  is no way to return it — see CREDIT_TIMEOUT_MS."));
        setCredit(0);
        showIdle();
      }
    }

    // THE CUSTOMER PAYS ON DELIVERY, NOT ON THE MOTOR STARTING.
    //
    //  Nothing is taken off the meter here. The money comes off in
    //  ST_DISPENSING, at the moment the beam confirms a pad actually fell.
    //
    //  This machine cannot give change and cannot give refunds — there is no
    //  hopper and nothing to pay out with. So the only way it can be honest
    //  about a vend that fails is to never have taken the money in the first
    //  place. Charge on delivery and a jam, an empty coil or a missed beam
    //  all leave the credit sitting on the meter where the customer can see
    //  it, instead of quietly consuming it for a pad that never arrived.
    if (credit >= PAD_PRICE)
      startVend("credit met");
    break;
  }

  // -----------------------------------------------------------------
  case ST_DISPENSING:
  {
    if (padSeen())
    {
      motorOff();
      uint32_t took = millis() - dispenseStartMs;

      // Delivered. NOW take the money — this is the only line in the firmware
      // that spends a customer's credit.
      //
      // The >= guard is not decoration: PAD_PRICE can be changed from the
      // console with  p  while a vend is in flight, and an unsigned subtraction
      // that went negative would wrap to about 65000 shillings of credit and
      // hand out free pads until someone noticed.
      uint16_t charged = (credit >= PAD_PRICE) ? PAD_PRICE : credit;
      setCredit(KEEP_CHANGE_AS_CREDIT ? (credit - charged) : 0);

      // KNOWN RACE, ACCEPTED DELIBERATELY. There are a few milliseconds
      // between the pad landing and the new balance reaching flash. A power
      // cut in exactly that gap leaves the OLD balance stored, so the machine
      // boots owing a pad it has already delivered and vends one more.
      //
      // The alternative is writing flash BEFORE confirming delivery, which
      // reopens the far worse failure this ordering exists to close: taking
      // money for a pad that never came out. Given a choice of which way to
      // be wrong in a window of milliseconds, err toward the customer.

      vendCount++;
      // If there is change, name the amount rather than just saying some
      // exists. She is about to be asked to add to it.
      char l2[17];
      if (credit > 0)
        snprintf(l2, sizeof(l2), "CREDIT KSH %u", credit);
      else
        l2[0] = 0;
      say("TAKE YOUR PAD", l2);
      beep(TONE_VEND_HZ, TONE_VEND_MS);
      messageUntilMs = millis() + MESSAGE_HOLD_MS;
      state = ST_DELIVERED;
      rule();
      LOG(F("PAD DELIVERED in "));
      LOG(took);
      LOGLN(F(" ms"));
      LOG(F("  session vends : "));
      LOGLN(vendCount);
      LOG(F("  charged       : KSH "));
      LOGLN(charged);
      LOG(F("  credit left   : KSH "));
      LOGLN(credit);
      if (took < 150)
      {
        LOGLN(F("  NOTE: that was very fast. Either the beam was"));
        LOGLN(F("  already breaking, or the sensor sees the coil."));
      }
      rule();
    }
    else if ((now - dispenseStartMs) > DISPENSE_TIMEOUT_MS)
    {
      motorOff();
      LOG(F("No pad in "));
      LOG(DISPENSE_TIMEOUT_MS);
      LOGLN(F(" ms."));
      if (credit > 0)
      {
        LOG(F("  KSH "));
        LOG(credit);
        LOGLN(F(" is still on the meter — NOT taken for this attempt."));
      }

      // The stock switch can tell the two causes apart, and they want
      // opposite handling: running dry is routine and has to come back on its
      // own after a refill, while a jam needs somebody to open the door.
      // Latching OUT OF ORDER every time the machine merely ran out would
      // waste a call-out and leave the site dead until someone drove there.
      if (PAD_SWITCH_FITTED && !padsPresent())
      {
        LOGLN(F("  Stock switch says the coil is empty, so this is not a jam."));
        enterEmpty();
      }
      else
      {
        LOGLN(F("  Pad wedged, coil empty, or the timeout is too short for"));
        LOGLN(F("  this motor. Time a revolution with  m1000."));
        enterFault(PAD_SWITCH_FITTED ? "JAMMED" : "EMPTY OR JAMMED");
      }
    }
    break;
  }

  // -----------------------------------------------------------------
  case ST_DELIVERED:
  {
    // Let the beam clear as the pad finishes falling.
    padSeen();
    if (reached(now, messageUntilMs))
    {
      showIdle();
      state = ST_IDLE;
      LOGLN(F("Ready."));
    }
    break;
  }

  // -----------------------------------------------------------------
  case ST_EMPTY:
  {
    // Credit can still be sitting here from someone who paid just as the last
    // pad went. It buys a pad the moment the coil is refilled, or it times out
    // like any other partial payment.
    // Same rule as ST_IDLE: only PART payment times out. Credit that is
    // enough for a whole pad stays put and waits for the refill.
    //
    //  The reason is that the coin is already in the cash box. The acceptor
    //  banks a coin before it sends a single pulse, so by the time this
    //  firmware knows money arrived, the money is gone into the housing and
    //  nothing can eject it. Inhibiting the acceptor stops the NEXT coin; it
    //  cannot undo one already taken.
    //
    //  So a full pad's worth of credit is not a number on a screen, it is a
    //  debt the machine has already been paid for. Wiping it would mean money
    //  in the box and no pad out of it, which is the exact failure that paying
    //  on delivery exists to prevent — it would be pointless to fix that at the
    //  vend and then undo it with a timeout twenty seconds later.
    if (CREDIT_TIMEOUT_MS > 0 && credit > 0 && credit < PAD_PRICE &&
        (now - lastCoinMs) > CREDIT_TIMEOUT_MS)
    {
      LOG(F("Part payment of KSH "));
      LOG(credit);
      LOGLN(F(" timed out while out of stock, cleared."));
      setCredit(0);
    }
    // The screen is the only thing standing between a customer and a locked
    // door, so say all three things in turn: that it is empty, that she is not
    // being charged, and — if she already paid — that her money is still here.
    if (reached(now, emptyRotateMs))
    {
      emptyRotateMs = now + FAULT_ROTATE_MS;
      char l2[17];
      switch (emptyMsgIdx)
      {
      case 0:
        strncpy(l2, "NO COINS TAKEN", sizeof(l2));
        break;
      case 1:
        strncpy(l2, "REFILL NEEDED", sizeof(l2));
        break;
      default:
        // Only reachable while credit > 0, except for the one frame after it
        // times out with the index already parked here.
        if (credit > 0)
          snprintf(l2, sizeof(l2), "CREDIT KSH %u", credit);
        else
          strncpy(l2, "NO COINS TAKEN", sizeof(l2));
        break;
      }
      l2[sizeof(l2) - 1] = 0;
      say("SORRY - NO PADS", l2);
      // The third panel only exists while somebody is owed something.
      uint8_t panels = (credit > 0) ? 3 : 2;
      emptyMsgIdx = (emptyMsgIdx + 1) % panels;
    }
    break;
  }

  // -----------------------------------------------------------------
  case ST_FAULT:
  {
    // Latched on purpose. No reverse, no self-retry. Coins are inhibited,
    // so it costs nobody anything to sit here until someone opens the door.
    if (reached(now, faultRotateMs))
    {
      faultRotateMs = now + FAULT_ROTATE_MS;
      const char *l2;
      switch (faultMsgIdx)
      {
      case 0:
        l2 = faultLine2[0] ? faultLine2 : "SEE SERIAL";
        break;
      case 1:
        l2 = "NO COINS TAKEN";
        break;
      default:
        l2 = "HOLD RESET BTN";
        break;
      }
      say("OUT OF ORDER", l2);
      faultMsgIdx = (faultMsgIdx + 1) % 3;
    }
    break;
  }
  }

  // ---- Heartbeat, for soak tests ----
  if (heartbeatOn && (now - lastBeatMs) > HEARTBEAT_MS)
  {
    lastBeatMs = now;
    LOG(F("[hb] "));
    LOG((now - bootMs) / 1000);
    LOG(F("s  "));
    LOG(stateName(state));
    LOG(F("  credit KSH "));
    LOG(credit);
    LOG(F("  vends "));
    LOG(vendCount);
    LOG(F("  faults "));
    LOG(faultCount);
    LOG(F("  beam "));
    LOG(irStable ? F("BLOCKED") : F("clear"));
    LOG(F("  stock "));
    LOG(!PAD_SWITCH_FITTED ? F("n/a") : (padsStable ? F("ok") : F("EMPTY")));
    LOG(F("  heap "));
    LOGLN(ESP.getFreeHeap());
  }

  // ---- Loop health ----
  uint32_t took = micros() - loopStartUs;
  if (took > loopMaxUs)
    loopMaxUs = took;

  // Hand the CPU back. The idle task needs it to feed the watchdog, and 1 ms
  // is well inside the IR debounce window so nothing is missed.
  delay(1);
}
