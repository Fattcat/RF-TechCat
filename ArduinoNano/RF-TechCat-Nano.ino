#include <RCSwitch.h>
#include <EEPROM.h>

// === KONFIGURÁCIA PINOV ===
#define LED_PIN          13   // zabudovaná LED
#define RX_INTERRUPT     0    // D2 = interrupt 0
#define TX_PIN           12   // D12 pre odosielanie
#define MASTER_CLEAR_PIN 11   // ✅ Nové tlačidlo pre vymazanie VŠETKÉHO


// !! NOT WORKING !!

// === TLAČIDLÁ ===
#define BUTTON_COUNT     8
const byte buttonPins[BUTTON_COUNT] = {10, 3, 4, 5, 6, 7, 8, 9}; // D10, D3, D4...D9

// === ŠTRUKTÚRA KÓDU ===
struct CodeData {
  unsigned long code;
  unsigned int length;
  unsigned int protocol;
};

// === EEPROM ===
#define EEPROM_START_ADDR 0

// === ČASOVÉ KONŠTANTY ===
const unsigned long LEARNING_TIMEOUT = 30000;
const unsigned long CLEAR_TIMEOUT = 10000;

// === GLOBÁLNE PREMENNÉ ===
RCSwitch mySwitch = RCSwitch();
CodeData storedCodes[BUTTON_COUNT];

unsigned long buttonPressStart[BUTTON_COUNT] = {0};
bool lastButtonState[BUTTON_COUNT] = {false};
unsigned long lastDebounceTime[BUTTON_COUNT] = {0};

bool inLearningMode = false;
int learningButtonIndex = -1;
unsigned long learningStartTime = 0;

unsigned long lastBlinkTime = 0;
bool ledState = false;
int blinkMode = 0;

// === MASTER CLEAR PREMENNÉ ===
unsigned long masterClearPressStart = 0;
bool lastMasterClearState = true; // pull-up → default HIGH

// === PROTOTYPY ===
void loadCodesFromEEPROM();
void saveCodeToEEPROM(int index, unsigned long code, unsigned int length, unsigned int protocol);
void blinkLED(int mode);
void errorBlink();
void exitLearningMode();

// === SETUP ===
void setup() {
  Serial.begin(9600);
  Serial.println("\n\n✅ 433MHz Universal Remote - STARTING...");

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);
  delay(300);
  digitalWrite(LED_PIN, LOW);
  delay(200);
  digitalWrite(LED_PIN, HIGH);
  delay(300);
  digitalWrite(LED_PIN, LOW);

  // Inicializácia tlačidiel
  for (int i = 0; i < BUTTON_COUNT; i++) {
    pinMode(buttonPins[i], INPUT_PULLUP);
    lastButtonState[i] = !digitalRead(buttonPins[i]);
  }

  // ✅ Inicializácia MASTER CLEAR tlačidla
  pinMode(MASTER_CLEAR_PIN, INPUT_PULLUP);

  mySwitch.enableReceive(RX_INTERRUPT);
  mySwitch.enableTransmit(TX_PIN);
  mySwitch.setRepeatTransmit(3);
  mySwitch.setReceiveTolerance(100);

  loadCodesFromEEPROM();

  Serial.println("🟢 Systém pripravený.");
  Serial.println("📚 Stlač tlačidlo na 5s → naučiť kód");
  Serial.println("🗑️  Stlač tlačidlo na 10s → vymazať kód (ak existuje)");
  Serial.println("🧨 Stlač D11 na 10s → vymazať VŠETKO");
  Serial.println("📡 Debug RX beží — všetky signály sa zobrazia tu.");
}

