/*
  Pads Vending Machine — Coin Acceptor Prototype
  Bricapa Technologies · Client: Rebecca Mpiima
  Ref: BT-PADVEND-2026-001

  ---------------------------------------------------------------------------
  WHAT THIS DOES
  ---------------------------------------------------------------------------
  Coin in  ->  acceptor pulses  ->  relay closes  ->  motor turns the coil
           ->  pad falls through the chute and breaks the IR beam
           ->  motor stops.  LCD and buzzer report each step.

  If no pad breaks the beam within DISPENSE_TIMEOUT_MS the motor stops anyway
  and the machine latches into a fault. It does NOT reverse and does NOT retry
  by itself — the relay drives one direction only, which is a deliberate design
  decision agreed with the client.

  ---------------------------------------------------------------------------
  WIRING
  ---------------------------------------------------------------------------
    12 V adaptor ─┬─ LM2596 buck → 5.0 V → Schottky → ESP32 VIN
                  │                          └─ ESP32 3V3 pin → KY-032 VCC
                  ├─ coin acceptor  (12 V)
                  └─ relay COM/NO   → motor  (flyback diode across motor)

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
  CALIBRATE THESE ON THE BENCH — see README.md
  ---------------------------------------------------------------------------
    PULSES_PER_VEND    depends on how you program the acceptor in learn mode
    IR_DETECTED_IS_LOW depends on your sensor and how you mount it
    LCD_I2C_ADDR       0x27 or 0x3F — run an I2C scanner if the screen is blank
    DISPENSE_TIMEOUT_MS  set from the measured time for one revolution + margin
*/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>

// ===========================================================================
//  PIN MAP
// ===========================================================================
const uint8_t PIN_COIN   = 18;
const uint8_t PIN_RELAY  = 26;
const uint8_t PIN_IR     = 27;
const uint8_t PIN_BUZZER = 25;
const uint8_t PIN_SDA    = 21;
const uint8_t PIN_SCL    = 22;

// ===========================================================================
//  CONFIGURATION — the values you will actually change
// ===========================================================================

// How many pulses the acceptor sends for one KSh 20 coin.
// Program the acceptor for ONE pulse per coin and leave this at 1. That is the
// simplest arrangement and all this prototype needs. Raise it only if you
// deliberately program a higher pulse count, or later teach a second coin.
const uint8_t  PULSES_PER_VEND = 1;

// true  = sensor output goes LOW when it sees the pad  (usual for KY-032)
// false = sensor output goes HIGH when it sees the pad
// If you use the "aim across the chute at the far wall" trick, this inverts.
const bool     IR_DETECTED_IS_LOW = true;

// Relay module polarity. This build uses an active-high module.
const bool     RELAY_ACTIVE_HIGH = true;

// I2C address of the LCD backpack. Almost always 0x27, sometimes 0x3F.
const uint8_t  LCD_I2C_ADDR = 0x27;

// ---- Timing ----
const uint32_t COIN_DEBOUNCE_MS    = 50;    // acceptor needs 0.6 s/coin, so 50 ms is safe
const uint32_t CREDIT_TIMEOUT_MS   = 20000; // drop partial credit after this long idle
const uint32_t DISPENSE_TIMEOUT_MS = 3000;  // give up waiting for the pad (jam or empty)
const uint32_t IR_DEBOUNCE_MS      = 15;    // ignore beam flicker shorter than this
const uint32_t MESSAGE_HOLD_MS     = 2500;  // how long "TAKE YOUR PAD" stays up
const uint32_t IR_SETTLE_MS        = 150;   // beam must read clear this long before we start

// ---- Buzzer. Keep it quiet and short: this sits in a public washroom. ----
const uint16_t TONE_COIN_HZ   = 2200;  const uint16_t TONE_COIN_MS   = 60;
const uint16_t TONE_VEND_HZ   = 1800;  const uint16_t TONE_VEND_MS   = 150;
const uint16_t TONE_FAULT_HZ  = 350;   const uint16_t TONE_FAULT_MS  = 500;

// ===========================================================================
//  STATE
// ===========================================================================
enum State { ST_IDLE, ST_DISPENSING, ST_DELIVERED, ST_FAULT };
State state = ST_IDLE;

