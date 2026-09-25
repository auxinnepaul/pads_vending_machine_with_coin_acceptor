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
const bool INHIBIT_RELAY_ACTIVE_HIGH = true;

// Which contact of the relay you wired across the 2-pin port.
//   true  = NC. Closed with the coil off, so the acceptor is inhibited
//           whenever this board is unpowered, booting, halted or crashed.
//           Fail-safe. Ship it this way.
//   false = NO. A dead controller accepts coins and delivers nothing.
//           Bench use only.
const bool INHIBIT_CONTACT_IS_NC = true;

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
  Serial.println(F("Coin interrupt attached."));
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
    Serial.print(F("Coin acceptor "));
    Serial.println(allow ? F("ENABLED") : F("INHIBITED — coins will be returned"));
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

void rule()
{
  Serial.println(F("-----------------------------------------------------"));
}

// void enterFault(const char *line2) {
//   motorOff();
//   faultCount++;
//   say("OUT OF ORDER", line2);
//   beep(TONE_FAULT_HZ, TONE_FAULT_MS);
//   state = ST_FAULT;
//   Serial.println();
//   Serial.print(F("*** FAULT: ")); Serial.println(line2);
//   Serial.println(F("*** Latched. Power-cycle, or type  x  to clear."));
// }

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
  Serial.println();
  Serial.print(F("*** FAULT: "));
  Serial.println(line2);
  Serial.println(F("*** Coins inhibited. Clear the chute, then hold the"));
  Serial.println(F("*** front-panel button for 1 s, or type  x."));
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

