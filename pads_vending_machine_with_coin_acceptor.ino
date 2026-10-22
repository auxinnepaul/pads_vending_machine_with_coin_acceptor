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

  ONE thing is stored in flash: the amount on the meter, so a power cut does
  not swallow part payment. See SECTION 3, REMEMBER_CREDIT_ACROSS_POWER_LOSS.

  Nothing else is. Every console setting, and every counter — vends, takings,
  faults — is lost on reset, and that is on purpose: the console is for finding
  the right number, then you write that number into CONFIG below and flash it.

  ---------------------------------------------------------------------------
  WIRING
  ---------------------------------------------------------------------------
    12 V adaptor --+-- LM2596 buck -> 5.0 V -> Schottky -> ESP32 VIN
                   |                        +- ESP32 3V3 pin -> KY-032 VCC
                   +-- coin acceptor  (12 V)
                   +-- relay COM/NO   -> motor  (flyback diode across motor)

    GPIO 21  LCD SDA
    GPIO 22  LCD SCL
    GPIO 18  Coin acceptor pulse   (via PC817, INPUT_PULLUP, pulled LOW per pulse)
    GPIO 19  Coin inhibit relay    (active HIGH, 10k pull-DOWN to GND, NC contact)
    GPIO 26  Relay control         (active HIGH, 10k pull-DOWN to GND)
    GPIO 27  KY-032 IR output
    GPIO 25  Passive buzzer

  ---------------------------------------------------------------------------
  BEFORE FIRST POWER-UP — three things that kill hardware
  ---------------------------------------------------------------------------
    1. Set the LM2596 to 5.0 V with a meter BEFORE connecting the ESP32.
    2. The coin line goes through a PC817, never straight to GPIO 18. It idles
       at 0 V, so measuring it proves nothing: plugging and unplugging the
       acceptor put 24-36 V on it and killed three boards. See README.
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
  THE SIX STATES
  ---------------------------------------------------------------------------
    IDLE        Waiting for money. Coins accepted.
    PAID        The price is met. "KSH 20 - PAID!" for a moment before the
                motor starts. Slot already shut.
    DISPENSING  Motor turning, watching the beam. Slot shut.
    DELIVERED   Pad fell, "PLEASE COLLECT YOUR PAD" on screen. Slot shut.
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
    MESSAGE_HOLD_MS       how long "PLEASE COLLECT YOUR PAD" stays up

  The first three can be changed live from the console, so you can find the
  right number without reflashing, then write it into SECTION 5. See the
  detailed notes on each one there.
*/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <Preferences.h>

// The service phone number is kept out of git. It lives in service_contact.h
// next to this sketch, which .gitignore excludes; copy
// service_contact.example.h to create it. Without it the placeholder below is
// used and the boot self-test warns.
#if __has_include("service_contact.h")
#include "service_contact.h"
#endif
#ifndef SERVICE_CONTACT_TEXT
#define SERVICE_CONTACT_TEXT "07XX XXX XXX"
#endif

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
//                      why the stock switch is not up there.
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
// On 13 rather than 33 purely because 13 lands next to the terminal block on
// this build. Both are equally valid: plain GPIOs, no strapping, no flash, and
// both have the internal pull-up a switch-to-ground needs. 13 is ADC2, which
// does not matter here because this pin is only ever read digitally.
const uint8_t PIN_PADS = 13;    // stock limit switch to GND (INPUT_PULLUP)

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
    {1, 5, "KSH 5"},
    {5, 10, "KSH 10"},
    {10, 20, "KSH 20"}, // two 20s taught as one channel, or delete
    {20, 40, "KSH 40"}, // delete any line you have not taught
    // Measured on the raw waveform with console  P  (2026-10-01): each pulse
    // is 50 ms LOW then 105 ms HIGH. An earlier table of 2 / 10 / 20 / 40 was
    // exactly double, because every pulse was being counted twice; see
    // COIN_PULSE_DEBOUNCE_MS.
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
//  COST. One write per coin plus one on the vend. With KSh 5 coins and a KSh 20
//  pad that is FIVE writes for a single sale, not three — the smallest coin in
//  the table sets the worst case, so re-check this if you add a smaller one.
//  Call it 90,000 writes a year at fifty sales a day.
//
//  That is still not a wear problem, but for the right reason rather than the
//  one first claimed here: NVS packs many writes into a flash page before it
//  erases anything, and wear-levels those erases across the partition, so the
//  flash sees far fewer erase cycles than the firmware does writes. Decades,
//  comfortably. Redundant writes are skipped in setCredit() by comparing
//  against a shadow of what is actually stored — Preferences does not do that
//  for you.
const bool REMEMBER_CREDIT_ACROSS_POWER_LOSS = true;

// What to do when a recovered balance already covers a whole pad.
//
//  true  = hold it and wait for a person. The machine says the pad is paid
//          for and asks for a press.
//  false = dispense it immediately on boot.
//
//  WHY HOLDING IS BETTER. Power comes back at three in the morning, the
//  machine wakes up owing a pad, and drops one into an empty washroom. Nobody
//  is there. It lies in the tray until whoever opens up finds it, or it walks.
//  Dispensing to nobody is not honouring the debt, it is losing a pad on top
//  of the money.
//
//  Held, the pad stays in the coil where it is safe until the next person
//  touches the machine — a button press, or a coin, which equally means
//  somebody is standing there. That is not the customer who paid either, but
//  she was gone the moment the power went, and at least the pad reaches a
//  person instead of a floor.
const bool HOLD_RECOVERED_PAD = true;

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
// true  = the relay module energises when its input is driven HIGH. This is
//         the same module as the motor relay, and what is fitted.
// false = energises when pulled LOW. Do not go back to one: an ESP32's 3.3 V
//         cannot turn a 5 V active-LOW module OFF, so it energises and never
//         releases. That is why it was replaced.
// IMPORTANT: the pull resistor on PIN_INHIBIT follows this setting. Active HIGH
// needs a 10k pull-DOWN to GND (the same as the motor's GPIO 26); active LOW
// would need a pull-UP. Get it backwards and the relay energises through the
// boot window, the NC contact opens, and the slot is live on every reset.
const bool INHIBIT_RELAY_ACTIVE_HIGH = true;

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
const bool PAD_SWITCH_FITTED = true;

// --- RETRY BUTTON ---
//
//  This is a CUSTOMER control, on the front where she can reach it, not an
//  attendant control behind a lock. The reason is the money: by the time a
//  vend fails, her coin is already in the cash box and the machine owes her a
//  pad. Making her find a member of staff to get what she has paid for is the
//  wrong answer when the machine can simply try again.
//
//  Held for CLEAR_BTN_HOLD_MS it clears the fault AND KEEPS HER CREDIT, so the
//  machine dispenses on the next pass. A hold rather than a tap so that
//  brushing past it does nothing.
//
//  How many times she may retry before it stops listening. A jam that survives
//  this many attempts is not going to clear itself, and continuing just works
//  the coil against whatever is stuck. At that point it is genuinely a staff
//  problem: the console  x  still clears it, and so does a power cycle.
//  Reset by any successful delivery.
const uint8_t MAX_BUTTON_RETRIES = 3;

// What the screen tells her once the retries are gone and it really does need
// a person. Put the actual number that is printed on the machine here — a
// screen that says GET HELP without saying who to get is no help at all.
//
// SIXTEEN CHARACTERS MAXIMUM. The display is 16 x 2 and anything longer is
// silently cut off, which is worse than a short message.
//                                 |<-- 16 chars max -->|
const char *SERVICE_CONTACT      = SERVICE_CONTACT_TEXT; // set in service_contact.h

// --- RETRY BUTTON ----------------------------------------------------------
// Momentary NO button between PIN_CLEAR and GND. Mount it on the FRONT, where
// a customer can reach it: see the note above MAX_BUTTON_RETRIES. A press also
// wakes a sleeping screen.
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
//    MESSAGE_HOLD_MS          how long "PLEASE COLLECT YOUR PAD" stays up
//    MOTOR_LIMIT_MARGIN_MS    the backstop that can never be reached normally
//    PAD_SWITCH_DEBOUNCE_MS   noise filter on the stock switch
//    IR_STUCK_WARN_MS         nag about something sitting in the chute
//

// ---------------------------------------------------------------------------
//  COIN_PULSE_DEBOUNCE_MS — noise filter on each individual coin pulse
// ---------------------------------------------------------------------------
//  WHAT        After a counted pulse, ignore every falling edge for this
//              long. Two falling edges closer together than this are one
//              pulse. This is NOT the gap between coins — that is COIN_GAP_MS.
//
//  WHY 75 AND NOT 20 — measured, and it cost real money to find.
//              The interrupt is set to FALLING, but on the ESP32 a rising
//              edge can also fire it: a glitch of a few microseconds on the
//              way up is enough. This acceptor's pulse is 50 ms LOW then
//              105 ms HIGH, so that false count lands exactly 50 ms after the
//              real one, where a 20 ms debounce does not reach it.
//
//              With the coin wired straight to the pin, EVERY rising edge fired
//              it, so every coin read exactly double and the coin table was
//              calibrated to the doubled numbers (2 / 10 / 20 / 40). The PC817
//              changed the edge shape and the false count became occasional:
//              a KSh 10 then read anywhere from 7 to 10, and coins were taken
//              and not credited. Console  P  recorded the raw waveform and
//              showed 5 clean pulses where the interrupt had counted 9.
//
//              75 ms sits between the false count (50 ms after a real one) and
//              the next real pulse (155 ms after it): 25 ms of margin on one
//              side, 80 ms on the other.
//  TOO SHORT   The false count on the rising edge comes back: coins read high
//              and at random.
//  TOO LONG    Real pulses get swallowed. At 155 ms or more, every second real
//              pulse is lost.
//  HOW TO SET  Run  P  and drop a coin. Set this between the LOW time and the
//              full LOW+HIGH period, with room on both sides. If the acceptor's
//              pulse speed setting is ever changed, measure again.
//  CONSOLE     none — needs a reflash
const uint32_t COIN_PULSE_DEBOUNCE_MS = 75;

// ---------------------------------------------------------------------------
//  COIN_GAP_MS — the silence that means "that coin is finished, count it"
// ---------------------------------------------------------------------------
//  WHAT        One coin is a burst of pulses. This is how long the line must
//              stay quiet before the firmware decides the burst has ended and
//              adds up what it counted. A KSh 40 coin is 20 pulses, 155 ms
//              apart, so its burst runs for about three seconds; this matters
//              more here than on a machine that only takes one coin type.
//  TOO SHORT   A single coin gets split in two. A KSh 20 (10 pulses) reads as
//              1 + 9: the first part is priced as KSh 5, the rest is unknown,
//              and the customer is short-changed by 15 shillings.
//  TOO LONG    Two coins inserted quickly get merged into one burst. Two KSh 5
//              coins become one 4-pulse "coin" that is in no table, and the
//              money is gone. The acceptor cannot physically deliver two coins
//              closer than about 600 ms, so that is your hard ceiling.
//  HOW TO SET  Drop one of each coin and read the PULSE TIMING line. Take the
//              WIDEST gap you see and roughly double it — that is your floor.
//              Then insert two coins as fast as the acceptor will take them
//              and confirm they still read as two. Anywhere comfortably
//              between those results is right. On this acceptor pulses are
//              155 ms apart (measured with  P ), so the floor is about 310 ms;
//              400 ms leaves margin below the 600 ms ceiling. The old 250 ms
//              only worked while the rising edge was also being counted,
//              which halved the apparent gap.
//  CONSOLE     g<ms>   e.g.  g350
uint32_t COIN_GAP_MS = 400;

// ---------------------------------------------------------------------------
//  DISPENSE_TIMEOUT_MS — how long to wait before deciding NOTHING DROPPED
// ---------------------------------------------------------------------------
//  This is the one to get right. It decides whether a perfectly good machine
//  starts throwing faults at people.
//
//  WHAT        The motor starts and the firmware watches the IR beam across
//              the chute. If a pad breaks that beam the motor stops and the
//              screen says PLEASE COLLECT. If nothing breaks it within this
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
//  CONSOLE     o<ms>   e.g.  o5000
//
//  7000 IS A PLACEHOLDER. It is not measured and must not be treated as if it
//  were.
//
//  Every timing in the log so far came from a finger being waved at the IR
//  sensor to mimic a pad, because the chute and panel do not exist yet. Those
//  numbers measure how fast somebody moves their hand. They say nothing about
//  how long a pad takes to fall, and the 3503 ms "slow drop" that this value
//  was derived from was a slow finger.
//
//  7000 is deliberately generous so bench work is not interrupted by faults
//  that mean nothing. It errs the safe way: too long only wastes a few
//  seconds on a real jam, while too short tells a paying customer the machine
//  is broken when it is not.
//
//  SET IT PROPERLY once pads fall down a real chute:
//    1. Load the coil and run twenty or thirty vends.
//    2. Read  slowest drop  from  s  — the firmware tracks it for you.
//    3. Set this to about twice that, with  o<ms>, then write it in here.
//  Status warns whenever the timeout drops under twice the slowest drop it
//  has actually seen, so the machine will tell you when this number is wrong.
uint32_t DISPENSE_TIMEOUT_MS = 7000;

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
//              gets nothing while the screen cheerfully says PLEASE COLLECT.
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
//  MESSAGE_HOLD_MS — how long "PLEASE COLLECT YOUR PAD" stays on the screen
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
//  THE SALE, SCREEN BY SCREEN — how long each step is held
// ---------------------------------------------------------------------------
//  The machine says what it understood at every step instead of jumping
//  straight from a coin to a running motor:
//
//    a coin, short     THAT IS KSH 5!   / TOTAL KSH 5       COIN_ACK_MS
//    then              ADD KSH 15 MORE  / TIME LEFT 1:58    until next coin
//    price met         KSH 20 - PAID!   / A FULL PAD!       PAID_HOLD_MS
//    (KSh 40)          KSH 40 - PAID!   / 2 PADS COMING!
//    motor             HERE IT COMES... / (PAD 1 OF 2)      until it drops
//    delivered         PLEASE COLLECT   / YOUR PAD :)       MESSAGE_HOLD_MS
//    next of two       ONE MORE COMING  / PAD 2 OF 2        NEXT_PAD_HOLD_MS
//    done              THANK YOU!       / HAVE A GOOD DAY   THANKS_MS
//
//  None of these blocks. They are deadlines checked by loop(), so the beam and
//  the coin line are watched throughout.
//
//  PAID_HOLD_MS IS ALSO A MONEY RULE, not only a pause to read. The slot shuts
//  the instant the price is met, but a coin already rolling through the
//  acceptor cannot be stopped. Its pulses must finish arriving BEFORE the
//  motor starts, because the coin line is muted while the coil turns and a
//  burst caught by the mute is thrown away: coin in the box, nothing credited.
//  So the motor waits for the hold AND for the coin line to be quiet, up to
//  PAID_MAX_WAIT_MS (a KSh 40 is about three seconds of pulses).
//  CONSOLE     none — needs a reflash
const uint32_t COIN_ACK_MS = 1500;
const uint32_t PAID_HOLD_MS = 2000;
const uint32_t NEXT_PAD_HOLD_MS = 1000;
const uint32_t PAID_MAX_WAIT_MS = 8000;
const uint32_t THANKS_MS = 2000;

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
//  MOTOR_NOISE_SETTLE_MS — how long the coin line stays untrusted after the
//                          motor stops
// ---------------------------------------------------------------------------
//  WHAT        The coin input is not counted while the motor is running, nor
//              for this long after it stops. Switching the relay and the coil
//              induces edges on GPIO 18 that look exactly like coin pulses —
//              on this machine, bursts of one or two with 21-50 ms gaps,
//              arriving the instant the motor starts or stops. A real coin
//              from this acceptor has 50-105 ms gaps and lasts seconds.
//
//              This is a HARDWARE problem and the flyback diode, the
//              capacitors and star wiring are the cure. Muting the input is
//              what stops the firmware spending money that was never inserted
//              while you fix it.
//  TOO SHORT   The turn-off spike still lands in a counted window. The relay
//              de-energising is the worst moment, not the start.
//  TOO LONG    A real coin inserted just after a vend is missed. The acceptor
//              needs about 600 ms per coin anyway, so there is room here.
//  HOW TO SET  Leave it at 500 unless the console reports discarded pulses on
//              every vend, which means the spike outlasts it. Status counts
//              them, so you can tell suppression working from suppression
//              missing.
//  CONSOLE     none — needs a reflash
const uint32_t MOTOR_NOISE_SETTLE_MS = 500;

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
//  INHIBIT_SETTLE_MS — how long the stock switch is not believed after the
//                      coin-inhibit relay moves
// ---------------------------------------------------------------------------
//  WHAT        A blackout window. For this long after the inhibit relay is
//              driven to a new level, the stock switch is not sampled at all,
//              and its debounce clock is restarted so a change still has to
//              hold for PAD_SWITCH_DEBOUNCE_MS AFTER the window closes.
//
//  WHY IT EXISTS — this is a real fault, seen on this bench, not a precaution.
//              The machine sat in this loop, forever, a few times a second:
//
//                relay energises  ->  stock line reads EMPTY
//                enterEmpty()     ->  relay releases
//                relay releases   ->  stock line reads PADS PRESENT
//                back in service  ->  relay energises  ->  ...
//
//              The log gives the causation away by its ordering: every single
//              stock change landed immediately after a relay line, never on
//              its own. A switch under a stack of pads does not change state
//              in lockstep with a relay coil. The relay was moving the stock
//              line — through the rail, the ground, or coupling between the
//              two runs of wire.
//
//              It is not a cosmetic fault. Hammering a relay coil several
//              times a second reset the board twice: once TG1WDT_SYS_RESET
//              (interrupt watchdog) and once a Guru Meditation double
//              exception part-way through printing the self-test.
//
//  THE REAL FIX IS PHYSICAL. Separate the stock switch run from the relay
//              wiring, give the relay module its own supply decoupling, and
//              make the grounds meet at one point. This constant stops the
//              firmware amplifying the problem into a reset loop; it does not
//              make the coupling go away.
//
//  TOO SHORT   The oscillation comes back — the induced reading outlasts the
//              window and gets believed.
//  TOO LONG    A genuine refill or a genuine empty coil takes this long plus
//              PAD_SWITCH_DEBOUNCE_MS to be noticed. At 600 ms that is about
//              a second in the worst case, which nobody can perceive.
//  CONSOLE     none — needs a reflash
const uint32_t INHIBIT_SETTLE_MS = 600;

