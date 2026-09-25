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
*/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <esp_system.h>

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
//  The three marked LIVE can be changed from the console while the machine is
//  running, so you can find the right value without a reflash. Write what you
//  find back into this section afterwards.

// Shortest gap between two pulses that we will believe. Must be WELL below the
// acceptor's own pulse period, or you will merge pulses and misread coins.
// Most acceptors run 40-100 ms between pulses, so 20 ms is a safe floor.
const uint32_t COIN_PULSE_DEBOUNCE_MS = 20;

// Quiet time that means "that coin is finished, count the pulses".     LIVE: g
// Must be longer than the gap BETWEEN pulses of one coin and shorter than a
// person can insert two coins. 250 ms suits nearly every acceptor.
uint32_t COIN_GAP_MS = 250;

// Give up waiting for the pad to break the beam: empty, or jammed.     LIVE: o
// Set it to about double the time one revolution of the coil takes. Command  m
// runs the motor for a measured number of milliseconds so you can time it.
uint32_t DISPENSE_TIMEOUT_MS = 3000;

// Drop partial credit after this long with nobody feeding it.
const uint32_t CREDIT_TIMEOUT_MS = 20000;

// Ignore beam flicker shorter than this.
const uint32_t IR_DEBOUNCE_MS = 15;

// Beam must read clear for this long before we dare start the motor.
const uint32_t IR_SETTLE_MS = 150;

// How long "TAKE YOUR PAD" stays up.
const uint32_t MESSAGE_HOLD_MS = 2500;

// Hard ceiling on motor run time, over and above DISPENSE_TIMEOUT_MS. This is
// the backstop: it is checked every single pass of loop() in every state, so
// even a bug in the state machine below cannot leave the coil turning. If it
// ever fires you have a real problem and the console will say so.
const uint32_t MOTOR_LIMIT_MARGIN_MS = 2000;

// A mechanical switch under a stack of pads chatters as the stack settles, so
// debounce it far harder than an electronic sensor.
const uint32_t PAD_SWITCH_DEBOUNCE_MS = 400;

// The beam reading "pad present" for this long while idle means something is
// sitting in the chute, or the sensor is mis-aimed. Warning only.
const uint32_t IR_STUCK_WARN_MS = 5000;

// ===========================================================================
//  SECTION 6 — SOUND.  It sits in a public washroom. Keep it short.
// ===========================================================================
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

uint16_t credit = 0; // shillings on the meter
uint32_t lastCoinMs = 0;
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

// Counters. RAM only — nothing is written to flash in this build.
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
//  ISR
// ===========================================================================
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
}

void motorOff()
{
  digitalWrite(PIN_RELAY, RELAY_ACTIVE_HIGH ? LOW : HIGH);
  motorRunning = false;
}

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
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(l1);
  lcd.setCursor(0, 1);
  lcd.print(l2);
}

// Reads the sensor and returns true only once a change has held for
// IR_DEBOUNCE_MS.
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