void attemptClear(const char *who)
{
  if (state != ST_FAULT)
  {
    Serial.print(F("Clear requested by "));
    Serial.print(who);
    Serial.println(F(" but the machine is not faulted."));
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
    Serial.print(F("Refusing to clear ("));
    Serial.print(who);
    Serial.println(F("): the beam still reads BLOCKED."));
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
    Serial.println(F("Halted at boot — re-running the self-test before"));
    Serial.println(F("returning this machine to service."));
    selfTestPassed = selfTest(true);
    if (!selfTestPassed)
    {
      say("STILL FAULTY", haltReason[0] ? haltReason : "SEE SERIAL");
      beep(TONE_REJECT_HZ, TONE_REJECT_MS);
      Serial.print(F("Refusing to clear ("));
      Serial.print(who);
      Serial.println(F("): the self-test still fails."));
      Serial.println(F("Coins stay inhibited. Fix it, then hold again."));
      armClearButtonForNewFault();
      faultRotateMs = millis() + MESSAGE_HOLD_MS;
      return;
    }
    attachCoinIsr();
  }

  motorOff();
  credit = 0;
  state = ST_IDLE;
  coinAcceptorAllow(true);
  beep(TONE_VEND_HZ, TONE_VEND_MS);
  showIdle();
  Serial.print(F("Fault cleared by "));
  Serial.println(who);
  Serial.println(F("Credit zeroed, coins enabled."));
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
  Serial.print(F("COIN IN      : "));
  Serial.print(pulses);
  Serial.println(pulses == 1 ? F(" pulse") : F(" pulses"));

  // With a 40-pulse coin in the table, these two numbers are what decide
  // whether COIN_GAP_MS is safe, and there is no other way to see them.
  if (b && b->pulses > 1)
  {
    Serial.print(F("PULSE TIMING : "));
    Serial.print(b->durMs);
    Serial.print(F(" ms burst, gaps "));
    Serial.print(b->gapMin);
    Serial.print(F("-"));
    Serial.print(b->gapMax);
    Serial.println(F(" ms"));

    if ((uint32_t)b->gapMax * 2 > COIN_GAP_MS)
    {
      Serial.print(F("  !! Widest gap is over half of COIN_GAP_MS ("));
      Serial.print(COIN_GAP_MS);
      Serial.println(F(" ms)."));
      Serial.println(F("  !! This coin is close to being read as two, which"));
      Serial.println(F("  !! would short-change the customer. Raise it with"));
      Serial.println(F("  !! g<ms> — but stay under 600 ms or two coins in a"));
      Serial.println(F("  !! row will merge into one unknown coin."));
    }
    if (b->gapMin < COIN_PULSE_DEBOUNCE_MS * 2)
    {
      Serial.println(F("  !! Tightest gap is near the debounce floor. Pulses"));
      Serial.println(F("  !! may be being merged. If counts read LOW, lower"));
      Serial.println(F("  !! COIN_PULSE_DEBOUNCE_MS in SECTION 5."));
    }
  }

  if (!coin)
  {
    Serial.println(F("RECOGNISED   : NO — this pulse count is not in the table"));
    Serial.println(F("MEANING      : the acceptor took a coin this firmware"));
    Serial.println(F("               cannot price. The customer has paid and"));
    Serial.println(F("               is owed nothing until you fix SECTION 2."));
    Serial.print(F("WILL VEND    : nothing. Add  { "));
    Serial.print(pulses);
    Serial.println(F(", <value>, \"<name>\" }  to COIN_TABLE."));
    rule();
    return;
  }

  Serial.print(F("RECOGNISED   : "));
  Serial.print(coin->name);
  Serial.print(F("  (worth KSH "));
  Serial.print(coin->value);
  Serial.println(F(")"));

  Serial.print(F("CREDIT NOW   : KSH "));
  Serial.print(credit);
  Serial.print(F(" of KSH "));
  Serial.println(PAD_PRICE);

  if (state != ST_IDLE)
  {
    Serial.print(F("MEANING      : machine is "));
    Serial.print(stateName(state));
    Serial.println(F(" — coin logged, NOT banked"));
    Serial.println(F("WILL VEND    : nothing. It never owes a pad it cannot"));
    Serial.println(F("               deliver. This coin is not refundable."));
  }
  else if (credit < PAD_PRICE)
  {
    Serial.print(F("MEANING      : short by KSH "));
    Serial.println(PAD_PRICE - credit);
    Serial.println(F("WILL VEND    : nothing yet — waiting for more"));
  }
  else
  {
    uint16_t pads = KEEP_CHANGE_AS_CREDIT ? (credit / PAD_PRICE) : 1;
    uint16_t left = KEEP_CHANGE_AS_CREDIT ? (credit % PAD_PRICE) : 0;
    Serial.println(F("MEANING      : price met"));
    Serial.print(F("WILL VEND    : "));
    Serial.print(pads);
    Serial.println(pads == 1 ? F(" pad") : F(" pads, one after the other"));
    Serial.print(F("LEFT OVER    : KSH "));
    Serial.print(left);
    if (left > 0)
      Serial.println(F("  (stays on the meter, no change given)"));
    else
      Serial.println();
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
  Serial.println(F("  I2C bus scan:"));
  uint8_t found = 0;
  for (uint8_t a = 1; a < 127; a++)
  {
    if (i2cPresent(a))
    {
      Serial.print(F("    device at 0x"));
      if (a < 16)
        Serial.print('0');
      Serial.print(a, HEX);
      if (a == LCD_I2C_ADDR)
        Serial.print(F("   <- LCD_I2C_ADDR"));
      else if (a == 0x27 || a == 0x3F)
        Serial.print(F("   <- looks like an LCD backpack"));
      Serial.println();
      found++;
    }
  }
  if (!found)
    Serial.println(F("    nothing responded. Check SDA/SCL and the LCD's 5 V."));
}

// Returns false if something fatal was found.
bool selfTest(bool verbose)
{
  bool ok = true;
  haltReason[0] = 0;

  Serial.println();
  rule();
  Serial.println(F("SELF-TEST"));
  rule();

  // --- 1. Power, and why we are here ---
  Serial.println(F("[1] POWER / RESET"));
  Serial.print(F("  last reset  : "));
  Serial.println(resetReasonName());
  if (esp_reset_reason() == ESP_RST_BROWNOUT)
  {
    Serial.println(F("  !! A brownout means the motor is dragging the rail down."));
    Serial.println(F("  !! That is what the 1000 uF across 12 V is for. Fit it."));
  }
  if (esp_reset_reason() == ESP_RST_PANIC || esp_reset_reason() == ESP_RST_TASK_WDT)
  {
    Serial.println(F("  !! The firmware crashed last run. Say so when reporting it."));
  }
  Serial.print(F("  free heap   : "));
  Serial.print(ESP.getFreeHeap());
  Serial.println(F(" bytes"));
  Serial.print(F("  CPU         : "));
  Serial.print(ESP.getCpuFreqMHz());
  Serial.println(F(" MHz"));

  // --- 2. Coin table sanity ---
  Serial.println(F("[2] COIN TABLE"));
  if (COIN_TABLE_LEN == 0)
  {
    Serial.println(F("  FAIL: table is empty. Nothing can ever be paid for."));
    strncpy(haltReason, "COIN TABLE EMPTY", sizeof(haltReason) - 1);
    ok = false;
  }
  for (uint8_t i = 0; i < COIN_TABLE_LEN; i++)
  {
    if (verbose)
    {
      Serial.print(F("  "));
      Serial.print(COIN_TABLE[i].pulses);
      Serial.print(COIN_TABLE[i].pulses == 1 ? F(" pulse  = ") : F(" pulses = "));
      Serial.print(COIN_TABLE[i].name);
      Serial.print(F("  (KSH "));
      Serial.print(COIN_TABLE[i].value);
      Serial.print(F(")"));
      if (COIN_TABLE[i].value == PAD_PRICE)
        Serial.print(F("   <- exact price"));
      Serial.println();
    }
    if (COIN_TABLE[i].pulses == 0)
    {
      Serial.println(F("  FAIL: a coin with 0 pulses can never be detected."));
      strncpy(haltReason, "BAD COIN TABLE", sizeof(haltReason) - 1);
      ok = false;
    }
    if (COIN_TABLE[i].value == 0)
    {
      Serial.print(F("  WARN: "));
      Serial.print(COIN_TABLE[i].name);
      Serial.println(F(" is worth nothing. Deliberate?"));
    }
    for (uint8_t j = i + 1; j < COIN_TABLE_LEN; j++)
    {
      if (COIN_TABLE[i].pulses == COIN_TABLE[j].pulses)
      {
        Serial.print(F("  FAIL: two coins share "));
        Serial.print(COIN_TABLE[i].pulses);
        Serial.println(F(" pulses. They are indistinguishable."));
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
    Serial.print(F("  WARN: no coin divides KSH "));
    Serial.print(PAD_PRICE);
    Serial.println(F(" exactly. Customers will always overpay."));
  }
  Serial.print(F("  price       : KSH "));
  Serial.println(PAD_PRICE);
  Serial.print(F("  change      : "));
  Serial.println(KEEP_CHANGE_AS_CREDIT ? F("kept on the meter")
                                       : F("FORFEIT — bench setting only"));

  // --- 3. Pulse decoding sanity ---
  Serial.println(F("[3] PULSE DECODING"));
  Serial.print(F("  debounce    : "));
  Serial.print(COIN_PULSE_DEBOUNCE_MS);
  Serial.println(F(" ms"));
  Serial.print(F("  coin gap    : "));
  Serial.print(COIN_GAP_MS);
  Serial.println(F(" ms"));
  if (COIN_GAP_MS <= COIN_PULSE_DEBOUNCE_MS * 2)
  {
    Serial.println(F("  FAIL: gap is too close to the debounce. One coin will be"));
    Serial.println(F("        read as several. Raise COIN_GAP_MS."));
    strncpy(haltReason, "BAD PULSE TIMING", sizeof(haltReason) - 1);
    ok = false;
  }
  Serial.print(F("  coin line   : GPIO "));
  Serial.print(PIN_COIN);
  Serial.print(F(" reads "));
  Serial.println(digitalRead(PIN_COIN) == HIGH ? F("HIGH (idle, correct)")
                                               : F("LOW  <- stuck, or acceptor is pulling it"));
  if (digitalRead(PIN_COIN) == LOW)
  {
    Serial.println(F("  !! A permanently LOW coin line means no coin will ever"));
    Serial.println(F("  !! register a falling edge. Check the wiring first."));
  }

  // --- 4. I2C and the screen ---
  Serial.println(F("[4] SCREEN"));
  i2cScan();
  lcdOk = i2cPresent(LCD_I2C_ADDR);
  Serial.print(F("  LCD at 0x"));
  Serial.print(LCD_I2C_ADDR, HEX);
  Serial.println(lcdOk ? F("  : present") : F("  : NOT FOUND"));
  if (!lcdOk)
  {
    Serial.println(F("  Running headless. The machine still vends, but the"));
    Serial.println(F("  customer is told nothing. Fix before any demo."));
    Serial.println(F("  Try the other address the scan found, in SECTION 4."));
  }

  // --- 5. IR beam ---
  Serial.println(F("[5] IR DROP SENSOR"));
  irRaw = irStable = false;
  uint32_t t0 = millis();
  while (millis() - t0 < 200)
    padSeen();
  Serial.print(F("  GPIO "));
  Serial.print(PIN_IR);
  Serial.print(F(" raw   : "));
  Serial.println(digitalRead(PIN_IR) == LOW ? F("LOW") : F("HIGH"));
  Serial.print(F("  polarity    : detected = "));
  Serial.println(IR_DETECTED_IS_LOW ? F("LOW") : F("HIGH"));
  Serial.print(F("  chute reads : "));
  Serial.println(irStable ? F("BLOCKED  <- problem") : F("clear    (correct when empty)"));
  // if (irStable)
  // {
  //   Serial.println(F("  A blocked chute at rest is one of three things:"));
  //   Serial.println(F("    - IR_DETECTED_IS_LOW is the wrong way round"));
  //   Serial.println(F("    - the sensor is aimed at nothing, or at the far wall"));
  //   Serial.println(F("    - there is genuinely a pad stuck in the chute"));
  //   Serial.println(F("  Use  i  to watch it live and wave your hand through."));
  // }

  if (irStable)
  {
    Serial.println(F("  FAIL: chute is BLOCKED at startup."));
    Serial.println(F("  Clear the chute and verify IR polarity/aiming."));
    strncpy(haltReason, "CHUTE BLOCKED", sizeof(haltReason) - 1);
    ok = false;
  }

  // --- 6. Relay and motor ---
  Serial.println(F("[6] RELAY / MOTOR"));
  Serial.print(F("  GPIO "));
  Serial.print(PIN_RELAY);
  Serial.print(F(", active "));
  Serial.println(RELAY_ACTIVE_HIGH ? F("HIGH") : F("LOW"));
  Serial.print(F("  dispense timeout : "));
  Serial.print(DISPENSE_TIMEOUT_MS);
  Serial.println(F(" ms"));
  Serial.print(F("  hard motor limit : "));
  Serial.print(motorHardLimitMs());
  Serial.println(F(" ms"));
  Serial.println(F("  Confirm the 10k pull-down on the relay pin is fitted."));
  if (SELFTEST_CLICKS_RELAY)
  {
    Serial.println(F("  Clicking the relay now — you should hear one click."));
    motorOn();
    delay(120);
    motorOff();
  }
  else
  {
    Serial.println(F("  Not clicking it. Set SELFTEST_CLICKS_RELAY once the"));
    Serial.println(F("  motor is disconnected, or just use  m500."));
  }

  // added during the additions of the jamming relay switch
  Serial.println(F("[7] COIN INHIBIT / CLEAR BUTTON"));
  Serial.print(F("  inhibit pin : GPIO "));
  Serial.print(PIN_INHIBIT);
  Serial.print(F(", active "));
  Serial.println(INHIBIT_RELAY_ACTIVE_HIGH ? F("HIGH") : F("LOW"));
  Serial.print(F("  contact     : "));
  if (INHIBIT_CONTACT_IS_NC)
  {
    Serial.println(F("NC — fail-safe, inhibited when unpowered"));
  }
  else
  {
    Serial.println(F("NO  <- NOT fail-safe"));
    Serial.println(F("  !! A dead board will accept coins and deliver nothing."));
    Serial.println(F("  !! Move to the NC contact before this leaves the bench."));
  }
  Serial.print(F("  coins now   : "));
  Serial.println(coinsAllowed ? F("enabled") : F("inhibited"));
  Serial.print(F("  clear button: GPIO "));
  Serial.print(PIN_CLEAR);
  Serial.print(F(" reads "));
  Serial.println(digitalRead(PIN_CLEAR) == HIGH ? F("HIGH (released, correct)")
                                                : F("LOW  <- held, shorted, or miswired"));
  if (digitalRead(PIN_CLEAR) == LOW)
  {
    Serial.println(F("  !! Held, shorted, or wired backwards. It will never"));
    Serial.println(F("  !! register a fresh press like this, so it cannot"));
    Serial.println(F("  !! clear a real fault. Use  x  until it's fixed."));
  }

  // --- 8. Buzzer ---
  Serial.println(F("[8] BUZZER"));
  Serial.println(F("  One short chirp now. Silence means check GPIO 25."));
  beep(TONE_COIN_HZ, TONE_COIN_MS);

  rule();
  Serial.println(ok ? F("SELF-TEST: PASS")
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
  Serial.println(F("COMMANDS"));
  rule();
  Serial.println(F("  ?        this list"));
  Serial.println(F("  s        status — state, credit, counters, loop health"));
  Serial.println(F("  d        run the full self-test again"));
  Serial.println(F("  c        print the coin table"));
  Serial.println(F("  z        scan the I2C bus"));
  Serial.println();
  Serial.println(F("  CALIBRATION (lost on reset — write the value into CONFIG)"));
  Serial.println(F("  p<ksh>   set the pad price          e.g.  p20"));
  Serial.println(F("  g<ms>    set the coin gap           e.g.  g250"));
  Serial.println(F("  o<ms>    set the dispense timeout   e.g.  o3000"));
  Serial.println();
  Serial.println(F("  BENCH TESTS"));
  Serial.println(F("  m<ms>    run the motor for <ms> — time one revolution"));
  Serial.println(F("  t        full test vend, no coin needed"));
  Serial.println(F("  v<n>     pretend a coin of <n> pulses went in"));
  Serial.println(F("  i        watch the IR sensor live (any key stops it)"));
  Serial.println(F("  l        LCD test pattern"));
  Serial.println(F("  b        buzzer test"));
  Serial.println();
  Serial.println(F("  x        clear a latched fault"));
  Serial.println(F("           (or hold the front-panel button for 1 s)"));
  Serial.println(F("  r        zero the session counters"));
  Serial.println(F("  h        toggle the heartbeat line"));
  rule();
}

void printCoinTable()
{
  rule();
  Serial.println(F("COIN TABLE — edit SECTION 2 to change it"));
  rule();
  for (uint8_t i = 0; i < COIN_TABLE_LEN; i++)
  {
    Serial.print(F("  "));
    Serial.print(COIN_TABLE[i].pulses);
    Serial.print(COIN_TABLE[i].pulses == 1 ? F(" pulse  -> ") : F(" pulses -> "));
    Serial.print(COIN_TABLE[i].name);
    Serial.print(F("  = KSH "));
    Serial.print(COIN_TABLE[i].value);
    if (COIN_TABLE[i].value >= PAD_PRICE)
    {
      Serial.print(F("   buys a pad outright"));
    }
    else if (COIN_TABLE[i].value > 0 && PAD_PRICE % COIN_TABLE[i].value == 0)
    {
      Serial.print(F("   x"));
      Serial.print(PAD_PRICE / COIN_TABLE[i].value);
      Serial.print(F(" buys a pad"));
    }
    Serial.println();
  }
  Serial.print(F("  PRICE: KSH "));
  Serial.println(PAD_PRICE);
  rule();
}

void printStatus()
{
  rule();
  Serial.println(F("STATUS"));
  rule();
  Serial.print(F("  firmware     : v"));
  Serial.println(F(FW_VERSION));
  Serial.print(F("  state        : "));
  Serial.println(stateName(state));
  Serial.print(F("  credit       : KSH "));
  Serial.print(credit);
  Serial.print(F(" of KSH "));
  Serial.println(PAD_PRICE);
  Serial.print(F("  motor        : "));
  Serial.println(motorRunning ? F("RUNNING") : F("off"));
  Serial.print(F("  chute beam   : "));
  Serial.println(irStable ? F("BLOCKED") : F("clear"));
  Serial.print(F("  screen       : "));
  Serial.println(lcdOk ? F("ok") : F("MISSING — headless"));
  Serial.println();
  Serial.print(F("  vends        : "));
  Serial.println(vendCount);
  Serial.print(F("  coins ok     : "));
  Serial.println(coinsAccepted);
  Serial.print(F("  coins unknown: "));
  Serial.println(coinsUnknown);
  Serial.print(F("  taken        : KSH "));
  Serial.println(shillingsIn);
  Serial.print(F("  faults       : "));
  Serial.println(faultCount);
  Serial.print(F("  raw pulses   : "));
  Serial.println(isrTotal);
  Serial.println();
  Serial.print(F("  uptime       : "));
  Serial.print((millis() - bootMs) / 1000);
  Serial.println(F(" s"));
  Serial.print(F("  free heap    : "));
  Serial.println(ESP.getFreeHeap());
  Serial.print(F("  loop max     : "));
  Serial.print(loopMaxUs);
  Serial.println(F(" us"));
  Serial.println(F("  (loop max over ~50000 us means I2C is stalling)"));
  Serial.print(F("  coin acceptor: "));
  Serial.println(coinsAllowed ? F("enabled") : F("INHIBITED"));
  rule();
}

void runMotorFor(uint32_t ms)
{
  if (ms == 0 || ms > 10000)
  {
    Serial.println(F("Refusing. Use 1 to 10000 ms."));
    return;
  }
  Serial.print(F("Motor ON for "));
  Serial.print(ms);
  Serial.println(F(" ms..."));
  say("MOTOR TEST", "");
  uint32_t t0 = millis();
  motorOn();
  bool broke = false;
  while (millis() - t0 < ms)
  {
    if (padSeen() && !broke)
    {
      broke = true;
      Serial.print(F("  beam broken at "));
      Serial.print(millis() - t0);
      Serial.println(F(" ms"));
    }
    buzzerService();
    delay(1); // this can run for ten seconds; give the watchdog its turn
  }
  motorOff();
  Serial.println(F("Motor OFF."));
  if (!broke)
    Serial.println(F("  beam never broke during the run"));
  Serial.println(F("Time one full revolution this way, then set"));
  Serial.println(F("DISPENSE_TIMEOUT_MS to about double it  (o<ms>)."));
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
    Serial.println(F("Refusing to start: the beam already reads BLOCKED."));
    enterFault("CHECK SENSOR");
    return false;
  }
  say("DISPENSING...", "");
  Serial.print(F("Motor on — "));
  Serial.println(why);
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

  // Any command other than 'i' itself stops the IR watch, so its chatter
  // never buries the answer you just asked for.
  if (irWatch && c != 'i')
  {
    irWatch = false;
    Serial.println(F("IR watch off."));
  }

  switch (c)
  {
  case '?':
    printHelp();
    break;

  case 'h':
    heartbeatOn = !heartbeatOn;
    Serial.print(F("Heartbeat "));
    Serial.println(heartbeatOn ? F("ON") : F("off"));
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
    Serial.println(F("Chirp."));
    beep(TONE_VEND_HZ, TONE_VEND_MS);
    break;

  case 'l':
    if (!lcdOk)
    {
      Serial.println(F("No LCD found. Run  z  to scan."));
      break;
    }
    Serial.println(F("LCD: both lines filled, then back to idle."));
    say("0123456789ABCDEF", "BRICAPA  v" FW_VERSION);
    holdFor(2000);
    showIdle();
    break;

  case 'i':
    irWatch = !irWatch;
    Serial.println(irWatch ? F("IR watch ON — wave a pad through the chute.")
                           : F("IR watch off."));
    break;

  case 'x':
    attemptClear("serial console");
    break;

  case 'r':
    vendCount = coinsAccepted = coinsUnknown = faultCount = shillingsIn = 0;
    isrTotal = 0;
    loopMaxUs = 0;
    Serial.println(F("Counters zeroed."));
    break;

  case 'p':
    if (n <= 0 || n > 1000)
    {
      Serial.println(F("Use p1 to p1000."));
      break;
    }
    PAD_PRICE = (uint16_t)n;
    Serial.print(F("Price is now KSH "));
    Serial.println(PAD_PRICE);
    Serial.println(F("Not saved. Put it in SECTION 3 to keep it."));
    showIdle();
    break;

  case 'g':
    if (n <= 0 || n > 3000)
    {
      Serial.println(F("Use g1 to g3000."));
      break;
    }
    COIN_GAP_MS = (uint32_t)n;
    Serial.print(F("Coin gap is now "));
    Serial.print(COIN_GAP_MS);
    Serial.println(F(" ms"));
    if (COIN_GAP_MS <= COIN_PULSE_DEBOUNCE_MS * 2)
      Serial.println(F("WARNING: that is too close to the debounce."));
    Serial.println(F("Not saved. Put it in SECTION 5 to keep it."));
    break;

  case 'o':
    if (n <= 0 || n > 30000)
    {
      Serial.println(F("Use o1 to o30000."));
      break;
    }
    DISPENSE_TIMEOUT_MS = (uint32_t)n;
    Serial.print(F("Dispense timeout is now "));
    Serial.print(DISPENSE_TIMEOUT_MS);
    Serial.println(F(" ms"));
    Serial.print(F("Hard motor limit follows it at "));
    Serial.print(motorHardLimitMs());
    Serial.println(F(" ms"));
    Serial.println(F("Not saved. Put it in SECTION 5 to keep it."));
    break;

  case 'm':
    if (state == ST_FAULT)
    {
      Serial.println(F("Faulted. Clear with  x  first."));
      break;
    }
    if (state != ST_IDLE)
    {
      Serial.println(F("Busy. Wait for IDLE."));
      break;
    }
    runMotorFor(n < 0 ? 500 : (uint32_t)n);
    break;

  case 't':
    if (state == ST_FAULT)
    {
      Serial.println(F("Faulted. Clear with  x  first."));
      break;
    }
    if (state != ST_IDLE)
    {
      Serial.println(F("Busy. Wait for IDLE."));
      break;
    }
    Serial.println(F("Test vend — no coin taken, no money counted."));
    startVend("test vend from console");
    break;

  case 'v':
  {
    if (n <= 0)
    {
      Serial.println(F("Use v1, v2, ... to fake a coin."));
      break;
    }
    Serial.print(F("Simulating a "));
    Serial.print(n);
    Serial.println(F("-pulse coin."));
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
    Serial.print(F("Unknown command '"));
    Serial.print(s);
    Serial.println(F("'. Type ? for the list."));
  }
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

  Serial.println();
  rule();
  Serial.println(F("  BRICAPA PAD DISPENSER  -  prototype firmware v" FW_VERSION));
  Serial.println(F("  Ref BT-PADVEND-2026-001"));
  rule();
  Serial.println(F("  Type ? for commands. Nothing is saved to flash."));
  rule();

  pinMode(PIN_COIN, INPUT_PULLUP);
  pinMode(PIN_CLEAR, INPUT_PULLUP);
  pinMode(PIN_IR, INPUT);

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
    Serial.println();
    Serial.println(F("HALTED. The self-test found something that would make this"));
    Serial.println(F("machine take money and do the wrong thing with it."));
    Serial.println(F("The coin input is left DETACHED, so it cannot bank money."));
    Serial.println(F("Fix CONFIG and reflash. The console stays alive."));
    state = ST_FAULT;
    return; // no attachInterrupt: coins cannot be counted in this condition
  }

  say("PAD DISPENSER", "BRICAPA  v" FW_VERSION);
  holdFor(1200);

  attachCoinIsr();

  showIdle();
  Serial.println(F("Ready. Waiting for a coin."));
  rule();
}

// ===========================================================================
//  LOOP
// ===========================================================================
void loop()
{
  uint32_t loopStartUs = micros();
  uint32_t now = millis();

  buzzerService();
  pollSerial();
  clearButtonService();

  // ---- Backstop. Checked in every state, before anything else can run. ----
  // Nothing below this line is trusted to stop the motor on its own.
  if (motorRunning && (now - motorOnSince) > motorHardLimitMs())
  {
    motorOff();
    Serial.println(F("!! Hard motor limit hit. The state machine did not stop it."));
    Serial.println(F("!! That is a firmware bug, not a jam. Report it."));
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
      Serial.print(F("IR: raw="));
      Serial.print(digitalRead(PIN_IR) == LOW ? F("LOW ") : F("HIGH"));
      Serial.print(F("  debounced="));
      Serial.println(v ? F("BLOCKED") : F("clear"));
    }
  }

  // ---- A pad sitting in the beam while idle is worth saying out loud ----
  if (state == ST_IDLE && irStable && !irStuckWarned &&
      (now - irBlockedSince) > IR_STUCK_WARN_MS)
  {
    irStuckWarned = true;
    Serial.println(F("WARN: the chute has read BLOCKED for a while at rest."));
    Serial.println(F("      Something is in it, or the sensor needs re-aiming."));
    Serial.println(F("      The next paid vend will fault rather than run blind."));
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
      Serial.print(F("Partial credit of KSH "));
      Serial.print(credit);
      Serial.println(F(" timed out, cleared."));
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
      Serial.print(F("PAD DELIVERED in "));
      Serial.print(took);
      Serial.println(F(" ms"));
      Serial.print(F("  session vends : "));
      Serial.println(vendCount);
      Serial.print(F("  credit left   : KSH "));
      Serial.println(credit);
      if (took < 150)
      {
        Serial.println(F("  NOTE: that was very fast. Either the beam was"));
        Serial.println(F("  already breaking, or the sensor sees the coil."));
      }
      rule();
    }
    else if ((now - dispenseStartMs) > DISPENSE_TIMEOUT_MS)
    {
      // Coil turned but nothing fell: empty, or a pad is wedged.
      Serial.print(F("No pad in "));
      Serial.print(DISPENSE_TIMEOUT_MS);
      Serial.println(F(" ms."));
      Serial.println(F("  Coil empty, pad wedged, or the timeout is too short"));
      Serial.println(F("  for this motor. Time a revolution with  m1000."));
      enterFault("EMPTY OR JAMMED");
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
      Serial.println(F("Ready."));
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
    Serial.print(F("[hb] "));
    Serial.print((now - bootMs) / 1000);
    Serial.print(F("s  "));
    Serial.print(stateName(state));
    Serial.print(F("  credit KSH "));
    Serial.print(credit);
    Serial.print(F("  vends "));
    Serial.print(vendCount);
    Serial.print(F("  faults "));
    Serial.print(faultCount);
    Serial.print(F("  beam "));
    Serial.print(irStable ? F("BLOCKED") : F("clear"));
    Serial.print(F("  heap "));
    Serial.println(ESP.getFreeHeap());
  }

  // ---- Loop health ----
  uint32_t took = micros() - loopStartUs;
  if (took > loopMaxUs)
    loopMaxUs = took;

  // Hand the CPU back. The idle task needs it to feed the watchdog, and 1 ms
  // is well inside the IR debounce window so nothing is missed.
  delay(1);
}