// === HLAVNÝ LOOP ===
void loop() {

  if (mySwitch.available()) {
    unsigned long receivedCode = mySwitch.getReceivedValue();
    if (receivedCode == 0) {
      Serial.println("⚠️  Zachytený signál, ale nebol dekódovaný.");
    } else {
      Serial.print("📡 PRIJATÝ SIGNÁL: 0x");
      Serial.print(receivedCode, HEX);
      Serial.print(" | bitov: ");
      Serial.print(mySwitch.getReceivedBitlength());
      Serial.print(" | protokol: ");
      Serial.println(mySwitch.getReceivedProtocol());
    }
    mySwitch.resetAvailable();
  }

  // Čítanie bežných tlačidiel
  for (int i = 0; i < BUTTON_COUNT; i++) {
    bool raw = digitalRead(buttonPins[i]);
    if (raw != lastButtonState[i]) {
      lastDebounceTime[i] = millis();
    }

    if ((millis() - lastDebounceTime[i]) > 50) {
      bool current = !raw;

      if (current && !lastButtonState[i]) {
        buttonPressStart[i] = millis();
        Serial.print("🔘 Tlačidlo "); Serial.print(i+1); Serial.println(" stlačené");
      } 
      else if (!current && lastButtonState[i]) {
        unsigned long pressDuration = millis() - buttonPressStart[i];
        Serial.print("🔘 Tlačidlo "); Serial.print(i+1);
        Serial.print(" uvoľnené po "); Serial.print(pressDuration); Serial.println("ms");

        bool handled = false;

        // ✅ VYMAZANIE KÓDU (10s)
        if (pressDuration >= CLEAR_TIMEOUT && !inLearningMode && storedCodes[i].code != 0) {
          storedCodes[i].code = 0;
          storedCodes[i].length = 0;
          storedCodes[i].protocol = 0;
          saveCodeToEEPROM(i, 0, 0, 0);

          Serial.print("🗑️  Kód z tlačidla ");
          Serial.print(i+1);
          Serial.println(" bol vymazaný!");

          for (int b = 0; b < 10; b++) {
            digitalWrite(LED_PIN, HIGH);
            delay(100);
            digitalWrite(LED_PIN, LOW);
            delay(100);
          }

          handled = true;
        }
        // ✅ REŽIM UČENIA (5s)
        else if (!handled && pressDuration >= 5000 && !inLearningMode) {
          inLearningMode = true;
          learningButtonIndex = i;
          learningStartTime = millis();
          blinkMode = 2;

          Serial.print("🚀 Tlačidlo ");
          Serial.print(i+1);
          Serial.println(" vstúpilo do LEARNING MODE...");
          Serial.println("⏳ Zachytávam signál... (máš 30 sekúnd)");

          digitalWrite(LED_PIN, HIGH);
          delay(100);
          digitalWrite(LED_PIN, LOW);

          handled = true;
        }
        // ✅ KRÁTKE STLAČENIE
        else if (!handled && pressDuration < 5000) {
          if (storedCodes[i].code != 0) {
            mySwitch.setProtocol(storedCodes[i].protocol);
            mySwitch.send(storedCodes[i].code, storedCodes[i].length);

            Serial.print("📤 Odosielam: kód=");
            Serial.print(storedCodes[i].code);
            Serial.print(", bitov=");
            Serial.print(storedCodes[i].length);
            Serial.print(", protokol=");
            Serial.println(storedCodes[i].protocol);

            digitalWrite(LED_PIN, HIGH);
            delay(200);
            digitalWrite(LED_PIN, LOW);
          } else {
            Serial.print("❌ Tlačidlo "); Serial.print(i+1); Serial.println(" nemá uložený kód!");
            errorBlink();
          }
        }

        lastButtonState[i] = current;
      }
    }
  }

  // ✅ MASTER CLEAR: Vymazanie VŠETKÉHO cez D11
  bool rawMaster = digitalRead(MASTER_CLEAR_PIN);
  if (rawMaster != lastMasterClearState) {
    lastDebounceTime[0] = millis(); // použi rovnaký debounce čas
  }

  if ((millis() - lastDebounceTime[0]) > 50) {
    bool currentMaster = !rawMaster; // pre INPUT_PULLUP

    // Keď bolo tlačidlo stlačené a následne uvoľnené
    if (!currentMaster && lastMasterClearState) {
      Serial.println("🧨 VYMAZÁVAM VŠETKY KÓDY Z EEPROM!");

      // Vymaž v RAM
      for (int i = 0; i < BUTTON_COUNT; i++) {
        storedCodes[i].code = 0;
        storedCodes[i].length = 0;
        storedCodes[i].protocol = 0;
      }

      // Vymaž v EEPROM
      for (int i = 0; i < BUTTON_COUNT; i++) {
        saveCodeToEEPROM(i, 0, 0, 0);
      }

      Serial.println("✅ VŠETKY KÓDY BOLI VYMAZANÉ!");

      // 10x rýchle bliknutie LED
      for (int b = 0; b < 10; b++) {
        digitalWrite(LED_PIN, HIGH);
        delay(100);
        digitalWrite(LED_PIN, LOW);
        delay(100);
      }
    }
    lastMasterClearState = currentMaster;
  }

  // Ak sme v režime učenia
  if (inLearningMode) {
    unsigned long elapsed = millis() - learningStartTime;

    static unsigned long lastTimePrint = 0;
    if (elapsed - lastTimePrint > 5000) {
      lastTimePrint = elapsed;
      Serial.print("⏱️  Uplynulo: ");
      Serial.print(elapsed / 1000);
      Serial.println("s z 30s");
    }

    if (elapsed > LEARNING_TIMEOUT) {
      Serial.println("⏰ Čas vypršal — režim učenia zrušený.");
      exitLearningMode();
    }

    if (mySwitch.available()) {
      unsigned long receivedCode = mySwitch.getReceivedValue();
      unsigned int bitLength = mySwitch.getReceivedBitlength();
      unsigned int protocol = mySwitch.getReceivedProtocol();

      if (receivedCode != 0) {
        Serial.println("");
        Serial.print("✅ Zachytený kód: ");
        Serial.print(receivedCode);
        Serial.print(", HEX: 0x");
        Serial.println(receivedCode, HEX);

        storedCodes[learningButtonIndex].code = receivedCode;
        storedCodes[learningButtonIndex].length = bitLength;
        storedCodes[learningButtonIndex].protocol = protocol;

        saveCodeToEEPROM(learningButtonIndex, receivedCode, bitLength, protocol);

        exitLearningMode();
        Serial.println("💾 Kód úspešne uložený do EEPROM!");
      }
      mySwitch.resetAvailable();
    }
  }

  blinkLED(blinkMode);
}