LiquidCrystal_I2C lcd(LCD_I2C_ADDR, 16, 2);
Preferences prefs;

// Coin pulses are counted in an ISR and drained in loop().
volatile uint32_t isrPulses  = 0;
volatile uint32_t isrLastMs  = 0;
volatile bool     isrSeenOne = false;
portMUX_TYPE coinMux = portMUX_INITIALIZER_UNLOCKED;

uint32_t credit          = 0;   // pulses banked toward the next vend
uint32_t lastCoinMs      = 0;
uint32_t dispenseStartMs = 0;
uint32_t messageUntilMs  = 0;

// Debounced IR state
bool     irRaw           = false;
bool     irStable        = false;
uint32_t irChangedMs     = 0;

uint32_t vendCount       = 0;   // lifetime, survives power loss

// millis() wraps after ~49.7 days and this machine is meant to run for months,
// so never compare two timestamps directly. Signed difference is wrap-safe.
static inline bool reached(uint32_t now, uint32_t deadline) {
  return (int32_t)(now - deadline) >= 0;
}

// ===========================================================================
//  ISR
// ===========================================================================
void IRAM_ATTR onCoinPulse() {
  uint32_t now = millis();
  portENTER_CRITICAL_ISR(&coinMux);
  // isrLastMs starts at 0, so the guard below would swallow a coin dropped in
  // the first COIN_DEBOUNCE_MS after boot. isrSeenOne makes the first pulse
  // unconditional and debounces every one after it.
  if (!isrSeenOne || (now - isrLastMs) >= COIN_DEBOUNCE_MS) {
    isrSeenOne = true;
    isrPulses++;
    isrLastMs = now;
  }
  portEXIT_CRITICAL_ISR(&coinMux);
}

uint32_t drainPulses() {
  portENTER_CRITICAL(&coinMux);
  uint32_t n = isrPulses;
  isrPulses = 0;
  portEXIT_CRITICAL(&coinMux);
  return n;
}

// ===========================================================================
//  HARDWARE HELPERS
// ===========================================================================
void motorOn()  { digitalWrite(PIN_RELAY, RELAY_ACTIVE_HIGH ? HIGH : LOW);  }
void motorOff() { digitalWrite(PIN_RELAY, RELAY_ACTIVE_HIGH ? LOW  : HIGH); }

void beep(uint16_t hz, uint16_t ms) { tone(PIN_BUZZER, hz, ms); }

// Every message must fit 16 characters.
void say(const char* l1, const char* l2 = "") {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(l1);
  lcd.setCursor(0, 1); lcd.print(l2);
}

// Reads the sensor and returns true only once a change has held for IR_DEBOUNCE_MS.
bool padSeen() {
  bool level = (digitalRead(PIN_IR) == LOW);
  bool seen  = IR_DETECTED_IS_LOW ? level : !level;
  uint32_t now = millis();

  if (seen != irRaw) {
    irRaw = seen;
    irChangedMs = now;
  } else if (irRaw != irStable && (now - irChangedMs) >= IR_DEBOUNCE_MS) {
    irStable = irRaw;
  }
  return irStable;
}

void saveVendCount() {
  prefs.begin("padvend", false);
  prefs.putUInt("vends", vendCount);
  prefs.end();
}

void enterFault(const char* line2) {
  motorOff();
  say("OUT OF ORDER", line2);
  beep(TONE_FAULT_HZ, TONE_FAULT_MS);
  state = ST_FAULT;
  Serial.print(F("FAULT: ")); Serial.println(line2);
  Serial.println(F("Latched. Power-cycle to clear."));
}

void showIdle() {
  say("INSERT KSH 20");
}