// ---------------------------------------------------------------------------
//  IR_STUCK_WARN_MS — how long a blocked beam at rest is tolerated
// ---------------------------------------------------------------------------
//  WHAT        If the beam reads BLOCKED for this long while the machine sits
//              idle, it goes to FAULT "CHECK SENSOR" and shuts the slot, so
//              nobody pays into a machine that cannot see a pad drop. That is
//              what an unplugged sensor looks like (with the 1 M pull-down
//              on GPIO 27, see WIRING.md), as well as a pad stuck in the chute.
//              It used to be a console warning only, and the beam was not
//              even read while idle, so an unplugged sensor went unnoticed.
//  TOO SHORT   A hand collecting a pad, or reaching into the chute, trips it.
//  TOO LONG    A dead sensor sits with the slot open for that long.
//  CONSOLE     none — needs a reflash
const uint32_t IR_STUCK_WARN_MS = 5000;

// ---------------------------------------------------------------------------
//  BEAM_CLEAR_RECOVER_MS — how long the beam must read clear to undo that
// ---------------------------------------------------------------------------
//  WHAT        A CHECK SENSOR or CHUTE BLOCKED fault caused by a blocked beam
//              clears ITSELF once the beam has read clear for this long: plug
//              the sensor back in, or take the pad out, and the machine goes
//              back to work without anyone holding the button. A boot halt for
//              a blocked chute re-runs the whole self-test first.
//              Jams (no pad within the dispense timeout) still latch: the beam
//              is clear during a jam, so it proves nothing there.
//  CONSOLE     none — needs a reflash
const uint32_t BEAM_CLEAR_RECOVER_MS = 3000;

// ---------------------------------------------------------------------------
//  SOAK TEST  (console command  T<n> )
// ---------------------------------------------------------------------------
//  How long a soak cycle waits for a pad before calling it a failure.
//
//  This is deliberately far longer than DISPENSE_TIMEOUT_MS, and that is the
//  whole point. You cannot measure the slowest drop with a stopwatch that
//  stops before it: a 6 s drop under a 5 s timeout is recorded as "no pad",
//  and the number you are hunting never appears. Keep this generous, find the
//  real distribution, then set the real timeout from it.
//
//  Like  m<ms> , a soak blocks, so the loop() motor backstop is not running
//  while it does. This ceiling is the only limit on one cycle. That is fine
//  for a command an operator types while watching the machine and wrong for
//  anything automatic, which is why nothing automatic uses it.
const uint32_t SOAK_MAX_WAIT_MS = 15000;

//  Pause between cycles: lets the pad clear the beam, the coil settle, and
//  gives you time to watch what the mechanism is actually doing.
const uint32_t SOAK_GAP_MS = 2000;

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
// SOUND_ON — the machine's sounds, on or off. The client's decision
// (IMPROVEMENTS_BACKLOG.md, "Questions for Rebecca at commissioning", Q5).
//   true  = coin, reject, vend and fault tones, and the boot chirp
//   false = silent; the screen still says everything. The console's  b
//           still chirps, so the buzzer can be checked on the bench.
const bool SOUND_ON = true;

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

// Does the BOOT output print — the banner and the nine-part self-test?
//
//  true  = yes. What you want on the bench and during commissioning.
//  false = silent boot. The self-test still RUNS and still gates the machine
//          exactly as before; it just does not narrate. Nothing is skipped
//          and nothing is weakened.
//
//  SERIAL_LOG_DEFAULT alone does not give you this. The banner and self-test
//  are deliberately forced on in setup() regardless of it, so a field machine
//  set quiet was still printing about a second of text at every power-up.
//  This is the switch for that.
//
//  A FAILING SELF-TEST STILL SHOUTS. If the check fails with this off, it is
//  run again with logging forced on so the reason reaches the console. Silent
//  when healthy, loud when broken — a machine that refuses to work and will
//  not say why is worse than one that chatters.
//
//  Either way  d  still prints the whole thing on demand, because a typed
//  command always answers.
const bool SERIAL_LOG_BOOT = true;

// Refuse to leave setup() if the self-test found something fatal. The machine
// shows the fault on the LCD and waits rather than pretending to work.
const bool HALT_ON_SELFTEST_FAIL = true;

// --- LCD HEALTH ------------------------------------------------------------
// How often to rebuild the display even when nothing has disturbed it.
//
//  The event triggers (motor, relay, coin) catch the three things we KNOW dip
//  the rail. This catches the ones we do not: a spike from something else on
//  the mains, a marginal connector, cable pickup, a glitch nobody diagnosed.
//
//  It is the difference between a display that recovers from known causes and
//  one that cannot stay corrupted at all. In a machine that sits in a washroom
//  for months, that is worth 50 ms a minute.
//
//  Set to 0 to disable.
const uint32_t LCD_REFRESH_MS = 60000;

// ---------------------------------------------------------------------------
//  LCD_BOOT_WAIT_MS / LCD_REPROBE_MS — finding a screen that is slow to wake
// ---------------------------------------------------------------------------
//  WHAT        At power-up the 5 V rail ramps as its 1000 uF charges, and the
//              ESP32 can run its self-test before the LCD backpack is powered
//              enough to answer on I2C. The screen used to be looked for once:
//              miss it and the machine ran the whole session with a blank
//              screen while everything else worked (seen on the bench,
//              2026-10-08, on cold power-ups but never on an EN reset).
//              Now the self-test waits up to LCD_BOOT_WAIT_MS for it, and if
//              it is still missing, loop() looks again every LCD_REPROBE_MS and
//              brings the screen up the moment it answers.
//  CONSOLE     none — needs a reflash
const uint32_t LCD_BOOT_WAIT_MS = 2000;
const uint32_t LCD_REPROBE_MS = 5000;

// How long the machine sits untouched before the backlight goes out.
//
//  The backlight is nearly all of what this display consumes — roughly 20 mA
//  against 2 mA for the logic — and it is the only part of the LCD that wears.
//  A washroom machine is idle almost all of the time, so leaving it lit buys
//  nothing and costs the backlight's life.
//
//  It is also the cleanest possible refresh. Waking ALWAYS rebuilds the
//  display from scratch, and waking happens on the first coin pulse — so the
//  screen is at its freshest at the exact moment somebody starts using it.
//  Corruption that happened while nobody was looking never gets seen.
//
//  Only plain idle sleeps. A machine showing OUT OF ORDER or SORRY - NO PADS
//  stays lit, because that message is the whole point of it being there, and
//  a machine with credit on the meter stays lit because somebody is mid-
//  transaction.
//
//  Set to 0 to keep the backlight on always.
const uint32_t BACKLIGHT_TIMEOUT_MS = 120000; // 2 minutes

// ###########################################################################
// #                 END OF CONFIG.  Below here is machinery.                #
// ###########################################################################