// === UKONČENIE REŽIMU UČENIA ===
void exitLearningMode() {
  inLearningMode = false;
  learningButtonIndex = -1;
  blinkMode = 0;
  digitalWrite(LED_PIN, LOW);
}

// === NAČÍTANIE KÓDOV Z EEPROM ===
void loadCodesFromEEPROM() {
  for (int i = 0; i < BUTTON_COUNT; i++) {
    int addr = EEPROM_START_ADDR + (i * sizeof(CodeData));
    EEPROM.get(addr, storedCodes[i]);

    if (storedCodes[i].code == 0xFFFFFFFF || storedCodes[i].code == 0) {
      storedCodes[i].code = 0;
    }

    Serial.print("💾 Tlačidlo ");
    Serial.print(i + 1);
    Serial.print(": ");
    if (storedCodes[i].code) {
      Serial.print("Uložený kód: ");
      Serial.print(storedCodes[i].code);
      Serial.print(", HEX: 0x");
      Serial.print(storedCodes[i].code, HEX);
      Serial.print(", bitov: ");
      Serial.print(storedCodes[i].length);
      Serial.print(", protokol: ");
      Serial.println(storedCodes[i].protocol);
    } else {
      Serial.println("PRÁZDNE");
    }
  }
}

// === ULOŽENIE KÓDU DO EEPROM ===
void saveCodeToEEPROM(int index, unsigned long code, unsigned int length, unsigned int protocol) {
  CodeData data;
  data.code = code;
  data.length = length;
  data.protocol = protocol;

  int addr = EEPROM_START_ADDR + (index * sizeof(CodeData));
  EEPROM.put(addr, data);
}

// === BLIKANIE LED ===
void blinkLED(int mode) {
  if (mode == 0) {
    digitalWrite(LED_PIN, LOW);
    return;
  }

  unsigned long interval = (mode == 1) ? 500 : 100;

  if (millis() - lastBlinkTime > interval) {
    lastBlinkTime = millis();
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState);
  }
}

// === CHYBOVÉ BLIKANIE ===
void errorBlink() {
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(300);
    digitalWrite(LED_PIN, LOW);
    delay(300);
  }
}