// ===========================================================================
//  SETUP
// ===========================================================================
void setup() {
  // Relay first and before anything else, so the motor cannot twitch on boot.
  pinMode(PIN_RELAY, OUTPUT);
  motorOff();

  Serial.begin(115200);
  delay(200);
  Serial.println(F("\n=== Bricapa Pad Dispenser — prototype ==="));

  pinMode(PIN_COIN, INPUT_PULLUP);
  pinMode(PIN_IR, INPUT);
  pinMode(PIN_BUZZER, OUTPUT);

  Wire.begin(PIN_SDA, PIN_SCL);
  lcd.init();
  lcd.backlight();

  prefs.begin("padvend", true);
  vendCount = prefs.getUInt("vends", 0);
  prefs.end();

  Serial.print(F("Lifetime vends: ")); Serial.println(vendCount);
  Serial.print(F("Pulses per vend: ")); Serial.println(PULSES_PER_VEND);

  char l2[17];
  snprintf(l2, sizeof(l2), "VENDS: %lu", (unsigned long)vendCount);
  say("PAD DISPENSER", l2);
  delay(1500);

  // Prime the IR debouncer so the first reading is honest.
  irRaw = irStable = false;
  padSeen();

  attachInterrupt(digitalPinToInterrupt(PIN_COIN), onCoinPulse, FALLING);

  showIdle();
  Serial.println(F("Ready. Waiting for a coin."));
}

// ===========================================================================
//  LOOP
// ===========================================================================
void loop() {
  uint32_t now = millis();

  // ---- Coins are counted in every state, but only banked when idle. ----
  uint32_t newPulses = drainPulses();
  if (newPulses) {
    Serial.print(F("Coin pulse x")); Serial.println(newPulses);
    if (state == ST_IDLE) {
      credit += newPulses;
      lastCoinMs = now;
      beep(TONE_COIN_HZ, TONE_COIN_MS);
      if (credit < PULSES_PER_VEND) {
        char l1[17];
        snprintf(l1, sizeof(l1), "CREDIT %lu/%u", (unsigned long)credit, PULSES_PER_VEND);
        say(l1, "INSERT MORE");
      }
    } else {
      // Mid-vend or faulted. Banking it now would owe a pad we cannot promise.
      Serial.println(F("  ignored (machine busy or faulted)"));
    }
  }

  switch (state) {

    // -----------------------------------------------------------------
    case ST_IDLE: {
      // Forget stale part-payment so the next customer starts clean.
      if (credit > 0 && credit < PULSES_PER_VEND && (now - lastCoinMs) > CREDIT_TIMEOUT_MS) {
        Serial.println(F("Partial credit timed out, cleared."));
        credit = 0;
        showIdle();
      }

      if (credit >= PULSES_PER_VEND) {
        // Refuse to run the motor blind. If the beam already reads "pad", the
        // sensor is mis-wired, mis-aimed, or something is stuck in the chute.
        padSeen();
        uint32_t t0 = millis();
        bool clear = true;
        while (millis() - t0 < IR_SETTLE_MS) {
          if (padSeen()) { clear = false; break; }
        }
        if (!clear) {
          enterFault("CHECK SENSOR");
          break;
        }

        credit -= PULSES_PER_VEND;
        say("DISPENSING...");
        Serial.println(F("Credit met. Motor on."));
        motorOn();
        dispenseStartMs = millis();
        state = ST_DISPENSING;
      }
      break;
    }

    // -----------------------------------------------------------------
    case ST_DISPENSING: {
      if (padSeen()) {
        motorOff();
        uint32_t took = millis() - dispenseStartMs;
        vendCount++;
        saveVendCount();
        say("TAKE YOUR PAD");
        beep(TONE_VEND_HZ, TONE_VEND_MS);
        messageUntilMs = millis() + MESSAGE_HOLD_MS;
        state = ST_DELIVERED;
        Serial.print(F("Pad delivered in ")); Serial.print(took);
        Serial.print(F(" ms. Lifetime vends: ")); Serial.println(vendCount);
      }
      else if ((now - dispenseStartMs) > DISPENSE_TIMEOUT_MS) {
        // Coil turned but nothing fell: empty, or a pad is wedged.
        enterFault("EMPTY OR JAMMED");
      }
      break;
    }

    // -----------------------------------------------------------------
    case ST_DELIVERED: {
      // Let the beam clear as the pad finishes falling.
      padSeen();
      if (reached(now, messageUntilMs)) {
        showIdle();
        state = ST_IDLE;
        Serial.println(F("Ready."));
      }
      break;
    }

    // -----------------------------------------------------------------
    case ST_FAULT: {
      // Latched on purpose. No reverse, no self-retry — someone looks at it.
      break;
    }
  }
}