// ===========================================================================
//  STATE
// ===========================================================================
//
//  The machine is always in exactly one of six states, and the state decides
//  two things: what is on the screen, and whether the coin acceptor is live.
//
//    ST_IDLE        Waiting for money. Coins accepted. Leaves here when
//                   credit reaches the price (-> PAID), or when the stock
//                   switch says the coil is empty.
//
//    ST_PAID        The price is met. The slot shuts at once, "KSH 20 -
//                   PAID!" holds for PAID_HOLD_MS, and the motor waits until
//                   the coin line is quiet so a coin already inside the
//                   acceptor is counted before the mute starts. -> DISPENSING.
//
//    ST_DISPENSING  Motor turning, watching the beam. Slot shut. Ends when a
//                   pad breaks the beam (-> DELIVERED) or when
//                   DISPENSE_TIMEOUT_MS runs out (-> EMPTY or FAULT).
//
//    ST_DELIVERED   Pad fell. "PLEASE COLLECT YOUR PAD" holds for
//                   MESSAGE_HOLD_MS, then back to IDLE: straight on to PAID if
//                   the credit buys another pad, else THANK YOU and the slot
//                   reopens.
//
//    ST_EMPTY       Stock switch says no pads. COINS INHIBITED. NOT a fault
//                   and NOT latched: it returns to IDLE on its own the moment
//                   the switch says pads are back, so a refill is all it takes
//                   and nobody has to drive out with a key.
//
//    ST_FAULT       Something is wrong that needs a person: a jam, a sensor
//                   reading wrong, or a failed self-test. COINS INHIBITED.
//                   LATCHED — it stays here until the clear button is held for
//                   CLEAR_BTN_HOLD_MS or  x  is typed, and even then it
//                   refuses while the beam still reads blocked.
//
//  THE RULE THE WHOLE DESIGN RESTS ON: the coin slot is live only in IDLE, and
//  only while the credit is short of a pad. It is derived in one line of
//  loop() and written nowhere else, so the slot can never disagree with what
//  the machine is doing.
//
//  It used to stay open during a vend, on the reasoning that a coin landing
//  mid-vend is banked. That stopped being true when the coin line was muted
//  around motor runs: a coin inserted while the coil turns has its pulses
//  binned, so the money was taken and never credited. Shutting the slot from
//  the moment the price is met until the pad is collected costs two relay
//  operations per sale (a relay is rated for 100,000+).
//
enum State
{
  ST_IDLE,
  ST_PAID,
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
  case ST_PAID:
    return "PAID";
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
//  What makes it miss a nibble is anything that dips the rail hard enough for
//  the LCD controller to brown out while the ESP32 — which has its own
//  regulator and a far better supply — carries on talking to it.
//
//  THREE THINGS ON THIS MACHINE DO THAT, and all three set this flag:
//
//    the motor        switching the coil is the biggest single dip
//    the inhibit relay  a relay coil de-energising kicks the rail
//    the acceptor     it fires a gate solenoid on EVERY coin it accepts or
//                     rejects, and it draws a surge when first powered
//
//  The acceptor is the one that catches people out, because it corrupts the
//  display on a bench with no motor connected at all — which is exactly what
//  happened on this build before the motor was even fitted.
//
//  So: after anything that kicks the rail, rebuild the display from scratch
//  before writing to it again. It costs about 50 ms, only at moments when the
//  screen was about to be repainted anyway, and it makes the symptom
//  self-clear instead of persisting until somebody power-cycles the machine.
//
//  This is a belt-and-braces measure, not a substitute for fixing the supply.
//  See README, "Funny symbols on the screen".
bool lcdNeedsReinit = false;
uint32_t lcdRefreshMs = 0;    // next scheduled rebuild, see LCD_REFRESH_MS
uint32_t lcdProbeMs = 0;      // next look for a screen missed at boot
bool backlightOn = true;      // false = asleep, see BACKLIGHT_TIMEOUT_MS
char lcdTopLine[17] = "";     // what say() last put on line 1
bool loopWdtOn = false;       // the loop watchdog is armed (end of setup)
uint32_t lastActivityMs = 0;  // last coin pulse or console command
uint32_t lastPulseTotal = 0;  // to spot a pulse the moment it arrives

// Coin pulses are counted in an ISR and decoded into whole coins in loop().
volatile uint16_t isrBurst = 0;
volatile uint32_t isrLastMs = 0;
volatile bool isrSeenOne = false;
volatile uint32_t isrTotal = 0; // every pulse ever, for diagnostics
portMUX_TYPE coinMux = portMUX_INITIALIZER_UNLOCKED;

// Gap statistics for the burst being counted right now. With a 20-pulse coin
// in the table these are the numbers that decide whether COIN_GAP_MS is safe,
// and there is no other way to see them. Reported with every coin.
// While this is true the coin interrupt counts nothing. Set the moment the
// motor starts and cleared MOTOR_NOISE_SETTLE_MS after it stops.
volatile bool coinMuted = false;
volatile uint32_t isrMutedPulses = 0; // how much noise we threw away, for status
uint32_t coinUnmuteMs = 0;

volatile uint16_t isrGapMin = 0xFFFF;
volatile uint16_t isrGapMax = 0;
volatile uint32_t isrBurstStartMs = 0;

// The coin interrupt is attached only once the self-test has passed. A machine
// that halted at boot must never return to service with this false, or the
// acceptor takes money the firmware cannot count.
bool coinIsrAttached = false;

Preferences prefs;   // holds only the meter balance
bool creditStoreOpen = false;     // did prefs.begin() ever succeed?
bool creditPersistOk = false;     // and are writes actually reaching flash?
uint32_t storeRetryMs = 0;        // next attempt to open a store that refused
bool creditPersistWarned = false; // shout about a failure once, not forever
uint16_t creditInFlash = 0;       // what we last successfully wrote

uint16_t credit = 0; // shillings on the meter
uint32_t lastCoinMs = 0;
uint32_t prevCoinMs = 0;        // the one before it, so the gap can be reported
bool holdForCollection = false; // a recovered pad, waiting for a person
uint32_t creditTickMs = 0; // next countdown repaint, see ST_IDLE
uint8_t idleMsgIdx = 0;    // which page of the idle prompt is showing
uint32_t idleRotateMs = 0; // next idle repaint
uint32_t dispenseStartMs = 0;
uint32_t messageUntilMs = 0;

// The sale's screens (see THE SALE, SCREEN BY SCREEN in SECTION 5).
bool bannerUp = false;       // IDLE leaves the screen alone while this is up
uint32_t bannerUntilMs = 0;  // ...until this deadline
uint32_t paidSinceMs = 0;    // when PAID began, or its hold was restarted
uint32_t paidHoldMs = 0;     // PAID_HOLD_MS, or NEXT_PAD_HOLD_MS mid-sale
uint16_t orderPads = 0;      // pads this sale is buying; 0 = no sale running
uint16_t orderIndex = 0;     // which of them is next, or being dispensed

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
uint32_t lastMotorRunMs = 0;  // how long the last run lasted
uint32_t mutedMotorMs = 0;    // total motor time since the coin line was muted
uint32_t inhibitChangedMs = 0; // when the inhibit relay was last driven to a
                               // NEW level. Two things hang off it: the stock
                               // switch is deaf for INHIBIT_SETTLE_MS after it,
                               // and the slot is never re-opened sooner than
                               // that after it was closed.
uint32_t slowestDropMs = 0;   // worst delivery this session, sets the timeout

// Counters. RAM only, and reset on every boot. The meter balance is the
// only thing that survives a power cut — see setCredit().
uint32_t vendCount = 0;
uint32_t coinsAccepted = 0;
uint32_t coinsUnknown = 0;
uint32_t faultCount = 0;
uint32_t shillingsIn = 0;
uint32_t noisePulses = 0; // induced pulses discarded, a suppression health check
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

// Milliseconds since a timestamp, clamped at zero.
//
//  THE BUG THIS EXISTS TO KILL, because it cost a whole bench session.
//
//  loop() reads `now` once at the top. Anything that stamps a timestamp with
//  millis() LATER in that same pass produces a value AHEAD of `now`. The
//  unsigned subtraction (now - stamp) then wraps to about 4.29 billion, and
//  every "has enough time passed?" test built on it fires instantly.
//
//  That is exactly what broke part payment: bankCoin() stamped lastCoinMs with
//  millis(), the credit timeout compared it against the stale `now`, and a
//  KSh 5 was cleared on the very pass it was banked. It only showed on 5 and
//  10 because 20 and 40 meet the price and vend before the timeout is reached.
//
//  So: never write (now - stamp) directly. Use sinceMs() to measure, and
//  reached(now, stamp + interval) to decide — both are correct whether the
//  stamp is behind, level with, or slightly ahead of `now`, and both survive
//  the millis() rollover as well.
static inline uint32_t sinceMs(uint32_t now, uint32_t stamp)
{
  int32_t d = (int32_t)(now - stamp);
  return (d > 0) ? (uint32_t)d : 0;
}

static inline uint32_t motorHardLimitMs()
{
  return DISPENSE_TIMEOUT_MS + MOTOR_LIMIT_MARGIN_MS;
}

// attemptClear() is defined above both of these but has to call them.
bool selfTest(bool verbose);
void attemptClear(const char *who, bool fromButton);
void attachCoinIsr();
static bool vendInFlight();
void repaintForState();
void wakeDisplay();
void sleepDisplay();
void soakTest(uint16_t cycles);
bool chuteClear();
bool ensureCreditStore();

// Debounced pad stock switch
bool padsRaw = true;
bool padsStable = true;
uint32_t padsChangedMs = 0;
bool stockWatch = false; // console command  k

// Stock-switch flap detector. A coil being refilled changes state once or
// twice. A line picking up the relay changes state over and over, and that
// pattern is worth shouting about because it is the one that resets the board.
uint16_t stockFlaps = 0;        // settled changes inside the current window
uint32_t stockFlapWindowMs = 0; // when the current 60 s window started
bool stockFlapWarned = false;   // warn once per window, not once per flap
uint32_t emptyRotateMs = 0;
uint8_t emptyMsgIdx = 0;

// added section on purpose of disabling the coin acceptor when jammed
bool coinsAllowed = false; // starts inhibited, on purpose
bool clearBtnRaw = false;
bool clearBtnStable = false;
uint32_t clearBtnChangedMs = 0;
uint32_t clearBtnDownMs = 0;
bool clearBtnHandled = false;
uint8_t buttonRetries = 0;     // consecutive button clears with no pad delivered
uint32_t lastRefusalMs = 0;    // rate-limit the out-of-retries message
bool serviceCycleSeen = false; // stock switch went EMPTY during a fault
char faultLine2[17] = "";
bool faultClearsWhenBeamClear = false; // a beam fault, undone by a clear beam
uint32_t beamClearSinceMs = 0;         // when the beam last turned clear in FAULT
bool beamClearTiming = false;
uint8_t faultMsgIdx = 0;
uint32_t faultRotateMs = 0;

// ===========================================================================
//  ISR — counting coin pulses without missing any
// ===========================================================================
//
//  HOW A MULTI-COIN ACCEPTOR TALKS
//  It does not say "that was twenty shillings". It pulls one line LOW a fixed
//  number of times, quickly, and the NUMBER OF PULSES identifies which coin it
//  was. On this machine: 1 pulse = KSh 5, 5 = KSh 10, 10 = KSh 20,
//  20 = KSh 40. See SECTION 2.
//
//  So reading a coin is two jobs, and they are split in two on purpose:
//
//    onCoinPulse()  runs in an interrupt, on the falling edge of every pulse.
//                   It does the absolute minimum — count, timestamp, return —
//                   because an interrupt that dawdles loses the next pulse.
//                   It also debounces, so one pulse is never counted twice,
//                   including the false FALLING interrupt an ESP32 can fire on
//                   the pulse's rising edge (COIN_PULSE_DEBOUNCE_MS).
//
//    takeCoin()     runs in loop(). It watches for the line to go quiet for
//                   COIN_GAP_MS, which is what "that coin is finished" looks
//                   like, and only then hands the finished count over to be
//                   priced.
//
//  It also measures the gaps between pulses as it goes, because with a
//  20-pulse coin those numbers are the only way to know whether COIN_GAP_MS
//  has enough margin on both sides. They print with every coin.
//
void IRAM_ATTR onCoinPulse()
{
  uint32_t now = millis();
  portENTER_CRITICAL_ISR(&coinMux);

  // THE COIN LINE IS NOT TRUSTED AROUND A MOTOR RUN.
  //
  //  Switching the relay and the coil induces edges on this pin that are
  //  indistinguishable from coin pulses at the input, and they arrive in ones
  //  and twos — which is exactly the pulse count of the cheapest coin in the
  //  table. Counted, they become credit nobody paid, and credit nobody paid
  //  becomes pads nobody paid for.
  //
  //  Inhibiting the acceptor does NOT help here: that stops the mechanism
  //  taking a coin, it does nothing about induced voltage on a signal wire.
  //  The only reliable answer in firmware is to stop counting.
  if (coinMuted)
  {
    isrMutedPulses++;
    portEXIT_CRITICAL_ISR(&coinMux);
    return;
  }
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
// True while a coin's pulses are still arriving. PAID waits on this, so a
// coin already inside the acceptor when the slot shut is counted before the
// motor starts and the mute would bin it.
bool coinBurstInProgress()
{
  portENTER_CRITICAL(&coinMux);
  bool busy = isrBurst > 0;
  portEXIT_CRITICAL(&coinMux);
  return busy;
}

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
//  TWO THINGS HERE ARE NOT OBVIOUS, AND BOTH WERE WRONG ONCE.
//
//  First, Preferences does NOT skip a write when the value is unchanged. It
//  goes straight to nvs_set_u16() + nvs_commit() every time — checked in the
//  core source, not assumed. The creditInFlash shadow below is what actually
//  makes "identical writes are free" true, rather than a hope about somebody
//  else's implementation.
//
//  Second, putUShort() returns 0 when it fails, including when begin() never
//  succeeded, and ignoring that is exactly the failure this whole feature
//  exists to prevent: the machine would think a balance was safe, say so, and
//  lose it at the next power cut with nothing logged.
//
//  A failure does NOT stop the machine trading. Refusing sales because flash
//  is unhappy helps nobody — it just turns a degraded machine into a dead one.
//  What it does is say so, loudly and once, and keep saying so in status and
//  in the self-test, so the power-cut protection is never quietly absent.
void setCredit(uint16_t value)
{
  credit = value;
  if (!REMEMBER_CREDIT_ACROSS_POWER_LOSS)
    return;

  // A store that refused at boot may be available now. This also seeds
  // creditInFlash, without which the overstated-balance check below is blind.
  ensureCreditStore();

  // Nothing to write. Only trust this when the last write actually landed,
  // otherwise a retry of the same value would be skipped forever.
  if (creditPersistOk && value == creditInFlash)
    return;

  if (prefs.putUShort("credit", value) != 0)
  {
    creditInFlash = value;
    if (!creditPersistOk)
    {
      creditPersistOk = true;
      creditPersistWarned = false;
      LOGLN(F("The meter balance is being saved again."));
    }
    return;
  }

  creditPersistOk = false;

  // NEVER LEAVE THE STORED BALANCE ABOVE THE REAL ONE.
  //
  //  The two directions of error are not equally bad, and that asymmetry is
  //  the whole rule:
  //
  //    stored ABOVE the truth  the next boot over-credits. If it holds a full
  //                            pad's worth the machine wakes up, believes it
  //                            owes a pad, and vends one into an empty
  //                            washroom where it is simply lost. Part payment
  //                            hands a discount to whoever walks up next.
  //                            DANGEROUS.
  //
  //    stored BELOW the truth  the next boot under-credits. The customer
  //                            recovers part of her claim instead of all of
  //                            it, which is worse for her than a working
  //                            machine but better than nothing, and it can
  //                            never produce a pad nobody paid for. SAFE.
  //
  //  So a stored value already at or below the meter is left exactly where it
  //  is. Clearing it would destroy a claim that was saved successfully,
  //  earlier, for no safety benefit at all — somebody who put in KSh 10, had
  //  it stored, then added a second KSh 10 whose write failed would lose BOTH
  //  coins instead of keeping the one that was safe.
  //
  //  Only an overstated balance is worth writing over, and zero is the safest
  //  thing to put there. If that write fails too, nothing is lost that was not
  //  already lost.
  if (creditInFlash > value && prefs.putUShort("credit", 0) != 0)
  {
    creditInFlash = 0;
    LOGLN(F("!! The stored balance was higher than the meter, so it has been"));
    LOGLN(F("!! cleared — a reset must not restore it and vend a pad nobody"));
    LOGLN(F("!! paid for. A stored balance BELOW the meter is left alone."));
  }

  if (!creditPersistWarned)
  {
    creditPersistWarned = true;
    LOGLN();
    LOGLN(F("!! THE METER BALANCE IS NOT REACHING FLASH."));
    LOG(F("!! KSH "));
    LOG(value);
    LOGLN(F(" is on the meter in RAM only, and a power cut will"));
    LOGLN(F("!! lose it — the exact thing this is supposed to prevent."));
    LOGLN(F("!! The machine keeps trading; stopping would not help anybody."));
    LOGLN(F("!! Check the NVS partition. Status and  d  both report this."));
  }
}

// Open the credit store, or keep trying if it refused at boot.
//
//  WHY THIS RETRIES AT ALL. If prefs.begin() fails during setup() we cannot
//  read what is in NVS, so we do not know whether an old balance is sitting in
//  there. Everything downstream is then blind: the "is the stored value too
//  high" check compares against a creditInFlash of zero and can never fire, so
//  an old balance survives the whole session untouched. If NVS then opens
//  normally on some later boot, that balance is restored as gospel and can
//  vend a pad nobody paid for. The danger is not the failure, it is the
//  failure being invisible.
//
//  Note prefs.begin() returns FALSE when the store is already open, so this
//  has to track "open" separately from "writes are working" — calling begin()
//  again on a healthy store would otherwise look like a failure.
bool ensureCreditStore()
{
  if (creditStoreOpen)
    return true;
  if (!prefs.begin("padvend", false))
    return false;

  creditStoreOpen = true;

  // Same optimistic assumption setup() makes: a store that opens is presumed
  // to write until a write actually fails. Without this the alarm raised when
  // it would not open stays latched forever, and  s  and  d  keep reporting a
  // flash fault that has healed — sending somebody to chase a problem that is
  // no longer there. A diagnostic that lies is worse than no diagnostic.
  creditPersistOk = true;
  creditPersistWarned = false;

  uint16_t stored = prefs.getUShort("credit", 0);
  creditInFlash = stored;

  LOG(F("Credit store opened late. It holds KSH "));
  LOGLN(stored);

  //  ANYTHING FOUND NOW IS STALE BY DEFINITION. The boot restore has already
  //  been and gone; this balance belongs to a session we could not read. It is
  //  not restored, because hours may have passed and the machine has been
  //  trading without it. It is reconciled DOWN to the meter for the same
  //  reason as everywhere else: a stored value above the truth can vend a pad
  //  nobody paid for, one at or below it cannot.
  if (stored > credit)
  {
    if (prefs.putUShort("credit", credit) != 0)
    {
      creditInFlash = credit;
      LOGLN(F("  Stale and higher than the meter, so it has been overwritten."));
      LOGLN(F("  A later boot cannot now restore it and vend an unpaid pad."));
    }
    else
    {
      // The store opens but will not take data. That is the other failure, and
      // it is now proven rather than assumed, so say so.
      creditPersistOk = false;
      creditPersistWarned = true;
      LOGLN(F("  Stale and higher than the meter, but it will not overwrite."));
      LOGLN(F("  The store opens and refuses writes. A later boot could still"));
      LOGLN(F("  restore this and vend an unpaid pad."));
    }
  }
  return true;
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
  // millis() here is AHEAD of the `now` loop() is holding, because this runs
  // later in the same pass. Everything that reads lastCoinMs goes through
  // reached() or sinceMs() for exactly that reason — see sinceMs().
  prevCoinMs = lastCoinMs; // keep the old stamp: the gap is measured from it
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
  // Mute BEFORE the relay moves, and bin any burst already in progress — a
  // coin that was still being read when a vend started cannot be trusted now
  // that noise is about to be mixed into it.
  // A soak blocks, so loop() never runs between its cycles and the mute is
  // never released. Motor time therefore has to ACCUMULATE across every run in
  // one mute window, or the pulse count gets divided by the last cycle alone
  // and reports a rate several times too high.
  if (!coinMuted)
    mutedMotorMs = 0; // a fresh mute window starts a fresh total

  portENTER_CRITICAL(&coinMux);
  coinMuted = true;
  isrBurst = 0;
  portEXIT_CRITICAL(&coinMux);

  digitalWrite(PIN_RELAY, RELAY_ACTIVE_HIGH ? HIGH : LOW);
  motorOnSince = millis();
  motorRunning = true;
  lcdNeedsReinit = true; // the coil kick is what corrupts the display
}

void motorOff()
{
  digitalWrite(PIN_RELAY, RELAY_ACTIVE_HIGH ? LOW : HIGH);
  lastMotorRunMs = millis() - motorOnSince;
  mutedMotorMs += lastMotorRunMs;
  motorRunning = false;
  // The coil collapsing as the relay opens is the WORST moment for induced
  // pulses, worse than starting. Keep the line muted well past it.
  coinUnmuteMs = millis() + MOTOR_NOISE_SETTLE_MS;
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
//  With the shipped settings (NC contact, active-HIGH module) it works out as:
//
//      want        contact      relay      pin
//      ----------  -----------  ---------  ------
//      inhibit     closed       off        LOW
//      accept      open         on         HIGH
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
  uint32_t now = millis();

  //  ONE-WAY RATE LIMIT. Closing the slot happens on the instant, always.
  //  Re-opening it waits until INHIBIT_SETTLE_MS has passed since the relay
  //  last moved.
  //
  //  This is deliberately asymmetric, because the two directions are not
  //  equally dangerous. Blocking money a moment early costs nothing. Letting
  //  money in a moment early is how a machine takes a coin it cannot honour.
  //  So the delay only ever applies to the direction that risks the customer's
  //  money, and never to the direction that protects it.
  //
  //  It also drains the oscillation described at INHIBIT_SETTLE_MS: a relay
  //  that cannot be re-energised for 600 ms cannot chatter at several hertz,
  //  whatever the stock line does.
  //
  //  Nothing is lost by returning here. loop() calls this on every single pass
  //  from one rule, so a deferred open simply happens on a later pass — there
  //  is no queue to remember and nothing to retry by hand.
  if (allow && !coinsAllowed && !reached(now, inhibitChangedMs + INHIBIT_SETTLE_MS))
    return;

  bool contactClosed = !allow; // closed == inhibited
  bool energised = INHIBIT_CONTACT_IS_NC ? !contactClosed : contactClosed;
  bool level = (energised == INHIBIT_RELAY_ACTIVE_HIGH);
  digitalWrite(PIN_INHIBIT, level ? HIGH : LOW);
  if (allow != coinsAllowed)
  {
    coinsAllowed = allow;
    inhibitChangedMs = now; // starts the stock switch's deaf window
    // A relay coil collapsing kicks the rail. Treat the display as suspect.
    lcdNeedsReinit = true;

    // Print the ELECTRICAL intent, not just the word. "Coins inhibited" only
    // says what the firmware decided; this says what it did to the pin and
    // what it expects the relay to do about it. If the relay disagrees with
    // the last line here, the fault is in the wiring or in
    // INHIBIT_RELAY_ACTIVE_HIGH — and it is no longer a matter of opinion.
    LOG(F("  [GPIO "));
    LOG(PIN_INHIBIT);
    LOG(F(" driven "));
    LOG(level ? F("HIGH") : F("LOW"));
    LOG(F(" -> expect the relay to be "));
    LOG(energised ? F("ENERGISED") : F("RELEASED"));
    LOG(F(", contact "));
    LOGLN(contactClosed ? F("CLOSED = slot blocked]") : F("OPEN = slot live]"));
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

// SILENT MEANS THE PIN HELD HIGH, NOT LOW.
//
//  The buzzer module switches its coil with a 9012, a PNP transistor: the
//  coil is ON while the input is LOW. A passive buzzer makes no sound on
//  steady DC, so a pin left LOW between beeps is silent but drives current
//  through the coil the whole time, and the module ran warm.
//  ledcWriteTone(ch, 0), which this used to silence with, sets 0% duty: LOW.
//  So between beeps the pin is taken off the PWM channel and driven HIGH.
//
//  HIGH only turns the PNP fully off if the module's VCC is the same 3.3 V.
//  From a 5 V VCC a 3.3 V HIGH still leaves ~1.7 V across emitter-base and
//  the transistor stays partly on, so the module is powered from the ESP32's
//  3V3 pin (WIRING.md section 6).
void buzzerSilence()
{
#if ESP_ARDUINO_VERSION_MAJOR < 3
  ledcDetachPin(PIN_BUZZER);
#else
  noTone(PIN_BUZZER);
#endif
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, HIGH);
}

void buzzerInit()
{
#if ESP_ARDUINO_VERSION_MAJOR < 3
  ledcSetup(BUZZER_LEDC_CH, 2000, 8);
#endif
  buzzerSilence();
}

// Plays whatever SOUND_ON says. Use toneOut() only for the bench test.
void toneOut(uint16_t hz, uint16_t ms);
void beep(uint16_t hz, uint16_t ms)
{
  if (SOUND_ON)
    toneOut(hz, ms);
}

void toneOut(uint16_t hz, uint16_t ms)
{
#if ESP_ARDUINO_VERSION_MAJOR < 3
  ledcAttachPin(PIN_BUZZER, BUZZER_LEDC_CH); // back onto the PWM channel
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
  buzzerSilence();
}

// THE LOOP WATCHDOG.
//
//  If loop() ever stops coming round (a hang, a lock-up, an endless wait),
//  the coin interrupt keeps counting but nothing banks the coins: money in the
//  box, nothing credited, nothing vended, for as long as the hang lasts. The
//  ESP32's task watchdog restarts the chip instead. enableLoopWDT() at the end
//  of setup() signs loop() up; the core checks in after every pass; and if a
//  pass takes longer than CONFIG_ESP_TASK_WDT_TIMEOUT_S (5 s in this core,
//  with panic-and-reset enabled), the chip resets. The next boot reports
//  "task watchdog". Through the restart the slot is shut (NC relay, pull-down)
//  and any banked credit comes back from flash.
//
//  Anything that waits ON PURPOSE for longer than that (the soak, the coin
//  probe, a long motor run, paged one-shot messages) must call
//  feedWatchdog() inside its wait. Console  H  hangs the loop deliberately to
//  prove the watchdog works.
void feedWatchdog()
{
  if (loopWdtOn)
    esp_task_wdt_reset();
}

// A delay that still lets the buzzer stop on time. Use this, never delay(),
// anywhere a note might still be sounding.
void holdFor(uint32_t ms)
{
  uint32_t t0 = millis();
  while (millis() - t0 < ms)
  {
    buzzerService();
    feedWatchdog();
    delay(1);
  }
  buzzerService();
}

bool i2cPresent(uint8_t addr)
{
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

// Re-sync the display's 4-bit interface after a supply dip, in ~15 ms.
//
//  WHY NOT lcd.init(). The library's init() is its power-up routine and waits
//  delay(50) + delay(1000) before it sends anything. Called from say() after
//  every motor start, relay switch and coin, that froze loop() for about a
//  second each time (loop max read 1.3 s and 2.2 s on the bench). The worst
//  case was the vend: the dispensing screen follows motorOn(), so for the first
//  second of every vend loop() was not watching the IR beam, and a pad falling
//  in that second would have been missed.
//
//  The display here is already powered; a dip only knocks the 4-bit interface
//  out of step. The HD44780's own software reset (datasheet fig. 24: 0x3 three
//  times, then 0x2) puts it back in step without the power-up wait. The first
//  nibbles have to be written raw, because the library only exposes whole
//  commands, so they go straight to the PCF8574 on the backpack:
//      P0 = RS, P1 = RW, P2 = E, P3 = backlight, P4-P7 = D4-D7
//  The full lcd.init() is still used once, at boot, where the wait is real.
void lcdRawNibble(uint8_t nibbleHigh)
{
  // RS=0 RW=0: a command. The backlight bit follows backlightOn, so a re-sync
  // in the dark does not flash the light.
  uint8_t b = (nibbleHigh & 0xF0) | (backlightOn ? LCD_BACKLIGHT : LCD_NOBACKLIGHT);
  Wire.beginTransmission(LCD_I2C_ADDR);
  Wire.write(b | 0x04); // E high
  Wire.endTransmission();
  delayMicroseconds(1);
  Wire.beginTransmission(LCD_I2C_ADDR);
  Wire.write(b); // E low: the nibble is latched
  Wire.endTransmission();
  delayMicroseconds(50);
}

void lcdResync()
{
  lcdRawNibble(0x30);
  delayMicroseconds(4500);
  lcdRawNibble(0x30);
  delayMicroseconds(4500);
  lcdRawNibble(0x30);
  delayMicroseconds(150);
  lcdRawNibble(0x20); // now in 4-bit mode; whole commands work from here
  lcd.command(LCD_FUNCTIONSET | LCD_4BITMODE | LCD_2LINE | LCD_5x8DOTS);
  lcd.display();
  lcd.clear();
  lcd.command(LCD_ENTRYMODESET | LCD_ENTRYLEFT | LCD_ENTRYSHIFTDECREMENT);
  lcd.home();
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
    lcdResync(); // ~15 ms; NOT lcd.init(), which blocks for over a second
    // Restore the light to what it should be, not always ON: a rebuild while
    // the backlight is asleep used to light it again and leave backlightOn
    // false, so the sleep timer never fired again and it stayed lit.
    if (backlightOn)
      lcd.backlight();
    else
      lcd.noBacklight();
  }
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(l1);
  lcd.setCursor(0, 1);
  lcd.print(l2);
  strncpy(lcdTopLine, l1, sizeof(lcdTopLine) - 1);
  lcdTopLine[sizeof(lcdTopLine) - 1] = 0;
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

// Break a long line into 16-character pages, on word boundaries.
//
//  WHY THIS EXISTS. Sixteen columns is not a reason to write in abbreviations.
//  "HOLD BTN 1 SEC" is what you get from squeezing a sentence into the
//  hardware; "HOLD THE BUTTON / FOR 1 SECOND / TO TRY AGAIN" across three
//  frames is the same hardware used properly, and a stranger in a hurry can
//  actually read it.
//
//  Copies page `page` into `out` and returns how many pages there are, so the
//  caller can cycle. Words are never split unless a single word is longer than
//  the display, in which case it is cut — nothing else can be done with it.
//
//  '|' FORCES A PAGE BREAK, and every customer message uses it. Left to fill
//  pages word by word, "... - NO COINS ARE BEING TAKEN" split as
//  "SEE SERIAL - NO" / "COINS ARE BEING" / "TAKEN", and the last two pages
//  read as the exact opposite. Each page has to make sense on its own,
//  because a passer-by may only catch one of them. Text between two '|' that
//  is still longer than 16 is word-wrapped as before.
uint8_t lcdPage(const char *text, uint8_t page, char *out, size_t n)
{
  uint8_t pages = 0;
  size_t i = 0, len = strlen(text);
  out[0] = 0;

  while (i < len)
  {
    while (i < len && (text[i] == ' ' || text[i] == '|'))
      i++; // a page never starts on a space or a break
    if (i >= len)
      break;

    size_t segEnd = i; // this page can run no further than the next '|'
    while (segEnd < len && text[segEnd] != '|')
      segEnd++;
    size_t remaining = segEnd - i;
    size_t take = remaining;
    if (remaining > 16)
    {
      take = 16;
      if (text[i + 16] != ' ')
      {
        // Back off to the last space that fits, so a word stays whole.
        size_t b = 16;
        while (b > 0 && text[i + b] != ' ')
          b--;
        if (b > 0)
          take = b;
      }
    }

    if (pages == page)
    {
      size_t c = (take < n - 1) ? take : n - 1;
      memcpy(out, text + i, c);
      out[c] = 0;
    }
    i += take;
    pages++;
  }
  return pages ? pages : 1;
}

// Hold l1 still on the top line and cycle l2's pages underneath it, one per
// call. The static top line gives the reader an anchor while the rest moves.
// The top line of every screen that gives the service number.
//
//  The number sits on line 1, which holds still, while the message pages
//  along line 2. So it stays readable for as long as the screen is up, long
//  enough to write down, and it is never split across pages. It used to be
//  inside the paged sentence: two seconds a page, and because pages break on
//  spaces, "07XX XXX XXX" could land as "PLEASE CALL 07XX" then "XXX XXX"
//  depending on the credit shown before it.
//
//  "CALL " + number if it fits in 16, else "TEL " + number, else the number
//  alone (the self-test already refuses one over 16).
const char *callLine()
{
  static char buf[17];
  size_t n = strlen(SERVICE_CONTACT);
  if (n + 5 <= 16)
    snprintf(buf, sizeof(buf), "CALL %s", SERVICE_CONTACT);
  else if (n + 4 <= 16)
    snprintf(buf, sizeof(buf), "TEL %s", SERVICE_CONTACT);
  else
    snprintf(buf, sizeof(buf), "%s", SERVICE_CONTACT);
  return buf;
}

// When the top line is already on screen, only line 2 is rewritten: no
// clear, half the I2C traffic, and no blink every two seconds. A pending
// rebuild (lcdNeedsReinit) still repaints both lines.
void sayPaged(const char *l1, const char *l2, uint8_t &page)
{
  char buf[17];
  uint8_t pages = lcdPage(l2, page, buf, sizeof(buf));
  if (!lcdNeedsReinit && strcmp(l1, lcdTopLine) == 0)
    sayLine2(buf);
  else
    say(l1, buf);
  page = (uint8_t)((page + 1) % pages);
}

// Show every page of a long line once, holding each. For one-shot messages —
// a rejected coin, a blocked chute — which have no rotation to live in.
void sayAllPages(const char *l1, const char *l2, uint32_t msPerPage)
{
  char buf[17];
  uint8_t pages = lcdPage(l2, 0, buf, sizeof(buf));
  for (uint8_t i = 0; i < pages; i++)
  {
    lcdPage(l2, i, buf, sizeof(buf));
    say(l1, buf);
    holdFor(msPerPage);
  }
}

// What this machine takes, built from COIN_TABLE so it can never go stale.
// Edit SECTION 2 and the screen follows without anybody remembering to.
char acceptedCoins[96] = "";

void buildAcceptedCoins()
{
  char *w = acceptedCoins;
  size_t left = sizeof(acceptedCoins);
  int n = snprintf(w, left, "INSERT A COIN - THIS MACHINE TAKES KSH");
  if (n < 0 || (size_t)n >= left)
    return;
  w += n;
  left -= n;

  for (uint8_t i = 0; i < COIN_TABLE_LEN; i++)
  {
    const char *sep = (i == 0) ? " " : (i + 1 == COIN_TABLE_LEN) ? " OR " : " ";
    n = snprintf(w, left, "%s%u", sep, COIN_TABLE[i].value);
    if (n < 0 || (size_t)n >= left)
      return;
    w += n;
    left -= n;
  }
}

// Build the "what to add, and how long you have" line for part payment.
//
//  With a timeout:     ADD 10 IN 1:35
//  Waiting forever:    ADD KSH 10 MORE
// Line 2 of the part-payment screen. Line 1 already says "ADD KSH 15 MORE",
// so this is the clock, or the total so far when part payment never expires.
void creditPromptLine(char *buf, size_t n, uint32_t now)
{
  if (CREDIT_TIMEOUT_MS == 0)
  {
    snprintf(buf, n, "TOTAL KSH %u", credit);
    return;
  }

  // sinceMs(), not (now - lastCoinMs): bankCoin() stamps lastCoinMs with
  // millis(), which can be a hair ahead of the loop's `now`. A raw subtraction
  // would then wrap to ~4.29 billion and flash "0:00" for a frame. The banner
  // after a coin happens to keep this off that pass today, but the house rule
  // is to never write (now - stamp) directly — so it does not.
  uint32_t elapsed = sinceMs(now, lastCoinMs);
  uint32_t leftMs = (elapsed >= CREDIT_TIMEOUT_MS) ? 0 : (CREDIT_TIMEOUT_MS - elapsed);
  uint32_t secs = (leftMs + 999) / 1000; // round up, so it never shows 0:00 early
  snprintf(buf, n, "TIME LEFT %lu:%02lu",
           (unsigned long)(secs / 60), (unsigned long)(secs % 60));
}

// ---------------------------------------------------------------------------
//  THE SALE'S SCREENS. See THE SALE, SCREEN BY SCREEN in SECTION 5 for the
//  sequence and timings. Every line here is 16 characters or fewer for every
//  value a real sale produces: the slot shuts once the price is met, so the
//  credit is at most the price plus two coins (KSH 1079 at the console's
//  highest price of 1000). snprintf cuts anything longer; it cannot overflow.
// ---------------------------------------------------------------------------

// A screen that IDLE leaves alone until it expires, then replaces with
// showIdle(). Used for the coin acknowledgement and the thank-you.
void showBanner(const char *l1, const char *l2, uint32_t ms)
{
  say(l1, l2);
  bannerUp = true;
  bannerUntilMs = millis() + ms;
}

// A coin that leaves her short of a pad: say what it was and the total.
void showCoinAck(const CoinDef *coin)
{
  char l1[17], l2[17];
  snprintf(l1, sizeof(l1), "THAT IS KSH %u!", coin->value);
  snprintf(l2, sizeof(l2), "TOTAL KSH %u", credit);
  showBanner(l1, l2, COIN_ACK_MS);
}

void showPaid()
{
  char l1[17], l2[17];
  if (orderIndex > 1)
  {
    snprintf(l1, sizeof(l1), "ONE MORE COMING");
    snprintf(l2, sizeof(l2), "PAD %u OF %u", orderIndex, orderPads);
  }
  else
  {
    snprintf(l1, sizeof(l1), "KSH %u - PAID!", credit);
    uint16_t pads = credit / PAD_PRICE;
    if (pads <= 1)
      snprintf(l2, sizeof(l2), "A FULL PAD!");
    else
      snprintf(l2, sizeof(l2), "%u PADS COMING!", pads);
  }
  say(l1, l2);
}

// After the last pad of a sale. The slot reopens underneath this.
void showThanks()
{
  char l2[17];
  if (credit > 0)
    snprintf(l2, sizeof(l2), "CREDIT KSH %u", credit);
  else
    snprintf(l2, sizeof(l2), "HAVE A GOOD DAY");
  showBanner("THANK YOU!", l2, THANKS_MS);
}

// The price is met (from IDLE). The slot rule in loop() shuts the acceptor on
// this same pass; PAID then holds before the motor starts.
void enterPaid()
{
  if (orderPads == 0)
  {
    // A new sale. How many pads it buys is fixed now, and only a late coin
    // can add to it.
    orderPads = credit / PAD_PRICE;
    if (orderPads == 0)
      orderPads = 1;
    orderIndex = 1;
    paidHoldMs = PAID_HOLD_MS;
    beep(TONE_VEND_HZ, TONE_VEND_MS);
  }
  else
  {
    // The next pad of a sale already running (a KSh 40 buys two).
    orderIndex++;
    if (orderIndex > orderPads)
      orderPads = orderIndex;
    paidHoldMs = NEXT_PAD_HOLD_MS;
  }
  bannerUp = false;
  paidSinceMs = millis();
  state = ST_PAID;
  showPaid();
  LOG(F("Price met — pad "));
  LOG(orderIndex);
  LOG(F(" of "));
  LOG(orderPads);
  LOGLN(F(". Slot shut; holding before the motor starts."));
}

// What the screen does after a coin is banked, in whichever state it landed.
// Shared by real coins and the console's  v  so both behave the same.
void coinScreenAfterBank(const CoinDef *coin)
{
  switch (state)
  {
  case ST_IDLE:
    // Short of a pad: say what it was. Enough: IDLE moves to PAID on this pass.
    if (credit < PAD_PRICE)
      showCoinAck(coin);
    break;
  case ST_PAID:
    // A coin already inside the acceptor when the slot shut. It can add pads
    // to this sale; restart the hold so the new total is read.
    orderPads = (orderIndex - 1) + credit / PAD_PRICE;
    paidSinceMs = millis();
    showPaid();
    break;
  case ST_DISPENSING:
  {
    // Rare now the slot is shut: the coin line is muted while the coil turns.
    orderPads = (orderIndex - 1) + credit / PAD_PRICE;
    char l2[17];
    snprintf(l2, sizeof(l2), "CREDIT KSH %u", credit);
    say("PLEASE WAIT", l2);
    break;
  }
  case ST_DELIVERED:
    // The pad for orderIndex is already paid for; anything left is more pads.
    // "PLEASE COLLECT YOUR PAD" stays up.
    orderPads = orderIndex + credit / PAD_PRICE;
    break;
  default:
    break;
  }
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

// Count a settled stock change and complain when they come too fast to be a
// person refilling a coil. Six in a minute is not a stack of pads settling —
// it is a wire, and the log should say so rather than leave somebody watching
// OUT OF STOCK scroll past wondering whether the switch is faulty.
void noteStockFlap(uint32_t now)
{
  if (stockFlapWindowMs == 0 || sinceMs(now, stockFlapWindowMs) > 60000)
  {
    stockFlapWindowMs = now;
    stockFlaps = 0;
    stockFlapWarned = false;
  }
  stockFlaps++;

  if (stockFlaps >= 6 && !stockFlapWarned)
  {
    stockFlapWarned = true;
    LOGLN();
    LOGLN(F("!! STOCK LINE UNSTABLE — 6 changes in a minute."));
    LOGLN(F("!! A coil being refilled changes once. This is electrical."));
    LOGLN(F("!!   - run the stock switch wire away from the relay wiring"));
    LOGLN(F("!!   - give the relay module its own decoupling"));
    LOGLN(F("!!   - make the grounds meet at one point, not in a daisy chain"));
    LOGLN(F("!! The firmware is already ignoring the stock line for "));
    LOG(F("!! "));
    LOG(INHIBIT_SETTLE_MS);
    LOGLN(F(" ms after every relay move, which is a plaster, not a cure."));
    LOGLN();
  }
}

// How long the stock switch has to hold a new reading before it is believed.
//
// Normally PAD_SWITCH_DEBOUNCE_MS. Once the line has proved itself unstable it
// becomes ten times that, and this is the guard that actually stops the reset
// loop rather than merely slowing it down.
//
// The reasoning: the deaf window and the rate limit together cut the
// oscillation from several hertz to about one, but they cannot end it, because
// an induced reading that outlasts 600 + 400 ms is still believed. What
// separates the two causes is TIME, and only time. A coil that has genuinely
// run out stays empty — four seconds, four hours, until somebody refills it.
// A line following a relay changes back within tens of milliseconds of the
// coil letting go. So a four-second confirmation is invisible to the real
// event and impossible for the fake one.
//
// It escalates rather than shipping at 4 s for everyone, because a machine
// with clean wiring should not wait four seconds to notice it is empty. The
// penalty only lands on a machine that has earned it, and it lifts by itself
// when the flap window expires quietly.
uint32_t stockDebounceMs()
{
  return stockFlapWarned ? (PAD_SWITCH_DEBOUNCE_MS * 10) : PAD_SWITCH_DEBOUNCE_MS;
}

// True when there are pads left to dispense. Bypassed entirely, and always
// true, until PAD_SWITCH_FITTED says the switch actually exists.
//
// Two filters, in this order, and the order matters:
//
//   1. DEAF WINDOW.  For INHIBIT_SETTLE_MS after the inhibit relay moves, the
//      pin is not read at all and the debounce clock is held at now. A change
//      induced by the relay therefore cannot be seen, and a change that is
//      still there when the window closes still has to survive step 2 in full.
//      Read the comment at INHIBIT_SETTLE_MS for what this actually fixed.
//
//   2. DEBOUNCE.     The reading must then hold for PAD_SWITCH_DEBOUNCE_MS.
//
// Both are wrap-safe: sinceMs() and reached(), never a bare subtraction
// against a stamp that may have been written later in the same pass.
bool padsPresent()
{
  if (!PAD_SWITCH_FITTED)
    return true;

  uint32_t now = millis();

  if (sinceMs(now, inhibitChangedMs) < INHIBIT_SETTLE_MS)
  {
    padsRaw = padsStable;  // forget any half-formed change
    padsChangedMs = now;   // and make the next one start its clock from here
    return padsStable;
  }

  bool level = (digitalRead(PIN_PADS) == LOW);
  bool present = PADS_PRESENT_IS_LOW ? level : !level;

  if (present != padsRaw)
  {
    padsRaw = present;
    padsChangedMs = now;
  }
  else if (padsRaw != padsStable && reached(now, padsChangedMs + stockDebounceMs()))
  {
    padsStable = padsRaw;
    noteStockFlap(now);
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
  faultClearsWhenBeamClear = false; // only a beam fault sets it, after this
  beamClearTiming = false;
  orderPads = 0; // the sale is over; credit stays on the meter
  orderIndex = 0;
  bannerUp = false;
  armClearButtonForNewFault();
  faultCount++;
  strncpy(faultLine2, line2, sizeof(faultLine2) - 1);
  faultLine2[sizeof(faultLine2) - 1] = 0;
  faultMsgIdx = 0;
  faultRotateMs = 0; // force an immediate repaint
  beep(TONE_FAULT_HZ, TONE_FAULT_MS);
  state = ST_FAULT;
  // Light the screen: "OUT OF ORDER / CALL THE NUMBER ABOVE" must be readable,
  // and the backlight only sleeps in IDLE, so it stays lit until it is fixed.
  wakeDisplay();
  LOGLN();
  LOG(F("*** FAULT: "));
  LOGLN(line2);
  LOGLN(F("*** Coins inhibited. Clear the chute, then hold the"));
  LOGLN(F("*** front-panel button for 1 s, or type  x."));
  if (credit >= PAD_PRICE)
  {
    LOG(F("*** The customer is owed a pad and has KSH "));
    LOG(credit);
    LOGLN(F(" on the meter."));
    LOG(F("*** The button will retry and dispense it. "));
    LOG(MAX_BUTTON_RETRIES - buttonRetries);
    LOGLN(F(" attempts left."));
  }
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
  bannerUp = false; // whatever was on screen, IDLE owns it now

  if (holdForCollection)
  {
    sayPaged("PAD IS PAID FOR", "PRESS THE BUTTON|TO COLLECT IT", idleMsgIdx);
    idleRotateMs = millis() + FAULT_ROTATE_MS;
    return;
  }

  if (credit > 0 && credit < PAD_PRICE)
  {
    snprintf(l1, sizeof(l1), "ADD KSH %u MORE", PAD_PRICE - credit);
    creditPromptLine(l2, sizeof(l2), millis());
    creditTickMs = millis() + 1000; // next countdown repaint is a second away
  }
  else
  {
    // Nothing owed. Cycle what the machine actually accepts rather than a bare
    // "INSERT COIN" that leaves her guessing which coins are any good.
    snprintf(l1, sizeof(l1), "PAD = KSH %u", PAD_PRICE);
    sayPaged(l1, acceptedCoins, idleMsgIdx);
    idleRotateMs = millis() + FAULT_ROTATE_MS;
    return;
  }
  say(l1, l2);
}

// Put the screen back to whatever the machine actually IS.
//
//  Any console command that takes the display over has to end with this rather
//  than calling showIdle(). showIdle() draws the idle screen unconditionally,
//  so on an out-of-stock or faulted machine it paints INSERT COIN over SORRY -
//  NO PADS or OUT OF ORDER — inviting a coin that the inhibited slot will not
//  take — and leaves it there until the next rotation tick up to two seconds
//  later.
//
//  Zeroing the two rotation deadlines makes those screens repaint immediately
//  instead of waiting for their turn.
void repaintForState()
{
  faultRotateMs = 0;
  emptyRotateMs = 0;
  if (state == ST_IDLE)
    showIdle();
  else if (state == ST_PAID)
    showPaid();
}

// Wake the display, rebuilding it on the way up.
//
//  Waking is always a full rebuild, never just switching the light back on.
//  That is deliberate and it is what makes the sleep worth having: whatever
//  happened to the screen while nobody was watching — a spike, a dropped
//  nibble, anything — is gone before the first person sees it.
void wakeDisplay()
{
  lastActivityMs = millis();
  if (!lcdOk || backlightOn)
    return;
  backlightOn = true;
  lcdNeedsReinit = true; // say() re-syncs the display and lights it for us
  repaintForState();
}

void sleepDisplay()
{
  if (!lcdOk || !backlightOn)
    return;
  backlightOn = false;
  lcd.noBacklight();
}

// Out of stock is NOT a fault. It is a normal condition with an obvious cure,
// so it does not latch: the moment the switch says pads are back, the machine
// returns to service on its own. Nobody drives out to reset it after a refill.
void enterEmpty()
{
  motorOff();
  orderPads = 0; // the sale is over; credit stays and buys a pad after a refill
  orderIndex = 0;
  bannerUp = false;
  state = ST_EMPTY;
  wakeDisplay(); // "SORRY - NO PADS" must be readable, as for a fault
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

void attemptClear(const char *who, bool fromButton)
{
  if (state != ST_FAULT)
  {
    LOG(F("Clear requested by "));
    LOG(who);
    LOGLN(F(" but the machine is not faulted."));
    return;
  }

  // A customer may only try so many times. Past that the coil is being worked
  // against something that is not moving, and she needs a person rather than
  // another attempt. The console keeps working, because that is staff.
  if (fromButton && buttonRetries >= MAX_BUTTON_RETRIES)
  {
    say(callLine(), "SORRY - JAMMED");
    beep(TONE_REJECT_HZ, TONE_REJECT_MS);
    faultRotateMs = millis() + MESSAGE_HOLD_MS;

    // She will keep pressing, because the screen is the only thing telling
    // her to stop and it may not be wired. Say it once properly, then stay
    // quiet — a log that repeats the same line forty times buries whatever
    // comes next.
    if (lastRefusalMs == 0 || sinceMs(millis(), lastRefusalMs) > 15000)
    {
      lastRefusalMs = millis();
      LOG(F("Refusing: "));
      LOG(buttonRetries);
      LOGLN(F(" retries already and still no pad."));
      LOGLN(F("This needs opening up. The console  x  still works."));
      LOGLN();

      //  BE HONEST ABOUT WHAT IS AND IS NOT KNOWN. The screen says JAMMED
      //  because that is the short word that fits, but the machine cannot
      //  actually tell these apart — the beam is its only evidence, and all
      //  three look identical from here.
      LOGLN(F("*** THREE THINGS LOOK THE SAME FROM IN HERE. Check all three:"));
      LOGLN(F("***   1. a pad wedged in the coil"));
      LOGLN(F("***   2. the coil empty, if the stock switch is not fitted"));
      LOGLN(F("***   3. THE IR SENSOR BLIND — pads may be dropping perfectly"));
      LOGLN(F("***      and the machine simply cannot see them. Check it with"));
      LOGLN(F("***      i  before assuming the mechanism is at fault."));
      LOGLN(F("*** Coins stay inhibited either way, because a machine that"));
      LOGLN(F("*** cannot tell whether pads are coming out must not keep"));
      LOGLN(F("*** taking money for them."));
    }
    return;
  }

  // Let the beam settle before believing it. Clearing a fault while a pad is
  // still wedged just faults again on the next coin, after taking the money.
  if (!chuteClear())
  {
    beep(TONE_REJECT_HZ, TONE_REJECT_MS);
    sayAllPages("CHUTE BLOCKED",
                "PLEASE CLEAR|THE CHUTE, THEN|HOLD THE BUTTON|AGAIN", 1400);
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
      beep(TONE_REJECT_HZ, TONE_REJECT_MS);
      {
        char msg[96];
        snprintf(msg, sizeof(msg), "OUT OF ORDER|STILL FAULTY|%s%sPLEASE CALL THE|NUMBER ABOVE",
                 haltReason, haltReason[0] ? "|" : "");
        sayAllPages(callLine(), msg, 1500);
      }
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

  //  THE CREDIT STAYS. This used to be zeroed here, which was wrong: the coin
  //  is already in the cash box and the machine owes a pad. Wiping the meter
  //  on the way out of a fault would have taken her money and given nothing,
  //  which is the one failure the whole payment design exists to prevent.
  //
  //  Keeping it means ST_IDLE sees credit >= price on the very next pass and
  //  dispenses. That is the entire point of the button: she presses it and
  //  gets what she paid for, without finding a member of staff.
  if (credit > 0)
  {
    LOG(F("KSH "));
    LOG(credit);
    LOGLN(F(" is still on the meter and will be dispensed now."));
    if (fromButton)
      buttonRetries++;
  }

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

// THE RETRY BUTTON — a customer control, on the front of the machine.
//
//  Any press wakes a sleeping screen at once, before the hold, so whoever is
//  standing there can read what it says. Held for CLEAR_BTN_HOLD_MS, it retries
//  a failed vend (keeping her credit) or releases a pad recovered after a power
//  cut. It is a hold rather than a press so that brushing past it does nothing
//  more than light the screen.
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
      // Somebody is at the machine. Light the screen now, not after the
      // hold, so she can read it. Harmless when it is already lit.
      wakeDisplay();
    }
  }

  if (clearBtnStable && !clearBtnHandled &&
      (now - clearBtnDownMs) >= CLEAR_BTN_HOLD_MS)
  {
    clearBtnHandled = true;

    // One button, two jobs, told apart by what the machine is doing. Faulted,
    // it retries. Holding a recovered pad, it releases it.
    if (holdForCollection && state == ST_IDLE)
    {
      holdForCollection = false;
      LOGLN(F("Button pressed — releasing the pad that was paid for before"));
      LOGLN(F("the power went. Dispensing now."));
      beep(TONE_COIN_HZ, TONE_COIN_MS);
    }
    else
    {
      attemptClear("the retry button", true);
    }
  }
}

// ===========================================================================
//  SERIAL REPORTING — "what went in, what it means, what comes out"
// ===========================================================================
void reportCoin(uint16_t pulses, const CoinDef *coin, const CoinBurst *b = nullptr)
{
  rule();
  LOG(F("AT           : "));
  LOG((millis() - bootMs) / 1000);
  LOG(F(" s since boot"));
  // Measured from the PREVIOUS coin, not the one just banked. Measuring from
  // lastCoinMs printed the gap since a stamp set microseconds earlier, so it
  // always read 0 — a diagnostic that reported nothing.
  if (prevCoinMs)
  {
    LOG(F(",  "));
    LOG(sinceMs(millis(), prevCoinMs) / 1000);
    LOG(F(" s after the previous coin"));
  }
  LOGLN();
  LOG(F("COIN IN      : "));
  LOG(pulses);
  LOGLN(pulses == 1 ? F(" pulse") : F(" pulses"));

  // With a 20-pulse coin in the table, these two numbers are what decide
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
  {
    // Give a backpack that is still powering up time to answer
    // (see LCD_BOOT_WAIT_MS).
    uint32_t w0 = millis();
    while (!i2cPresent(LCD_I2C_ADDR) && millis() - w0 < LCD_BOOT_WAIT_MS)
    {
      buzzerService();
      feedWatchdog();
      delay(50);
    }
    uint32_t waited = millis() - w0;
    if (waited >= 50 && waited < LCD_BOOT_WAIT_MS)
    {
      LOG(F("  LCD answered after "));
      LOG(waited);
      LOGLN(F(" ms (still powering up)."));
    }
  }
  i2cScan();
  lcdOk = i2cPresent(LCD_I2C_ADDR);
  LOG(F("  LCD at 0x"));
  LOG(LCD_I2C_ADDR, HEX);
  LOGLN(lcdOk ? F("  : present") : F("  : NOT FOUND"));
  if (!lcdOk)
  {
    LOGLN(F("  Running headless. The machine still vends, but the"));
    LOGLN(F("  customer is told nothing. It keeps looking every 5 s and"));
    LOGLN(F("  brings the screen up the moment it answers."));
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
    feedWatchdog();
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
    LOGLN(F(" needs a 10k pull-UP to 3V3, not a"));
    LOGLN(F("  pull-down: the pin floats until setup() runs, and held low"));
    LOGLN(F("  through that window the slot is live on every reset."));
    LOGLN(F("  NOTE: an ESP32's 3.3 V cannot turn OFF a 5 V active-low module"));
    LOGLN(F("  — it energises and never releases. Drive it through an NPN and"));
    LOGLN(F("  set ACTIVE_HIGH true. See README."));
  }
  else
  {
    LOG(F("  Active HIGH, so GPIO "));
    LOG(PIN_INHIBIT);
    LOGLN(F(" needs a 10k pull-DOWN to GND, the"));
    LOGLN(F("  same as the motor pin: held high through boot, the relay"));
    LOGLN(F("  would energise and open the slot on every reset."));
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
  LOG(F("  pin reads   : "));
  LOG(digitalRead(PIN_INHIBIT) == HIGH ? F("HIGH") : F("LOW"));
  LOGLN(F("  <- compare this with what the relay is doing"));
  LOGLN(F("  If they disagree, the config is wrong, not the firmware."));
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
    LOGLN(F("  OFF — the NVS store is never opened and nothing is written."));
    LOGLN(F("  A power cut with money on the meter loses it silently, and"));
    LOGLN(F("  there is no recovered pad to collect. Everything else in the"));
    LOGLN(F("  machine behaves identically. See SECTION 3."));
  }
  else
  {
    LOG(F("  store       : "));
    if (!creditStoreOpen)
      LOGLN(F("WILL NOT OPEN  <- cannot see or clear an old balance"));
    else if (!creditPersistOk)
      LOGLN(F("open but NOT WRITING  <- power-cut protection is gone"));
    else
      LOGLN(F("open, writing normally"));
    LOG(F("  balance in flash: KSH "));
    LOGLN(prefs.getUShort("credit", 0));
    LOGLN(F("  This is the only thing this firmware stores."));
    if (!creditPersistOk)
    {
      LOGLN(F("  The machine still trades, but a power cut now loses whatever"));
      LOGLN(F("  is on the meter. Check the NVS partition."));
    }
  }

  // --- 9. Buzzer ---
  LOG(F("  service msg : \""));
  LOG(SERVICE_CONTACT);
  LOG(F("\"  ("));
  LOG(strlen(SERVICE_CONTACT));
  LOGLN(F(" chars)"));
  if (strlen(SERVICE_CONTACT) > 16)
  {
    LOGLN(F("  !! Longer than the 16-character display. It will be cut off."));
    LOGLN(F("  !! Shorten SERVICE_CONTACT in SECTION 4."));
  }
  else if (strchr(SERVICE_CONTACT, 'X') || strchr(SERVICE_CONTACT, 'x'))
  {
    LOGLN(F("  !! STILL THE PLACEHOLDER. Put the real number printed on the"));
    LOGLN(F("  !! machine here before this goes to a site — the screen tells"));
    LOGLN(F("  !! a paying customer to call it."));
  }

  LOGLN(F("[9] BUZZER"));
  if (SOUND_ON)
  {
    LOGLN(F("  One short chirp now. Silence means check GPIO 25."));
    beep(TONE_COIN_HZ, TONE_COIN_MS);
  }
  else
  {
    LOGLN(F("  Sound is OFF (SOUND_ON = false): the machine plays no tones."));
    LOGLN(F("  Type  b  to check the buzzer anyway."));
  }

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
//    Nothing typed here is saved. The meter balance is the only thing this
//    firmware keeps in flash, and no console command writes to it. So every
//    timing, price and policy you set from here is gone on the next reset, by
//    design: the console is for FINDING the number; SECTION 5 is where it
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
  LOGLN(F("  T<n>     soak: n vends back to back, times every drop and"));
  LOGLN(F("           tells you what to set the dispense timeout to. T25"));
  LOGLN(F("  v<n>     pretend a coin of <n> pulses went in"));
  LOGLN(F("  i        watch the IR sensor live (any key stops it)"));
  LOGLN(F("  k        watch the pad stock switch live"));
  LOGLN(F("  l        LCD test pattern"));
  LOGLN(F("  b        buzzer test"));
  LOGLN(F("  n        click the inhibit relay 3x — does it actually move?"));
  LOGLN(F("  H        hang the loop on purpose: the watchdog must restart it"));
  LOGLN(F("  P<s>     record the raw coin waveform on GPIO 18 for one coin,"));
  LOGLN(F("           next to what the interrupt counted. P or P30"));
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
  LOG(F("  balance saved: "));
  if (!REMEMBER_CREDIT_ACROSS_POWER_LOSS)
    LOGLN(F("off (SECTION 3)"));
  else if (!creditStoreOpen)
    LOGLN(F("NO — store will not open, old balance unknown"));
  else
    LOGLN(creditPersistOk ? F("yes") : F("NO — a power cut will lose it"));
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
  if (!lcdOk)
    LOGLN(F("MISSING — headless"));
  else if (backlightOn)
    LOGLN(F("ok, lit"));
  else
    LOGLN(F("ok, backlight asleep"));
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
  //  The timeout should be set from THIS machine's worst drop, not from a
  //  number somebody picked once. A coil that is wearing, or a cold morning,
  //  shows up here first — as shrinking margin — long before it shows up as a
  //  customer being told OUT OF ORDER by a machine that was working.
  LOG(F("  slowest drop : "));
  if (slowestDropMs == 0)
    LOGLN(F("nothing delivered yet"));
  else
  {
    LOG(slowestDropMs);
    LOG(F(" ms, timeout is "));
    LOG(DISPENSE_TIMEOUT_MS);
    LOGLN(F(" ms"));
    if (DISPENSE_TIMEOUT_MS < slowestDropMs * 2)
    {
      LOG(F("  !! Under twice the slowest drop. Raise it to at least "));
      LOG(slowestDropMs * 2);
      LOGLN(F(" ms"));
      LOGLN(F("  !! (o<ms>), or this machine will fault on a pad that was"));
      LOGLN(F("  !! only slow, and tell a paying customer it is broken."));
    }
  }
  LOG(F("  motor noise  : "));
  LOG(noisePulses);
  LOGLN(F(" pulses discarded"));
  if (noisePulses > 0)
    LOGLN(F("  (any at all means the suppression is not finished)"));
  //  Same idea as motor noise, for the other wire that has been picking up the
  //  relays. A refill is one change. A handful in a minute is a wiring fault,
  //  and it is the one that reset this board mid-self-test.
  if (PAD_SWITCH_FITTED)
  {
    LOG(F("  stock changes: "));
    LOG(stockFlaps);
    LOGLN(F(" in the last minute"));
    if (stockFlaps >= 6)
      LOGLN(F("  !! That is the relay moving the stock line, not pads moving."));
  }
  LOGLN();
  LOG(F("  uptime       : "));
  LOG((millis() - bootMs) / 1000);
  LOGLN(F(" s"));
  LOG(F("  free heap    : "));
  LOGLN(ESP.getFreeHeap());
  LOG(F("  loop max     : "));
  LOG(loopMaxUs);
  LOGLN(F(" us"));
  // Measured 2026-10-01: one full screen paint is ~50 ms over I2C at
  // 100 kHz, and the pass that finishes a coin and starts a vend (report,
  // flash write, two paints) ~230 ms. The old note called 50 ms a stall.
  LOGLN(F("  (a screen paint is ~50 ms; over ~300000 us, something blocked)"));
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
    feedWatchdog();
    delay(1); // this can run for ten seconds; give the watchdog its turn
  }
  motorOff();
  LOGLN(F("Motor OFF."));
  if (!broke)
    LOGLN(F("  beam never broke during the run"));
  LOGLN(F("Time one full revolution this way, then set"));
  LOGLN(F("DISPENSE_TIMEOUT_MS to about double it  (o<ms>)."));
  // Not showIdle(): this command is deliberately allowed while EMPTY, and the
  // machine is still out of stock with the slot inhibited when it finishes.
  repaintForState();
}

// Run the coil over and over with no coins, timing every drop.
//
//  THIS IS HOW THE DISPENSE TIMEOUT GETS ITS NUMBER. Everything else is a
//  guess: a finger at the sensor measures how fast a hand moves, and a handful
//  of vends does not show you the tail. Twenty or thirty real pads down a real
//  chute shows the distribution, and the timeout is set from its worst case.
//
//  It also answers the question the client actually asked, which is not "does
//  it work" but "how many out of thirty come out cleanly".
//
//  Stops early on an empty coil, a blocked chute, three consecutive failures,
//  or any keypress.
void soakTest(uint16_t cycles)
{
  if (cycles == 0 || cycles > 500)
  {
    LOGLN(F("Use T1 to T500.  T25 is a good first run."));
    return;
  }

  rule();
  LOG(F("SOAK TEST — "));
  LOG(cycles);
  LOGLN(F(" cycles, no coins, nothing charged."));
  LOGLN(F("  Load the coil first. Press any key to stop early."));
  LOG(F("  Waiting up to "));
  LOG(SOAK_MAX_WAIT_MS / 1000);
  LOGLN(F(" s per drop, deliberately longer than the"));
  LOGLN(F("  real timeout so slow drops are measured instead of cut off."));
  rule();

  uint16_t delivered = 0, missed = 0, missedInARow = 0;
  uint32_t fastest = 0xFFFFFFFF, worst = 0, total = 0;
  bool aborted = false;

  for (uint16_t i = 1; i <= cycles; i++)
  {
    if (!padsPresent())
    {
      LOGLN(F("  Stock switch says the coil is empty. Stopping."));
      break;
    }

    // Never start over a blocked chute, same rule as a paid vend.
    if (!chuteClear())
    {
      LOGLN(F("  Chute is blocked. Clear it and start again. Stopping."));
      LOGLN(F("  If the last pad has already gone, the sensor is still seeing"));
      LOGLN(F("  something — check it with  i  before soaking again."));
      break;
    }

    char l2[17];
    snprintf(l2, sizeof(l2), "%u of %u", i, cycles);
    say("SOAK TEST", l2);

    motorOn();
    uint32_t start = millis();
    bool dropped = false;
    while (millis() - start < SOAK_MAX_WAIT_MS)
    {
      if (padSeen())
      {
        dropped = true;
        break;
      }
      if (Serial.available())
      {
        aborted = true;
        break;
      }
      buzzerService();
      feedWatchdog();
      delay(1);
    }
    uint32_t took = millis() - start;
    motorOff();

    if (aborted)
    {
      while (Serial.available())
        Serial.read(); // swallow the key that stopped it
      LOGLN(F("  Stopped by keypress."));
      break;
    }

    LOG(F("  #"));
    LOG(i);
    if (dropped)
    {
      delivered++;
      missedInARow = 0;
      total += took;
      if (took < fastest)
        fastest = took;
      if (took > worst)
        worst = took;
      if (took > slowestDropMs)
        slowestDropMs = took;
      LOG(F("  delivered in "));
      LOG(took);
      LOGLN(F(" ms"));
    }
    else
    {
      missed++;
      missedInARow++;
      beep(TONE_REJECT_HZ, TONE_REJECT_MS);
      LOGLN(F("  NO PAD"));
      if (missedInARow >= 3)
      {
        LOGLN(F("  Three in a row with nothing. Coil empty or jammed."));
        LOGLN(F("  Stopping — the rest of the run would measure nothing."));
        break;
      }
    }

    holdFor(SOAK_GAP_MS); // let the beam clear and the coil settle
  }

  // ---- the answer ----
  rule();
  LOGLN(F("SOAK RESULT"));
  rule();
  LOG(F("  delivered    : "));
  LOG(delivered);
  LOG(F(" of "));
  LOGLN(delivered + missed);
  LOG(F("  no pad       : "));
  LOG(missed);
  if (delivered + missed > 0)
  {
    LOG(F("   ("));
    LOG((missed * 100UL) / (delivered + missed));
    LOG(F("%)"));
  }
  LOGLN();

  if (delivered == 0)
  {
    LOGLN(F("  Nothing came out, so there is nothing to measure."));
    LOGLN(F("  Load the coil and check the sensor with  i  first."));
    rule();
    repaintForState();
    return;
  }

  LOG(F("  fastest      : "));
  LOG(fastest);
  LOGLN(F(" ms"));
  LOG(F("  average      : "));
  LOG(total / delivered);
  LOGLN(F(" ms"));
  LOG(F("  SLOWEST      : "));
  LOG(worst);
  LOGLN(F(" ms   <- this is the one that matters"));
  LOGLN();

  uint32_t suggested = ((worst * 2) + 499) / 500 * 500; // round up to 500 ms
  LOG(F("  SET DISPENSE_TIMEOUT_MS TO "));
  LOG(suggested);
  LOGLN(F(" ms"));
  LOG(F("  Try it now:  o"));
  LOGLN(suggested);
  LOGLN(F("  Then write that number into SECTION 5 and reflash, because the"));
  LOGLN(F("  console does not save it."));
  LOGLN();
  LOGLN(F("  Twice the slowest, not the average: the machine has to survive"));
  LOGLN(F("  its worst pad, and a timeout set on the average turns every slow"));
  LOGLN(F("  one into a paying customer being told the machine is broken."));

  if (missed > 0)
  {
    LOGLN();
    LOGLN(F("  The misses are the real result. That percentage is the answer"));
    LOGLN(F("  to the only question the client actually asked, and no amount"));
    LOGLN(F("  of firmware improves it — it is the coil and the pads."));
  }
  rule();
  repaintForState();
}

// Watch the beam for IR_SETTLE_MS and say whether the chute is clear.
//
//  IT POLLS THE WHOLE WINDOW AND ONLY JUDGES AT THE END, which is not fussiness
//  — it is the fix for a real bug.
//
//  padSeen() is a debouncer, so it carries state from whenever it was last
//  called. Immediately after a delivery that state is BLOCKED: the pad tripped
//  the beam, and nothing has called padSeen() since to notice it has gone. A
//  check that bails out on the FIRST reading therefore reports a blocked chute
//  on a chute that is empty.
//
//  That is what stopped the soak dead after every first cycle. It survived in
//  startVend() only by luck: ST_DELIVERED polls padSeen() on every pass for
//  2.5 seconds, so the debouncer was always fresh by the time a vend started.
//  Nothing polled it during the soak's gap between cycles.
//
//  Polling the full window lets the debouncer catch up before the verdict, so
//  the answer reflects the chute rather than the last thing that happened.
bool chuteClear()
{
  uint32_t t0 = millis();
  while (millis() - t0 < IR_SETTLE_MS)
  {
    padSeen();
    buzzerService();
    feedWatchdog();
    delay(1); // 1 ms is well inside IR_DEBOUNCE_MS, so nothing is missed
  }
  return !irStable;
}

// Returns false if it refused to start. The caller must not take the money
// until this has returned true.
bool startVend(const char *why)
{
  // Refuse to run the motor blind. If the beam still reads "pad", the sensor
  // is mis-wired, mis-aimed, or something is stuck in the chute.
  if (!chuteClear())
  {
    LOGLN(F("Refusing to start: the beam already reads BLOCKED."));
    enterFault("CHECK SENSOR");
    faultClearsWhenBeamClear = true;
    return false;
  }
  char l2[17] = "";
  if (orderPads > 1)
    snprintf(l2, sizeof(l2), "PAD %u OF %u", orderIndex, orderPads);
  say("HERE IT COMES...", l2);
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
//  DELIVERED is included because the beam is still clearing and "PLEASE
//  COLLECT YOUR PAD" is still the thing the customer needs to be reading.
//  PAID is included because a coin may still be counting and the motor is
//  about to start.
static bool vendInFlight()
{
  return state == ST_PAID || state == ST_DISPENSING || state == ST_DELIVERED;
}

// ---------------------------------------------------------------------------
//  coinProbe() — console  P<s>.  Record the raw waveform on the coin pin.
// ---------------------------------------------------------------------------
//  The interrupt only ever sees falling edges, so when a coin arrives short of
//  pulses it cannot say why: did the pin never go low for the missing pulse,
//  or never come back high between two of them? Those need opposite fixes.
//
//  This samples GPIO 18 directly, every ~20 us, for one coin, and prints every
//  LOW and HIGH period next to what the interrupt counted for the same coin.
//
//  It only READS the pin. The interrupt keeps counting throughout, and the
//  coin is banked by loop() afterwards exactly as it would have been. Blocks
//  for up to <s> seconds waiting (default 30); any key aborts the wait.
const uint16_t PROBE_MAX_EDGES = 400;
uint32_t probeEdgeUs[PROBE_MAX_EDGES]; // time of each level change, from the first fall

void coinProbe(long waitS)
{
  if (motorRunning || coinMuted)
  {
    LOGLN(F("Motor running or coin line muted. Wait for IDLE."));
    return;
  }
  if (waitS <= 0)
    waitS = 30;

  rule();
  LOG(F("COIN PROBE: drop ONE coin within "));
  LOG(waitS);
  LOGLN(F(" s. Any key aborts."));
  while (Serial.available())
    Serial.read();

  // Wait for the first fall.
  uint32_t t0 = millis();
  while (digitalRead(PIN_COIN) == HIGH)
  {
    if (millis() - t0 >= (uint32_t)waitS * 1000UL || Serial.available())
    {
      while (Serial.available())
        Serial.read();
      LOGLN(F("No coin seen. Probe stopped."));
      rule();
      return;
    }
    feedWatchdog();
    delayMicroseconds(20);
  }

  uint32_t isrBefore = isrTotal; // the first fall has already been counted
  if (isrBefore > 0)
    isrBefore--;
  uint32_t start = micros();
  uint16_t n = 0;
  probeEdgeUs[n++] = 0; // the fall that started it
  int level = LOW;
  uint32_t lastChange = start;

  // Record until the pin has been HIGH for 600 ms (longer than any gap inside
  // a coin), 8 s have passed, or the buffer is full.
  while (true)
  {
    uint32_t now = micros();
    int v = digitalRead(PIN_COIN);
    if (v != level)
    {
      level = v;
      lastChange = now;
      if (n < PROBE_MAX_EDGES)
        probeEdgeUs[n++] = now - start;
    }
    if (level == HIGH && (now - lastChange) >= 600000UL)
      break;
    if ((now - start) >= 8000000UL || n >= PROBE_MAX_EDGES)
      break;
    feedWatchdog();
    delayMicroseconds(20);
  }
  uint32_t isrCounted = isrTotal - isrBefore;

  // Pair up edges: even index = fall (start of LOW), odd = rise (start of HIGH).
  uint16_t lows = 0, shortLows = 0, shortHighs = 0;
  LOGLN(F("  #   LOW ms   then HIGH ms"));
  for (uint16_t i = 0; i < n; i += 2)
  {
    float lowMs = (i + 1 < n) ? (probeEdgeUs[i + 1] - probeEdgeUs[i]) / 1000.0f : -1;
    float highMs = (i + 2 < n) ? (probeEdgeUs[i + 2] - probeEdgeUs[i + 1]) / 1000.0f : -1;
    lows++;
    if (lowMs >= 0 && lowMs < 2.0f)
      shortLows++;
    if (highMs >= 0 && highMs < (float)COIN_PULSE_DEBOUNCE_MS)
      shortHighs++;
    LOG(F("  "));
    if (lows < 10)
      LOG(' ');
    LOG(lows);
    LOG(F("  "));
    if (lowMs < 0)
      LOG(F("  (still low)"));
    else
    {
      if (lowMs < 10)
        LOG(' ');
      LOG(lowMs, 1);
    }
    LOG(F("     "));
    if (highMs < 0)
      LOGLN(F("-  (end of coin)"));
    else
      LOGLN(highMs, 1);
  }
  LOGLN();
  LOG(F("  raw LOW periods seen : "));
  LOGLN(lows);
  LOG(F("  interrupt counted    : "));
  LOGLN(isrCounted);
  LOG(F("  LOWs under 2 ms      : "));
  LOGLN(shortLows);
  LOG(F("  HIGHs under "));
  LOG(COIN_PULSE_DEBOUNCE_MS);
  LOG(F(" ms     : "));
  LOGLN(shortHighs);
  if (n >= PROBE_MAX_EDGES)
    LOGLN(F("  !! buffer full, the end of the coin was not recorded"));
  LOGLN(F("  Sampling is every ~20 us: a dip shorter than that can be"));
  LOGLN(F("  missed here and still be seen by the interrupt."));
  LOGLN(F("  The coin itself is now counted and banked as normal."));
  rule();
}

void handleCommand(char *s)
{
  char c = s[0];
  long n = (strlen(s) > 1) ? atol(s + 1) : -1;

  // Somebody is standing at the machine with a laptop, so wake the screen.
  wakeDisplay();

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
    repaintForState();
    break;
  case 'b':
    LOGLN(SOUND_ON ? F("Chirp.") : F("Chirp (the bench test plays even with SOUND_ON = false)."));
    toneOut(TONE_VEND_HZ, TONE_VEND_MS);
    break;

  case 'n':
    // Click the inhibit relay so you can hear whether it actually moves.
    // "The firmware says inhibited" and "the relay moved" are different
    // claims, and only the first is visible from a console.
    LOGLN(F("Clicking the coin-inhibit relay three times. LISTEN."));
    LOGLN(F("  Three clicks = driven correctly."));
    LOGLN(F("  Silence      = check the pin, the module's supply, and that"));
    LOGLN(F("                 its ground is common with the ESP32's."));
    LOGLN(F("  One click,   = a 5 V active-low module cannot be released by"));
    LOGLN(F("  then stuck     3.3 V. It needs an NPN — see README."));
    // The hold is longer than INHIBIT_SETTLE_MS on purpose. Re-opening the
    // slot is rate-limited by that constant, so a shorter hold here would
    // silently skip every second click and make a working relay sound dead.
    for (uint8_t i = 0; i < 3; i++)
    {
      coinAcceptorAllow(false);
      holdFor(INHIBIT_SETTLE_MS + 150);
      coinAcceptorAllow(true);
      holdFor(INHIBIT_SETTLE_MS + 150);
    }
    LOGLN(F("Done. loop() puts it back to whatever the state calls for."));
    break;

  case 'P':
    coinProbe(n);
    break;

  case 'H':
    // Prove the watchdog: hang loop() on purpose, with no check-in.
    if (vendInFlight())
    {
      LOGLN(F("Busy. Wait for IDLE."));
      break;
    }
    if (!loopWdtOn)
    {
      LOGLN(F("The watchdog is not armed. Not hanging."));
      break;
    }
    LOGLN(F("Hanging loop() on purpose. The watchdog should restart the"));
    LOGLN(F("board within about 5 s; the next boot reports 'task watchdog'."));
    Serial.flush();
    while (true)
    {
      // deliberately nothing: no feed, no yield
    }
    break;

  case 'i':
    irWatch = !irWatch;
    LOGLN(irWatch ? F("IR watch ON — wave a pad through the chute.")
                           : F("IR watch off."));
    break;

  case 'x':
    // Staff override: always allowed, and it forgives the retry ceiling,
    // because somebody with a console has presumably just looked inside.
    buttonRetries = 0;
    lastRefusalMs = 0;
    attemptClear("serial console", false);
    break;

  case 'r':
    vendCount = coinsAccepted = coinsUnknown = faultCount = shillingsIn = 0;
    noisePulses = 0;
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

  case 'T':
    if (state == ST_FAULT)
    {
      LOGLN(F("Faulted. Clear with  x  first."));
      break;
    }
    if (state != ST_IDLE && state != ST_EMPTY)
    {
      LOGLN(F("Busy. Wait for IDLE."));
      break;
    }
    soakTest(n < 0 ? 25 : (uint16_t)n);
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
    bool banked = false;
    if (!coin)
      coinsUnknown++;
    else if (state == ST_IDLE || state == ST_PAID || state == ST_DISPENSING ||
             state == ST_DELIVERED)
      banked = bankCoin(coin);
    reportCoin((uint16_t)n, coin);
    if (banked)
      coinScreenAfterBank(coin);
    else if (state == ST_IDLE)
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

  logEvents = SERIAL_LOG_DEFAULT;
  logOn = SERIAL_LOG_BOOT;

  LOGLN();
  rule();
  LOGLN(F("  PAD DISPENSER  -  prototype firmware v" FW_VERSION));
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

  buildAcceptedCoins();
  buzzerInit();

  // The meter balance is the only thing this firmware keeps in flash.
  //  Not opened at all when the feature is off: a machine built without
  //  persistence should not be touching the NVS partition for any reason.
  if (REMEMBER_CREDIT_ACROSS_POWER_LOSS)
  {
    creditStoreOpen = prefs.begin("padvend", false);
    creditPersistOk = creditStoreOpen;
  }
  if (REMEMBER_CREDIT_ACROSS_POWER_LOSS && !creditStoreOpen)
  {
    LOGLN(F("!! The credit store would not open. Two consequences, and the"));
    LOGLN(F("!! second is the one that matters:"));
    LOGLN(F("!!   1. the meter balance cannot be saved, so a power cut loses"));
    LOGLN(F("!!      whatever is on it;"));
    LOGLN(F("!!   2. we cannot SEE what is already in NVS, so an old balance"));
    LOGLN(F("!!      may be sitting there. If the store opens on a later boot"));
    LOGLN(F("!!      it would be restored and could vend an unpaid pad."));
    LOGLN(F("!! The machine keeps trying to open it every 30 s and will clear"));
    LOGLN(F("!! anything stale the moment it can. It still trades meanwhile."));
    creditPersistWarned = true;
  }

  Wire.begin(PIN_SDA, PIN_SCL);
  // 100 kHz, stated rather than inherited. This is the slowest standard I2C
  // rate and the most tolerant of long unshielded runs to a display mounted on
  // a cabinet door. There is nothing on this bus that needs to go faster.
  Wire.setClock(100000);

  selfTestPassed = selfTest(true);

  // Silent when healthy, loud when broken. If the boot was quiet and the test
  // failed, run it again with the console on — a machine that refuses to work
  // and will not say why is worse than one that chatters.
  if (!selfTestPassed && !logOn)
  {
    logOn = true;
    LOGLN();
    LOGLN(F("SELF-TEST FAILED ON A QUIET BOOT. Repeating it with the console"));
    LOGLN(F("on, because you need to see this one."));
    selfTestPassed = selfTest(true);
  }

  // Initialise the library even when no screen answered. Its init() is the
  // only thing that sets the line count, and setCursor() reads it: on a
  // never-initialised object, line 2 indexes past the end of an array. So a
  // screen found later by the re-probe in loop() can be driven straight away.
  // With no screen attached the writes simply go unanswered.
  lcd.init();
  if (lcdOk)
    lcd.backlight();

  if (!selfTestPassed && HALT_ON_SELFTEST_FAIL)
  {
    {
      char msg[96];
      snprintf(msg, sizeof(msg), "OUT OF ORDER|%s|PLEASE CALL THE|NUMBER ABOVE",
               haltReason[0] ? haltReason : "SELF-TEST FAILED");
      sayAllPages(callLine(), msg, 1600);
    }
    armClearButtonForNewFault();
    // Keep the reason on the fault screen for as long as the halt lasts, not
    // only on the one-shot pages above.
    strncpy(faultLine2, haltReason[0] ? haltReason : "SELF-TEST FAILED",
            sizeof(faultLine2) - 1);
    faultLine2[sizeof(faultLine2) - 1] = 0;
    // A blocked beam is the one boot failure the machine can see being fixed.
    faultClearsWhenBeamClear = (strcmp(haltReason, "CHUTE BLOCKED") == 0);
    LOGLN();
    LOGLN(F("HALTED. The self-test found something that would make this"));
    LOGLN(F("machine take money and do the wrong thing with it."));
    LOGLN(F("The coin input is left DETACHED, so it cannot bank money."));
    LOGLN(F("Fix CONFIG and reflash. The console stays alive."));
    state = ST_FAULT;
    enableLoopWDT(); // a halted machine still runs loop(): guard it too
    loopWdtOn = true;
    return; // no attachInterrupt: coins cannot be counted in this condition
  }

  say("PAD DISPENSER", "FIRMWARE v" FW_VERSION);
  holdFor(1200);
  // Initialise the display once more, now the rails have had over a second
  // to settle. If the controller took its first setup while the 5 V rail was
  // still climbing it can sit blank although it answers on I2C; the next
  // say() re-syncs it (lcdResync, ~15 ms).
  lcdNeedsReinit = true;

  // ---- Restore a balance left on the meter by a power cut ----
  //
  //  The countdown deliberately restarts from here rather than resuming where
  //  it left off. A short cut protects the customer who is still standing
  //  there; a long one must not leave a stranger's balance on the screen all
  //  night for whoever opens up in the morning.
  if (REMEMBER_CREDIT_ACROSS_POWER_LOSS)
  {
    uint16_t saved = prefs.getUShort("credit", 0);
    creditInFlash = saved; // seed the shadow so we do not rewrite what is there
    if (saved > 0)
    {
      credit = saved; // not setCredit(): the stored value is already this
      lastCoinMs = millis();
      creditTickMs = 0;
      rule();
      LOG(F("RECOVERED KSH "));
      LOG(credit);
      LOGLN(F(" left on the meter when the power went."));
      if (credit >= PAD_PRICE && HOLD_RECOVERED_PAD)
      {
        holdForCollection = true;
        LOGLN(F("  That is a full pad, already paid for. HOLDING it until"));
        LOGLN(F("  somebody presses the button or puts a coin in, rather than"));
        LOGLN(F("  dropping it into an empty room where it is simply lost."));
      }
      else if (credit >= PAD_PRICE)
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

  lastActivityMs = millis(); // start the backlight clock from boot
  lastPulseTotal = isrTotal;

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
  logOn = logEvents; // whatever the boot setting was, run at the field level

  // Armed last, so setup's own waits cannot trip it. See THE LOOP WATCHDOG.
  enableLoopWDT();
  loopWdtOn = true;
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

  // ---- Trust the coin line again once the motor has been still a while ----
  if (coinMuted && !motorRunning && reached(now, coinUnmuteMs))
  {
    uint32_t junk;
    portENTER_CRITICAL(&coinMux);
    junk = isrMutedPulses;
    isrMutedPulses = 0;
    isrBurst = 0; // anything counted during the window is noise by definition
    coinMuted = false;
    portEXIT_CRITICAL(&coinMux);

    if (junk)
    {
      noisePulses += junk;
      LOG(F("Discarded "));
      LOG(junk);
      LOG(junk == 1 ? F(" pulse induced by the motor over ")
                    : F(" pulses induced by the motor over "));
      LOG(mutedMotorMs);
      LOGLN(F(" ms of running."));

      //  THE RATE IS THE DIAGNOSIS, not the count. Two quite different faults
      //  produce noise here and they need different parts:
      //
      //    a FIXED burst per run, whatever the run length, is the relay coil
      //    and the motor's field collapsing as it switches — a flyback diode
      //    across the motor is the cure;
      //
      //    a STEADY RATE that scales with run time is continuous arcing at the
      //    brushes, and a flyback diode does nothing for it. That wants
      //    capacitors across the motor terminals.
      //
      //  Printing per-second lets the bench tell them apart at a glance.
      if (mutedMotorMs > 100)
      {
        uint32_t perSec = (junk * 1000UL) / mutedMotorMs;
        LOG(F("  That is about "));
        LOG(perSec);
        LOGLN(F(" per second of motor run."));
        if (perSec > 20)
        {
          LOGLN(F("  A steady rate like that is brush arcing, not the switching"));
          LOGLN(F("  spike, so a flyback diode alone will not fix it. Fit a"));
          LOGLN(F("  ceramic across the motor terminals — anything from 10 to"));
          LOGLN(F("  100 nF helps, bigger cuts more. See README."));
        }
      }
      if (noisePulses == junk)
        LOGLN(F("  Not coins. Every one would have been free credit."));
    }
  }

  // ---- Wake on the FIRST pulse, not on the finished coin ----
  //
  //  takeCoin() does not hand a coin over until the acceptor has been quiet
  //  for COIN_GAP_MS, and on a 20-pulse coin that is seconds away. Watching
  //  the raw pulse counter instead lights the screen the instant her coin
  //  registers, which is when she is looking at it.
  uint32_t pulsesNow = isrTotal;
  if (pulsesNow != lastPulseTotal)
  {
    lastPulseTotal = pulsesNow;
    wakeDisplay();
  }

  // ---- Sleep only when there is genuinely nothing to show ----
  if (BACKLIGHT_TIMEOUT_MS > 0 && backlightOn && state == ST_IDLE &&
      credit == 0 && reached(now, lastActivityMs + BACKLIGHT_TIMEOUT_MS))
  {
    sleepDisplay();
  }

  // ---- Backstop. Checked in every state, before anything else can run. ----
  // Nothing below this line is trusted to stop the motor on its own.
  if (motorRunning && reached(now, motorOnSince + motorHardLimitMs()))
  {
    motorOff();
    LOGLN(F("!! Hard motor limit hit. The state machine did not stop it."));
    LOGLN(F("!! That is a firmware bug, not a jam. Report it."));
    enterFault("MOTOR RUNAWAY");
  }

  // ---- Read the beam on EVERY pass ----
  //  padSeen() is a debouncer and only knows what it was last asked. It used
  //  to be polled only during a vend, so at rest the machine never noticed an
  //  unplugged sensor or a pad stuck in the chute. One digitalRead per pass.
  padSeen();

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

  // ---- A beam that stays blocked at rest: shut the slot ----
  //  Something is in the chute, or the sensor is unplugged (with the 1 M
  //  pull-down an unplugged sensor reads BLOCKED). Either way a vend could
  //  not see its pad drop, so stop taking money now rather than after a
  //  customer has paid. It clears itself once the beam reads clear again.
  if (state == ST_IDLE && irStable && !irStuckWarned &&
      reached(now, irBlockedSince + IR_STUCK_WARN_MS))
  {
    irStuckWarned = true;
    LOGLN(F("The chute has read BLOCKED at rest for 5 s: something is in it,"));
    LOGLN(F("or the IR sensor is unplugged. Shutting the coin slot."));
    enterFault("CHECK SENSOR");
    faultClearsWhenBeamClear = true;
  }

  // ---- ...and undo a beam fault once the beam reads clear again ----
  if (state == ST_FAULT && faultClearsWhenBeamClear)
  {
    if (irStable)
    {
      beamClearTiming = false;
    }
    else if (!beamClearTiming)
    {
      beamClearTiming = true;
      beamClearSinceMs = now;
    }
    else if (reached(now, beamClearSinceMs + BEAM_CLEAR_RECOVER_MS))
    {
      beamClearTiming = false;
      LOGLN(F("The beam has read clear for 3 s. Trying to return to service."));
      attemptClear("the sensor reading clear again", false);
      // If it is still faulted, something other than the beam is wrong:
      // leave it for a person rather than retrying every three seconds.
      if (state == ST_FAULT)
        faultClearsWhenBeamClear = false;
    }
  }

  // ---- Coins ----
  CoinBurst cb;
  if (takeCoin(cb))
  {
    // The acceptor has just fired its gate solenoid to take or reject that
    // coin, and that dips the 12 V rail. On this build the acceptor corrupted
    // the display on a bench with no motor connected, so this is not
    // theoretical. The screen is about to be repainted anyway, so rebuilding
    // it first costs nothing.
    lcdNeedsReinit = true;

    const CoinDef *coin = lookupCoin(cb.pulses);

    if (!coin)
    {
      coinsUnknown++;
      beep(TONE_REJECT_HZ, TONE_REJECT_MS);
      reportCoin(cb.pulses, coin, &cb);
      if (state == ST_IDLE)
      {
        // "SEE SERIAL" means nothing to somebody standing in a washroom. Tell
        // her which coins this machine actually takes.
        sayAllPages("COIN NOT TAKEN", acceptedCoins, 1400);
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
      // IDLE, PAID, DISPENSING or DELIVERED: the machine can honour this, so
      // bank it. Outside IDLE the slot is shut, so this is a coin that was
      // already inside the acceptor; those states end in IDLE within seconds
      // and the credit buys the next pad.
      // A coin means somebody is standing at the machine, which is the only
      // thing a held pad was ever waiting for.
      if (holdForCollection)
      {
        holdForCollection = false;
        LOGLN(F("Somebody is here — releasing the pad held from before the"));
        LOGLN(F("power cut as well."));
      }

      if (bankCoin(coin))
        beep(TONE_COIN_HZ, TONE_COIN_MS);
      else
        beep(TONE_REJECT_HZ, TONE_REJECT_MS);
      reportCoin(cb.pulses, coin, &cb);
      coinScreenAfterBank(coin);
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

  //  A LATCHED JAM CLEARS ITSELF WHEN SOMEBODY HAS CLEARLY BEEN INSIDE.
  //
  //  Nothing the machine can sense tells it a jam has been fixed — the beam
  //  was already clear while it was jammed, so that proves nothing. But
  //  emptying and refilling the coil is something only a person with the door
  //  open can do, and it is exactly what fixing a jam involves. So the stock
  //  switch going away and coming back is treated as a service visit, and the
  //  machine tries to go back to work on its own.
  //
  //  Without this a jam that ran out its three retries stays dead until
  //  somebody thinks to plug in a laptop, which is not a reasonable thing to
  //  need after a refill.
  if (state == ST_FAULT)
  {
    if (!stocked)
    {
      serviceCycleSeen = true;
    }
    else if (serviceCycleSeen)
    {
      serviceCycleSeen = false;
      LOGLN();
      LOGLN(F("Stock cycled away and back while faulted — somebody has been"));
      LOGLN(F("inside. Treating it as a service visit and returning to work."));
      buttonRetries = 0; // whoever refilled it earned a fresh budget
      lastRefusalMs = 0;
      attemptClear("a refill", false);
    }
  }
  else
  {
    serviceCycleSeen = false;
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
  //  The acceptor is LIVE only while the machine is waiting for money: IDLE,
  //  with the credit still short of a pad (or a recovered pad waiting for
  //  someone, whose coin is what releases it). It shuts on the very pass the
  //  price is met and stays shut through PAID, DISPENSING and DELIVERED, as
  //  well as in EMPTY and FAULT.
  //
  //  Why shut during a sale: the coin line is muted while the motor turns,
  //  and a coin whose pulses arrive then is binned, money taken and nothing
  //  credited. A coin already inside the acceptor when the slot shuts is
  //  still counted, because PAID waits for the coin line to go quiet before
  //  the motor starts.
  //
  //  Driving it from state in one place means the slot can never disagree with
  //  what the machine is actually doing. There is no way to enter a state and
  //  forget to open or close it, because nothing else touches it.
  coinAcceptorAllow(state == ST_IDLE && !(credit >= PAD_PRICE && !holdForCollection));

  switch (state)
  {

  // -----------------------------------------------------------------
  case ST_IDLE:
  {
    // ---- A banner (coin acknowledgement, thank-you) holds the screen ----
    if (bannerUp && reached(now, bannerUntilMs))
      showIdle(); // clears bannerUp

    // ---- Part payment: keep asking, and show how long is left. ----
    if (credit > 0 && credit < PAD_PRICE)
    {
      // Repaint line 2 once a second. Line 1 already says what to add, and
      // does not change until another coin lands, so it is left alone.
      if (!bannerUp && reached(now, creditTickMs))
      {
        creditTickMs = now + 1000;
        char l2[17];
        creditPromptLine(l2, sizeof(l2), now);
        sayLine2(l2);
      }

      // CREDIT_TIMEOUT_MS of 0 means wait forever, so the clock never runs
      // out. Without this guard a zero would make (now - lastCoinMs) > 0 true
      // almost immediately and wipe the credit on the very next pass.
      if (CREDIT_TIMEOUT_MS > 0 && reached(now, lastCoinMs + CREDIT_TIMEOUT_MS))
      {
        // Print the MEASURED wait, not the configured one. Printing the
        // setting here made the log useless for telling "she really did take
        // two minutes" apart from "something cleared it early" — the two look
        // identical, and only one of them is a bug.
        LOG(F("Part payment of KSH "));
        LOG(credit);
        LOG(F(" cleared after an actual wait of "));
        LOG(sinceMs(now, lastCoinMs) / 1000);
        LOG(F(" s (limit "));
        LOG(CREDIT_TIMEOUT_MS / 1000);
        LOGLN(F(" s)."));
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
    //  A pad recovered from a power cut waits for a person. Everything else
    //  vends straight away: somebody is standing there, they just paid.
    if (credit >= PAD_PRICE && !holdForCollection)
      enterPaid();
    break;
  }

  // -----------------------------------------------------------------
  case ST_PAID:
  {
    // The slot is already shut (the rule above). This pause is for her to
    // read the screen, AND for any coin that was already inside the acceptor
    // when the slot shut: its pulses must finish before the motor starts,
    // because the coin line is muted while the coil turns.
    if (!stocked)
    {
      LOGLN(F("Stock ran out before the motor started. Credit stays."));
      enterEmpty();
      break;
    }
    bool quiet = !coinBurstInProgress();
    if (quiet && reached(now, paidSinceMs + paidHoldMs))
    {
      startVend("credit met");
    }
    else if (reached(now, paidSinceMs + PAID_MAX_WAIT_MS))
    {
      LOGLN(F("Coin line still busy at PAID_MAX_WAIT_MS. Starting anyway."));
      startVend("credit met");
    }
    break;
  }

  // -----------------------------------------------------------------
  case ST_DISPENSING:
  {
    if (padSeen())
    {
      motorOff();
      uint32_t took = millis() - dispenseStartMs;
      if (took > slowestDropMs)
        slowestDropMs = took;

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
      buttonRetries = 0; // it worked, so the retry budget is whole again
      lastRefusalMs = 0;
      // A request, not an order. Any change left over is named on the
      // THANK YOU screen that follows.
      say("PLEASE COLLECT", "YOUR PAD :)");
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
    else if (reached(now, dispenseStartMs + DISPENSE_TIMEOUT_MS))
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
      state = ST_IDLE;
      LOGLN(F("Ready."));
      if (credit >= PAD_PRICE && !holdForCollection)
      {
        // More pads in this sale (a KSh 40). IDLE moves straight to PAID on
        // the next pass, and the slot stays shut in between.
      }
      else
      {
        orderPads = 0;
        orderIndex = 0;
        showThanks(); // the slot reopens underneath it
      }
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
        reached(now, lastCoinMs + CREDIT_TIMEOUT_MS))
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
      char msg[80];
      if (credit > 0)
        snprintf(msg, sizeof(msg),
                 "COIN SLOT CLOSED|YOUR KSH %u|IS SAFE AND|WAITING FOR YOU",
                 credit);
      else
        snprintf(msg, sizeof(msg),
                 "COIN SLOT CLOSED|PLEASE COME|BACK LATER");
      sayPaged("SORRY - NO PADS", msg, emptyMsgIdx);
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
      //  Two quite different messages, because two quite different people are
      //  standing there. With credit on the meter it is a customer who has
      //  paid and has no pad: tell her to hold the button, not that the
      //  machine is out of order. With nothing owed it really is out of order.
      //  Three different people can be standing here and they need three
      //  different things said to them. Every line below is 16 characters or
      //  fewer, because the display silently cuts off anything longer.
      //  Three different people can be standing here and they need three
      //  different things said to them. Each bottom line is written as a whole
      //  sentence and paged across frames rather than abbreviated to fit.
      if (buttonRetries >= MAX_BUTTON_RETRIES)
      {
        //  Out of retries. She cannot fix this, so stop telling her to try.
        //
        //  Two audiences read this same screen and neither has a laptop: the
        //  customer standing there, and whoever eventually opens the door. The
        //  customer's part comes first because she is here now, and the reason
        //  to call is named — "you have paid" is what makes a call worth
        //  making. The staff hint goes last, where it does not delay her.
        //
        //  The number holds on the top line (callLine()) for as long as the
        //  fault lasts, so it can be written down; only line 2 pages. The
        //  first page still carries the point on its own, that she has paid,
        //  and the staff hint trails behind where it delays nobody.
        //
        //  Nothing here promises her money back. The machine cannot give it
        //  back and must not imply otherwise; a person on the phone can.
        char msg[120];
        if (credit > 0)
          snprintf(msg, sizeof(msg),
                   "YOU PAID KSH %u|OUT OF ORDER|PLEASE CALL THE|NUMBER ABOVE"
                   "|STAFF: CHECK THE|COIL AND SENSOR",
                   credit);
        else
          snprintf(msg, sizeof(msg),
                   "OUT OF ORDER|CANNOT DISPENSE|PLEASE CALL THE|NUMBER ABOVE"
                   "|STAFF: CHECK THE|COIL AND SENSOR");
        sayPaged(callLine(), msg, faultMsgIdx);
      }
      else if (credit >= PAD_PRICE)
      {
        // She has paid and has no pad, and the button will try again for her.
        sayPaged("PAID - NO PAD",
                 "HOLD THE BUTTON|FOR 1 SECOND|TO TRY AGAIN", faultMsgIdx);
      }
      else
      {
        // Nobody is owed anything, so it is simply broken. Never "SEE SERIAL":
        // that means nothing to somebody standing in a washroom.
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "OUT OF ORDER|%s%sCOIN SLOT CLOSED|PLEASE CALL THE|NUMBER ABOVE",
                 faultLine2, faultLine2[0] ? "|" : "");
        sayPaged(callLine(), msg, faultMsgIdx);
      }
    }
    break;
  }
  }

  // ---- Cycle the idle prompt so the whole of it gets read ----
  //  It keeps cycling while the backlight is asleep. The text is readable in
  //  the dark, and this used to stop with the backlight, freezing the screen
  //  mid-sentence ("PAD = KSH 20" / "THIS MACHINE") until somebody came by.
  //  A repaint is ~50 ms every two seconds.
  if (state == ST_IDLE && lcdOk && !bannerUp &&
      reached(now, idleRotateMs) && (credit == 0 || holdForCollection))
  {
    idleRotateMs = now + FAULT_ROTATE_MS;
    if (holdForCollection)
    {
      sayPaged("PAD IS PAID FOR", "PRESS THE BUTTON|TO COLLECT IT", idleMsgIdx);
    }
    else
    {
      char l1[17];
      snprintf(l1, sizeof(l1), "PAD = KSH %u", PAD_PRICE);
      sayPaged(l1, acceptedCoins, idleMsgIdx);
    }
  }

  // ---- Rebuild the display periodically, whatever happened to it ----
  //
  //  Belt and braces on top of the event triggers. Whatever corrupted the
  //  screen, and whether or not we know what it was, it cannot survive longer
  //  than this. Skipped mid-vend: the beam matters more than the screen, and
  //  a vend is over in seconds anyway.
  //  Skipped while asleep: waking rebuilds the display anyway, so refreshing a
  //  dark screen would be work nobody benefits from.
  if (LCD_REFRESH_MS > 0 && lcdOk && backlightOn && !vendInFlight() &&
      reached(now, lcdRefreshMs))
  {
    lcdRefreshMs = now + LCD_REFRESH_MS;
    lcdNeedsReinit = true;
    repaintForState();
  }

  // ---- A screen missed at boot is looked for until it answers ----
  //  Not mid-vend: bringing it up paints the screen, and while the coil turns
  //  the beam comes first.
  if (!lcdOk && !vendInFlight() && reached(now, lcdProbeMs))
  {
    lcdProbeMs = now + LCD_REPROBE_MS;
    if (i2cPresent(LCD_I2C_ADDR))
    {
      lcdOk = true;
      backlightOn = true;
      lastActivityMs = now;
      lcdNeedsReinit = true; // say() runs the full HD44780 init sequence
      LOGLN(F("LCD answered after boot. Screen is up."));
      repaintForState();
    }
  }

  // ---- A store that refused at boot is retried until it opens ----
  //
  //  setCredit() also tries, but only when money moves. An idle machine with
  //  an unreadable store would otherwise never find out that an old balance is
  //  waiting in NVS to be restored by a later boot.
  if (REMEMBER_CREDIT_ACROSS_POWER_LOSS && !creditStoreOpen &&
      reached(now, storeRetryMs))
  {
    storeRetryMs = now + 30000;
    ensureCreditStore();
  }

  // ---- Heartbeat, for soak tests ----
  if (heartbeatOn && reached(now, lastBeatMs + HEARTBEAT_MS))
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