void enterFault(const char *line2)
{
  motorOff();
  coinAcceptorAllow(false); // stop taking money for pads we can't drop
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

void showIdle()
{
  char l1[17], l2[17];
  snprintf(l1, sizeof(l1), "PAD = KSH %u", PAD_PRICE);
  if (credit > 0)
    snprintf(l2, sizeof(l2), "CREDIT KSH %u", credit);
  else
    snprintf(l2, sizeof(l2), "INSERT COIN");
  say(l1, l2);
}

// Out of stock is NOT a fault. It is a normal condition with an obvious cure,
// so it does not latch: the moment the switch says pads are back, the machine
// returns to service on its own. Nobody drives out to reset it after a refill.
void enterEmpty()
{
  motorOff();
  coinAcceptorAllow(false);
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
    LOGLN(F(" is still on the meter and will buy a pad on refill."));
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
  credit = 0;
  LOG(F("Fault cleared by "));
  LOGLN(who);
  LOGLN(F("Credit zeroed."));

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
  coinAcceptorAllow(true);
  beep(TONE_VEND_HZ, TONE_VEND_MS);
  showIdle();
  LOGLN(F("Coins enabled."));
}

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

  if (state != ST_IDLE)
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
//  SELF-TEST
// ===========================================================================
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
    padSeen();
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
//  SERIAL CONSOLE
// ===========================================================================
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
  coinAcceptorAllow(false); // physically refuse coins mid-vend
  motorOn();
  dispenseStartMs = millis();
  state = ST_DISPENSING;
  return true;
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
    selfTestPassed = selfTest(true);
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
    LOGLN(F("LCD: both lines filled, then back to idle."));
    say("0123456789ABCDEF", "BRICAPA  v" FW_VERSION);
    holdFor(2000);
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
    const CoinDef *coin = lookupCoin((uint16_t)n);
    if (coin && state == ST_IDLE)
    {
      credit += coin->value;
      shillingsIn += coin->value;
      coinsAccepted++;
      lastCoinMs = millis();
    }
    else if (!coin)
    {
      coinsUnknown++;
    }
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
//  SETUP
// ===========================================================================
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
  LOGLN(F("  Type ? for commands. Nothing is saved to flash."));
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

  Wire.begin(PIN_SDA, PIN_SCL);

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
    else if (state != ST_IDLE)
    {
      // Mid-vend or faulted. Banking it now would owe a pad we cannot promise.
      beep(TONE_REJECT_HZ, TONE_REJECT_MS);
      if (state == ST_DISPENSING || state == ST_DELIVERED)
        say("PLEASE WAIT", "ONE AT A TIME");
      reportCoin(cb.pulses, coin, &cb);
    }
    else
    {
      credit += coin->value;
      shillingsIn += coin->value;
      coinsAccepted++;
      lastCoinMs = now;
      beep(TONE_COIN_HZ, TONE_COIN_MS);
      reportCoin(cb.pulses, coin, &cb);

      if (credit < PAD_PRICE)
      {
        char l1[17], l2[17];
        snprintf(l1, sizeof(l1), "CREDIT KSH %u", credit);
        snprintf(l2, sizeof(l2), "NEED KSH %u MORE", PAD_PRICE - credit);
        say(l1, l2);
      }
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
    coinAcceptorAllow(true);
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

  // Self-healing: idle and not faulted means the acceptor should be live.
  if (state == ST_IDLE && !coinsAllowed)
    coinAcceptorAllow(true);

  switch (state)
  {

  // -----------------------------------------------------------------
  case ST_IDLE:
  {
    // Forget stale part-payment so the next customer starts clean.
    if (credit > 0 && credit < PAD_PRICE && (now - lastCoinMs) > CREDIT_TIMEOUT_MS)
    {
      LOG(F("Partial credit of KSH "));
      LOG(credit);
      LOGLN(F(" timed out, cleared."));
      credit = 0;
      showIdle();
    }

    // Take the money only once the motor is actually turning. If the beam
    // check refuses, the credit stays on the meter rather than vanishing
    // into a fault the customer did not cause.
    if (credit >= PAD_PRICE)
    {
      uint16_t after = KEEP_CHANGE_AS_CREDIT ? (credit - PAD_PRICE) : 0;
      if (startVend("credit met"))
        credit = after;
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
      vendCount++;
      say("TAKE YOUR PAD", credit > 0 ? "CREDIT REMAINS" : "");
      beep(TONE_VEND_HZ, TONE_VEND_MS);
      messageUntilMs = millis() + MESSAGE_HOLD_MS;
      state = ST_DELIVERED;
      rule();
      LOG(F("PAD DELIVERED in "));
      LOG(took);
      LOGLN(F(" ms"));
      LOG(F("  session vends : "));
      LOGLN(vendCount);
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
      LOGLN(F("  The customer has paid for this pad and has not got it."));

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
    if (credit > 0 && (now - lastCoinMs) > CREDIT_TIMEOUT_MS)
    {
      LOG(F("Credit of KSH "));
      LOG(credit);
      LOGLN(F(" timed out while out of stock."));
      credit = 0;
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
