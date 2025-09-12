#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <SmartRC_CC1101.h>
#include <EEPROM.h>

// === WiFi nastavenia ===
const char* ssid = "ESP32_Control";
const char* password = "12345678";

// === CC1101 nastavenia ===
#define CC1101_CS_PIN   5
#define CC1101_GDO0_PIN 21
SmartRC_CC1101 cc1101(CC1101_CS_PIN, CC1101_GDO0_PIN);

// === Ukladanie signálov ===
#define MAX_CODE_LENGTH 64  // Max 64 bajtov = 512 bitov
#define MAX_CODES 20
#define CODE_ITEM_SIZE (1 + MAX_CODE_LENGTH + 32)  // dĺžka + dáta + meno
#define EEPROM_SIZE (MAX_CODES * CODE_ITEM_SIZE)

struct CodeItem {
  uint8_t length;           // Počet bajtov signálu (1–64)
  uint8_t data[MAX_CODE_LENGTH]; // Samotné dáta (RF signál)
  char name[32];            // Názov (32 znakov)
};

CodeItem savedCodes[MAX_CODES];
int codeCount = 0;

// === Globálne premenné ===
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

bool isReceiving = false;
unsigned long receiveStartTime = 0;
uint8_t lastValidPacket[MAX_CODE_LENGTH] = {0};
uint8_t lastValidLength = 0;
String pendingName = "Unknown";
bool signalReceived = false;

volatile bool rollingShouldStop = false;

// === Konfigurácia CC1101 pre 433.92 MHz s variable packet length ===
const uint8_t cc1101_regs[] = {
  0x06, 0x2B, // IOCFG2
  0x07, 0x2E, // IOCFG1
  0x08, 0x06, // IOCFG0 - GDO0 pre interrupt
  0x09, 0x07, // FIFOTHR
  0x0A, 0x47, // SYNC1
  0x0B, 0x43, // SYNC0
  0x0C, 0x00, // PKTLEN - ignoruje sa v variable mode
  0x0D, 0x04, // PKTCTRL1 - enable CRC, address check
  0x0E, 0x05, // PKTCTRL0 - VARIABLE LENGTH MODE (KLÚČOVÉ!)
  0x0F, 0x00, // ADDR
  0x10, 0x00, // CHANNR
  0x11, 0x08, // FSCTRL1
  0x12, 0x21, // FSCTRL0
  0x13, 0x22, // FREQ2
  0x14, 0x65, // FREQ1
  0x15, 0x6A, // FREQ0 → 433.92 MHz
  0x16, 0x04, // MDMCFG4
  0x17, 0x83, // MDMCFG3
  0x18, 0x13, // MDMCFG2
  0x19, 0x22, // MDMCFG1
  0x1A, 0xF8, // MDMCFG0
  0x1B, 0x08, // DEVIATN
  0x1C, 0x07, // MCSM2
  0x1D, 0x00, // MCSM1
  0x1E, 0x18, // MCSM0
  0x1F, 0x0D, // FOCCFG
  0x20, 0x1C, // BSCFG
  0x21, 0x6C, // AGCCTRL2
  0x22, 0x43, // AGCCTRL1
  0x23, 0x00, // AGCCTRL0
  0x24, 0x87, // WOREVT1
  0x25, 0x6B, // WOREVT0
  0x26, 0x80, // WORCTRL
  0x27, 0xFB, // FREND1
  0x28, 0x10, // FREND0
  0x29, 0xE9, // FSCAL3
  0x2A, 0x2A, // FSCAL2
  0x2B, 0x00, // FSCAL1
  0x2C, 0x1F, // FSCAL0
  0x31, 0x00, // TEST2
  0x32, 0x20, // TEST1
  0x33, 0x0B, // TEST0
};

// === EEPROM operácie ===
void loadCodesFromEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  codeCount = 0;
  for (int i = 0; i < MAX_CODES; i++) {
    CodeItem item;
    EEPROM.get(i * CODE_ITEM_SIZE, item);
    if (item.length == 0 || item.length > MAX_CODE_LENGTH) break;
    bool validName = false;
    for (int j = 0; j < 32; j++) {
      if (item.name[j] == '\0') break;
      if (item.name[j] >= 32 && item.name[j] <= 126) {
        validName = true;
        break;
      }
    }
    if (!validName) strcpy(item.name, "Nezmenovaný");
    savedCodes[codeCount++] = item;
  }
  EEPROM.end();
}

void saveCodeToEEPROM(uint8_t* data, uint8_t length, const char* name) {
  if (codeCount >= MAX_CODES || length == 0 || length > MAX_CODE_LENGTH) return;
  CodeItem item;
  item.length = length;
  memcpy(item.data, data, length);
  strncpy(item.name, name, 31);
  item.name[31] = '\0';
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(codeCount * CODE_ITEM_SIZE, item);
  EEPROM.commit();
  EEPROM.end();
  savedCodes[codeCount++] = item;
}

int findCodeIndexByData(uint8_t* data, uint8_t length) {
  for (int i = 0; i < codeCount; i++) {
    if (savedCodes[i].length != length) continue;
    if (memcmp(savedCodes[i].data, data, length) == 0) return i;
  }
  return -1;
}

void deleteCodeFromEEPROM(int index) {
  if (index < 0 || index >= codeCount) return;
  EEPROM.begin(EEPROM_SIZE);
  for (int i = index; i < codeCount - 1; i++) {
    savedCodes[i] = savedCodes[i + 1];
  }
  codeCount--;
  for (int i = 0; i < MAX_CODES; i++) {
    if (i < codeCount) {
      EEPROM.put(i * CODE_ITEM_SIZE, savedCodes[i]);
    } else {
      CodeItem empty = {0, {0}, ""};
      EEPROM.put(i * CODE_ITEM_SIZE, empty);
    }
  }
  EEPROM.commit();
  EEPROM.end();
}

void clearAllCodesInEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  for (int i = 0; i < MAX_CODES; i++) {
    CodeItem empty = {0, {0}, ""};
    EEPROM.put(i * CODE_ITEM_SIZE, empty);
  }
  EEPROM.commit();
  EEPROM.end();
  codeCount = 0;
}

void printEEPROMStatus() {
  int used = codeCount;
  int total = MAX_CODES;
  float percent = (float)used / total * 100;
  Serial.println("\n--- EEPROM Stav ---");
  Serial.printf("Signály: %d / %d (%.1f %%)\n", used, total, percent);
  Serial.println("-------------------");
}

// === WebSocket Callback ===
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("WebSocket Client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
  } else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("WebSocket Client #%u disconnected\n", client->id());
  }
}

// === HTML stránka ===
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="sk">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0"/>
  <title>ESP32 RF Control</title>
  <style>
    body {
      font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
      background: linear-gradient(135deg, #6a11cb 0%, #2575fc 100%);
      color: #333;
      margin: 0;
      padding: 20px;
      min-height: 100vh;
      transition: filter 0.3s ease;
    }
    .container {
      max-width: 900px;
      margin: 0 auto;
      background: white;
      border-radius: 15px;
      box-shadow: 0 10px 30px rgba(0, 0, 0, 0.2);
      overflow: hidden;
    }
    header {
      background: #2c3e50;
      color: white;
      padding: 20px;
      text-align: center;
    }
    header h1 {
      margin: 0;
      font-size: 28px;
    }
    .content {
      padding: 20px;
    }
    .section {
      margin-bottom: 30px;
      padding: 20px;
      background: #f9f9f9;
      border-radius: 10px;
      border: 1px solid #e0e0e0;
    }
    .section h2 {
      margin-top: 0;
      color: #2c3e50;
      border-bottom: 2px solid #3498db;
      padding-bottom: 10px;
    }
    input[type="text"], input[type="number"] {
      width: 100%;
      padding: 12px;
      margin: 10px 0;
      border: 2px solid #ddd;
      border-radius: 8px;
      font-size: 16px;
      box-sizing: border-box;
    }
    .flex {
      display: flex;
      gap: 10px;
      align-items: center;
    }
    .flex input {
      flex: 1;
    }
    button {
      background: #3498db;
      color: white;
      border: none;
      padding: 12px 20px;
      margin: 5px;
      border-radius: 8px;
      cursor: pointer;
      font-size: 16px;
      transition: background 0.3s;
    }
    button:hover {
      background: #2980b9;
    }
    button.danger {
      background: #e74c3c;
    }
    button.danger:hover {
      background: #c0392b;
    }
    .placeholder {
      background: #fffde7;
      padding: 15px;
      border-radius: 8px;
      border: 1px solid #ffe082;
      font-size: 14px;
      line-height: 1.6;
      color: #5d4037;
    }
    /* Progress bar */
    .progress-container {
      width: 100%;
      background: #eee;
      border-radius: 10px;
      padding: 2px;
      margin: 10px 0;
    }
    .progress-bar {
      height: 20px;
      border-radius: 8px;
      background: linear-gradient(90deg, #4caf50, #8bc34a);
      text-align: center;
      color: white;
      font-size: 14px;
      line-height: 20px;
      transition: width 0.1s linear;
    }
    /* Progress bar pre prijímanie */
    .receive-progress {
      width: 100%;
      height: 10px;
      background: #ddd;
      border-radius: 5px;
      overflow: hidden;
      margin: 10px 0 5px;
    }
    .receive-fill {
      height: 100%;
      width: 0%;
      background: #2980b9;
      transition: width 0.1s linear;
    }
    /* Canvas spektrálna vizualizácia */
    .signal-animation {
      height: 100px;
      margin: 15px 0;
      background: #000;
      border-radius: 10px;
      overflow: hidden;
      position: relative;
      border: 1px solid #0f0;
      box-shadow: 0 0 10px rgba(0, 255, 0, 0.3);
    }
    #spectrumCanvas {
      width: 100%;
      height: 100%;
      display: block;
    }
    #spectrumCanvas::before {
      content: '';
      position: absolute;
      top: 0; left: 0; right: 0; bottom: 0;
      background: linear-gradient(transparent 50%, rgba(0, 255, 0, 0.1) 50%);
      background-size: 100% 4px;
      pointer-events: none;
      opacity: 0.3;
      z-index: 2;
    }
    /* Frekvenčná mierka */
    .frequency-scale {
      display: flex;
      justify-content: space-between;
      margin-top: 5px;
      font-family: monospace;
      font-size: 12px;
      color: #0f0;
      padding: 0 10px;
    }
    .codes-list {
      max-height: 500px;
      overflow-y: auto;
      border: 1px solid #ddd;
      border-radius: 8px;
      padding: 10px;
      background: #f8f9fa;
    }
    .code-item {
      padding: 12px;
      margin: 8px 0;
      background: white;
      border: 1px solid #ddd;
      border-radius: 8px;
      display: flex;
      justify-content: space-between;
      align-items: center;
      font-size: 14px;
      box-shadow: 0 1px 3px rgba(0,0,0,0.1);
    }
    .code-info {
      flex: 1;
    }
    .code-info strong {
      font-size: 16px;
      color: #2c3e50;
    }
    .code-info code {
      font-family: monospace;
      background: #f0f0f0;
      padding: 4px 8px;
      border-radius: 4px;
      color: #c0392b;
    }
    .code-actions {
      display: flex;
      gap: 5px;
    }
    .code-actions button {
      padding: 6px 10px;
      font-size: 14px;
    }
    .message {
      margin-top: 10px;
      padding: 10px;
      background: #d4edda;
      color: #155724;
      border: 1px solid #c3e6cb;
      border-radius: 5px;
      display: none;
    }
    /* Popup styles */
    .popup-overlay {
      position: fixed;
      top: 0;
      left: 0;
      width: 100%;
      height: 100%;
      background-color: rgba(0, 0, 0, 0.5);
      display: flex;
      align-items: center;
      justify-content: center;
      z-index: 1000;
      opacity: 0;
      visibility: hidden;
      transition: opacity 0.3s, visibility 0.3s;
    }
    .popup-overlay.active {
      opacity: 1;
      visibility: visible;
    }
    .popup-content {
      background: white;
      border-radius: 15px;
      padding: 25px;
      width: 90%;
      max-width: 400px;
      box-shadow: 0 10px 30px rgba(0, 0, 0, 0.3);
      transform: translateY(-50px);
      opacity: 0;
      transition: all 0.5s ease;
      position: relative;
    }
    .popup-overlay.active .popup-content {
      transform: translateY(0);
      opacity: 1;
    }
    /* Blur effect for main content when popup is active */
    body.popup-active .container {
      filter: blur(3px);
    }
    .popup-header {
      display: flex;
      justify-content: space-between;
      align-items: center;
      margin-bottom: 20px;
      padding-bottom: 10px;
      border-bottom: 2px solid #eee;
    }
    .popup-header h3 {
      margin: 0;
      color: #333;
      font-size: 20px;
    }
    .popup-close {
      background: none;
      border: none;
      font-size: 24px;
      color: #e74c3c;
      cursor: pointer;
      width: 30px;
      height: 30px;
      display: flex;
      align-items: center;
      justify-content: center;
      border-radius: 50%;
      transition: background-color 0.3s;
    }
    .popup-close:hover {
      background-color: #f8f9fa;
    }
    .popup-message {
      padding: 15px;
      border-radius: 8px;
      margin-bottom: 20px;
      text-align: center;
      font-weight: bold;
    }
    .popup-success {
      background-color: #d4edda;
      color: #155724;
      border: 1px solid #c3e6cb;
    }
    .popup-error {
      background-color: #f8d7da;
      color: #721c24;
      border: 1px solid #f5c6cb;
    }
    .popup-data {
      background-color: #f8f9fa;
      border-radius: 8px;
      padding: 15px;
      margin-bottom: 15px;
    }
    .popup-data-item {
      display: flex;
      justify-content: space-between;
      padding: 8px 0;
      border-bottom: 1px solid #eee;
    }
    .popup-data-item:last-child {
      border-bottom: none;
    }
    .popup-data-label {
      font-weight: bold;
      color: #555;
    }
    .popup-data-value {
      color: #333;
    }
    .popup-code {
      font-family: monospace;
      background-color: #e9ecef;
      padding: 3px 6px;
      border-radius: 4px;
      font-weight: normal;
    }
    .popup-timer {
      text-align: center;
      font-size: 12px;
      color: #6c757d;
      margin-top: 10px;
    }
  </style>
</head>
<body>
  <div class="container">
    <header>
      <h1>|RF Control Panel</h1>
    </header>
    <div class="content">
      <!-- Stav pamäte -->
      <div class="section">
        <h2>📊 Stav pamäte</h2>
        <div>Použité: <span id="usedSlots">0</span> / 20 kódov</div>
        <div class="progress-container">
          <div class="progress-bar" id="progressBar" style="width:0%">0%</div>
        </div>
      </div>
      <!-- Manuálny vstup -->
      <div class="section">
        <h2>🔧 Manuálny vstup signálu</h2>
        <input type="text" id="codeInput" placeholder="Zadaj hex kód (napr. AA BB CC DD)" />
        <button onclick="validateAndSaveManualCode()" class="bg-green-600 hover:bg-green-700" style="background: #27ae60;">Overiť a uložiť signál</button>
        <div class="placeholder">
          <strong>Podporované:</strong> Hexadecimálne hodnoty (0–9, A–F), medzery sú povolené.<br>
          <strong>Maximálna dĺžka:</strong> 64 bajtov (128 znakov).<br>
          <strong>Príklad:</strong> <code>AA BB CC DD EE FF</code> alebo <code>AABBCCDDEEFF</code>
        </div>
      </div>
      <!-- Prijímanie s menom -->
      <div class="section">
        <h2>📥 Prijímanie a ukladanie</h2>
        <div class="flex">
          <input type="text" id="nameInput" placeholder="Názov (napr. Garáž)" />
          <button onclick="receiveAndSave()" id="receiveBtn">Receive & Save</button>
        </div>
        <button onclick="clearAllCodes()" class="danger">Vymazať všetky signály</button>
        <!-- Progress bar pri prijímaní -->
        <div class="receive-progress">
          <div class="receive-fill" id="receiveFill"></div>
        </div>
        <!-- Názov vizualizácie -->
        <h3>|RF Spektrálna analýza</h3>
        <!-- Spektrálna vizualizácia -->
        <div class="signal-animation">
          <canvas id="spectrumCanvas"></canvas>
        </div>
        <!-- Frekvenčná mierka -->
        <div class="frequency-scale">
          <span>433.0</span>
          <span>433.4</span>
          <span>433.8</span>
          <span>434.2</span>
          <span>434.6</span>
        </div>
      </div>
      <!-- Odosielanie -->
      <div class="section">
        <h2>📤 Odoslanie signálu</h2>
        <button onclick="transmitCodeWithValidation()">Transmit</button>
        <button onclick="transmit3TimesWithValidation()">Transmit 3x (1s medzera)</button>
        <button onclick="startTransmitLoopWithValidation()">Transmit Loop (ON)</button>
        <button onclick="stopTransmitLoop()" class="danger">Stop Loop</button>
      </div>
      <!-- Rolling Codes -->
      <div class="section">
        <h2>🎲 Rolling Codes</h2>
        <div class="flex">
          <input type="text" id="rollFrom" placeholder="Od (hex)" value="AA BB CC" />
          <input type="text" id="rollTo" placeholder="Do (hex)" value="AA BB CD" />
        </div>
        <div class="flex" style="margin-top: 10px;">
          <input type="number" id="rollDelay" placeholder="Delay (ms)" value="500" min="50" max="2000" step="50" style="width: 120px;" />
          <button onclick="toggleRollingCodes()" id="rollBtn">▶️ Start RollingCodes</button>
        </div>
        <div class="placeholder" style="font-size: 12px; margin-top: 10px;">
          <strong>ℹ️ Pokyny:</strong>
          <ul style="margin: 5px 0; padding-left: 20px;">
            <li>Hex signály musia mať **rovnakú dĺžku**.</li>
            <li>Minimálna dĺžka: 1 bajt, maximálna: 64 bajty.</li>
            <li>Delay: min <code>50ms</code>, max <code>2000ms</code>.</li>
            <li>Po stlačení "Start" sa tlačidlo zmení na "Stop" (červené)</li>
          </ul>
        </div>
      </div>
      <!-- Live Log Odoslaných Signálov -->
      <div class="section">
        <h2>📡 Live Log Odoslaných Signálov</h2>
        <!-- Progress Bar -->
        <div style="display: flex; align-items: center; gap: 20px; margin: 15px 0;">
          <div style="position: relative; width: 120px; height: 120px;">
            <svg viewBox="0 0 100 100" style="transform: rotate(-90deg);">
              <circle cx="50" cy="50" r="45" stroke="#e0e0e0" stroke-width="8" fill="none"/>
              <circle id="progressCircle" cx="50" cy="50" r="45" stroke="#3498db" stroke-width="8" fill="none" stroke-dasharray="283" stroke-dashoffset="283"/>
            </svg>
            <div id="progressText" style="position: absolute; top: 50%; left: 50%; transform: translate(-50%, -50%); font-size: 24px; font-weight: bold; color: #333;">0%</div>
          </div>
          <div>
            <div><strong id="progressStatus">Čaká sa na spustenie...</strong></div>
            <div id="progressSubtext" style="font-size: 14px; color: #666;">Celkom: 0/0 signálov</div>
          </div>
        </div>
        <!-- Log Area -->
        <div id="sendLog" style="height: 200px; overflow-y: auto; border: 1px solid #ddd; border-radius: 8px; padding: 10px; background: #f8f9fa; font-family: monospace; font-size: 14px; margin: 10px 0;">
          <div style="color: #666;">Žiadne odoslané signály</div>
        </div>
        <button onclick="clearSendLog()" class="danger" style="width: 100%; padding: 10px; font-size: 16px; border-radius: 8px;">
          🧹 Clear Send codes
        </button>
      </div>
      <!-- Zoznam signálov -->
      <div class="section">
        <h2>💾 Uložené signály</h2>
        <div id="codesList" class="codes-list">
          Načítavam...
        </div>
      </div>
      <div id="message" class="message"></div>
    </div>
  </div>
  <!-- Popup Overlay -->
  <div id="popupOverlay" class="popup-overlay">
    <div class="popup-content">
      <div class="popup-header">
        <h3 id="popupTitle">RF Signál</h3>
        <button class="popup-close" onclick="closePopup()">×</button>
      </div>
      <div id="popupMessage" class="popup-message"></div>
      <div id="popupData" class="popup-data" style="display: none;">
        <div class="popup-data-item">
          <span class="popup-data-label">Hex:</span>
          <span class="popup-data-value"><code id="popupCode" class="popup-code"></code></span>
        </div>
        <div class="popup-data-item">
          <span class="popup-data-label">Dĺžka:</span>
          <span class="popup-data-value" id="popupLength"></span>
        </div>
        <div class="popup-data-item">
          <span class="popup-data-label">Názov:</span>
          <span class="popup-data-value" id="popupName"></span>
        </div>
        <div class="popup-data-item">
          <span class="popup-data-label">Frekvencia:</span>
          <span class="popup-data-value" id="popupFrequency">433.92 MHz</span>
        </div>
        <div class="popup-data-item" id="popupStatusRow">
          <span class="popup-data-label">Stav:</span>
          <span class="popup-data-value" id="popupStatus"></span>
        </div>
      </div>
      <div class="popup-timer">Okno sa automaticky zatvorí za <span id="popupTimer">5</span> sekúnd</div>
    </div>
  </div>
  <script>
  // === WebSocket Connection ===
  let ws = null;
  function initWebSocket() {
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const url = protocol + '//' + window.location.host + '/ws';
    ws = new WebSocket(url);
    ws.onopen = function(event) {
      console.log('WebSocket connected');
    };
    ws.onmessage = function(event) {
      try {
        const data = JSON.parse(event.data);
        console.log('WebSocket message received:', data);
        if (data.type === 'roll_start') {
          updateProgress(data.total, 0);
          document.getElementById('progressStatus').textContent = '🔄 Prebieha odosielanie...';
          document.getElementById('progressStatus').style.color = '#3498db';
        } else if (data.type === 'roll_progress') {
          updateProgress(data.total, data.current);
          if (data.hex !== undefined) {
            addToSendLog(data.hex, 'odoslaný');
          }
        } else if (data.type === 'roll_complete') {
          if (data.success) {
            showPopup('Rolling Codes', true, { 
              message: `✅ Dokončené! Odoslaných ${data.count} signálov za ${data.duration}s` 
            });
            updateProgress(data.count, data.count);
            document.getElementById('progressStatus').textContent = '✅ Dokončené!';
            document.getElementById('progressStatus').style.color = '#27ae60';
          } else {
            showPopup('Rolling Codes', false, { 
              message: `❌ Chyba: ${data.message}` 
            });
          }
          resetRollingButton();
        } else if (data.type === 'roll_stopped') {
          showPopup('Rolling Codes', true, { 
            message: `⏹️ Zastavené! Odoslaných ${data.count} signálov` 
          });
          updateProgress(data.count, data.count);
          document.getElementById('progressStatus').textContent = '⏹️ Zastavené používateľom';
          document.getElementById('progressStatus').style.color = '#e74c3c';
          resetRollingButton();
        }
      } catch (e) {
        console.error('Chyba pri spracovaní WebSocket správy:', e);
      }
    };
    ws.onclose = function(event) {
      console.log('WebSocket disconnected, attempting to reconnect...');
      setTimeout(initWebSocket, 3000);
    };
    ws.onerror = function(error) {
      console.error('WebSocket Error:', error);
    };
  }
  window.addEventListener('load', initWebSocket);

  let loopInterval = null;
  let canvas, ctx;
  let spectrumData = new Uint8Array(128);
  window.lastSignalReceived = false;
  let popupTimeout = null;
  let popupTimerInterval = null;
  let popupTimerValue = 5;
  let isCurrentlyReceiving = false;
  let pendingReceiveName = "";
  let previousCodeCount = 0;

  function showMessage(text, isError = false) {
    const msg = document.getElementById('message');
    msg.style.display = 'block';
    msg.textContent = text;
    msg.style.background = isError ? '#f8d7da' : '#d4edda';
    msg.style.color = isError ? '#721c24' : '#155724';
    setTimeout(() => msg.style.display = 'none', 3000);
  }

  function updateMemoryUsage(used) {
    const percent = Math.round((used / 20) * 100);
    document.getElementById('usedSlots').textContent = used;
    document.getElementById('progressBar').style.width = percent + '%';
    document.getElementById('progressBar').textContent = percent + '%';
  }

  function initSpectrum() {
    canvas = document.getElementById('spectrumCanvas');
    ctx = canvas.getContext('2d');
    const dpi = window.devicePixelRatio || 1;
    canvas.width = canvas.offsetWidth * dpi;
    canvas.height = canvas.offsetHeight * dpi;
    ctx.scale(dpi, dpi);
    simulateSpectrum();
  }

  function simulateSpectrum() {
    const barCount = 128;
    const data = new Array(barCount);
    for (let i = 0; i < barCount; i++) {
      data[i] = Math.random() * 20 + 5;
    }
    if (window.lastSignalReceived) {
      const pos = Math.floor(Math.random() * (barCount - 15));
      for (let i = 0; i < 15; i++) {
        data[pos + i] = 40 + Math.random() * 60;
      }
      window.lastSignalReceived = false;
    }
    spectrumData = new Uint8Array(data);
    drawSpectrum();
    setTimeout(simulateSpectrum, 100);
  }

  function drawSpectrum() {
    const width = canvas.offsetWidth;
    const height = canvas.offsetHeight;
    const barWidth = width / spectrumData.length;
    ctx.clearRect(0, 0, width, height);
    for (let i = 0; i < spectrumData.length; i++) {
      const v = spectrumData[i];
      const barHeight = (v / 100) * height;
      let r, g, b;
      if (v < 30) { r = 0; g = 200; b = 0; }
      else if (v < 60) { r = 255; g = 200; b = 0; }
      else { r = 255; g = 0; b = 0; }
      ctx.fillStyle = `rgb(${r}, ${g}, ${b})`;
      ctx.fillRect(i * barWidth, height - barHeight, barWidth - 1, barHeight);
    }
  }

  function showPopup(title, isSuccess, data = null) {
    const overlay = document.getElementById('popupOverlay');
    const messageDiv = document.getElementById('popupMessage');
    const dataDiv = document.getElementById('popupData');
    const titleDiv = document.getElementById('popupTitle');
    const statusRow = document.getElementById('popupStatusRow');
    const statusDiv = document.getElementById('popupStatus');
    titleDiv.textContent = title;
    if (isSuccess) {
      messageDiv.textContent = data?.message || 'Operácia úspešne dokončená!';
      messageDiv.className = 'popup-message popup-success';
    } else {
      messageDiv.textContent = data?.message || 'Chyba: Operácia zlyhala!';
      messageDiv.className = 'popup-message popup-error';
    }
    if (data && data.hex !== undefined) {
      dataDiv.style.display = 'block';
      document.getElementById('popupCode').textContent = data.hex;
      document.getElementById('popupLength').textContent = data.length + " bajtov";
      document.getElementById('popupName').textContent = data.name || 'Nezmenovaný';
      document.getElementById('popupFrequency').textContent = data.frequency || '433.92 MHz';
      if (data.status) {
        statusRow.style.display = 'flex';
        statusDiv.textContent = data.status;
      } else {
        statusRow.style.display = 'none';
      }
    } else {
      dataDiv.style.display = 'none';
      statusRow.style.display = 'none';
    }
    document.querySelector('.container').style.filter = 'blur(3px)';
    overlay.classList.add('active');
    popupTimerValue = 5;
    document.getElementById('popupTimer').textContent = popupTimerValue;
    if (popupTimerInterval) clearInterval(popupTimerInterval);
    popupTimerInterval = setInterval(() => {
      popupTimerValue--;
      document.getElementById('popupTimer').textContent = popupTimerValue;
      if (popupTimerValue <= 0) closePopup();
    }, 1000);
    if (popupTimeout) clearTimeout(popupTimeout);
    popupTimeout = setTimeout(closePopup, 5000);
  }

  function closePopup() {
    const overlay = document.getElementById('popupOverlay');
    overlay.classList.remove('active');
    document.querySelector('.container').style.filter = 'none';
    if (popupTimeout) clearTimeout(popupTimeout);
    if (popupTimerInterval) clearInterval(popupTimerInterval);
  }

  function validateHex(hexStr) {
    hexStr = hexStr.trim().toUpperCase();
    if (!hexStr) return { isValid: false, message: 'Hex nesmie byť prázdny!' };
    let clean = hexStr.replace(/\s+/g, '');
    if (clean.length === 0) return { isValid: false, message: 'Hex nesmie byť prázdny!' };
    if (clean.length % 2 !== 0) return { isValid: false, message: 'Počet znakov musí byť párny!' };
    if (clean.length > 128) return { isValid: false, message: 'Maximálna dĺžka je 64 bajtov (128 znakov)!' };
    if (!/^[0-9A-F]+$/.test(clean)) return { isValid: false, message: 'Povolené znaky: 0–9, A–F!' };
    return { isValid: true, hex: clean, length: clean.length / 2, message: 'Platný hex signál!' };
  }

  function validateAndSaveManualCode() {
    const codeInput = document.getElementById('codeInput');
    const hexStr = codeInput.value;
    const validationResult = validateHex(hexStr);
    if (!validationResult.isValid) {
      showPopup('Chyba validácie signálu', false, { message: validationResult.message });
      return;
    }
    const hex = validationResult.hex;
    const name = "Saved: " + hex.slice(0, 10) + "...";
    const signalData = {
      hex: hex,
      length: validationResult.length,
      name: name,
      frequency: '433.92 MHz',
      message: 'Signál bol úspešne overený a uložený!',
      status: 'Uložené do pamäte'
    };
    showPopup('Manuálny signál', true, signalData);
    fetch('/saveManual', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: 'hex=' + encodeURIComponent(hex) + '&name=' + encodeURIComponent(name)
    })
    .then(response => response.text())
    .then(data => {
      console.log('Signál uložený:', data);
      showMessage('Signál uložený (' + validationResult.length + ' bajtov)');
      updateCodesList();
    })
    .catch(error => {
      console.error('Chyba pri ukladaní:', error);
      showMessage('Chyba pri ukladaní signálu!', true);
    });
  }

  function transmitCodeWithValidation() {
    const codeInput = document.getElementById('codeInput');
    const hexStr = codeInput.value;
    const validationResult = validateHex(hexStr);
    if (!validationResult.isValid) {
      showPopup('Chyba pri odosielaní', false, { message: validationResult.message });
      return;
    }
    const hex = validationResult.hex;
    const transmitData = {
      hex: hex,
      length: validationResult.length,
      name: 'Odoslaný signál',
      frequency: '433.92 MHz',
      message: 'Signál bol úspešne odoslaný!',
      status: 'Odoslané cez CC1101'
    };
    showPopup('RF Odosielanie', true, transmitData);
    fetch('/transmit', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: 'hex=' + encodeURIComponent(hex)
    })
    .then(response => response.text())
    .then(data => {
      console.log('Signál odoslaný:', data);
      showMessage('Signál odoslaný (' + validationResult.length + ' bajtov)');
    })
    .catch(error => {
      console.error('Chyba pri odosielaní:', error);
      showMessage('Chyba pri odosielaní signálu!', true);
    });
  }

  function transmit3TimesWithValidation() {
    const codeInput = document.getElementById('codeInput');
    const hexStr = codeInput.value;
    const validationResult = validateHex(hexStr);
    if (!validationResult.isValid) {
      showPopup('Chyba pri odosielaní', false, { message: validationResult.message });
      return;
    }
    const hex = validationResult.hex;
    const transmitData = {
      hex: hex,
      length: validationResult.length,
      name: 'Odoslaný signál (3x)',
      frequency: '433.92 MHz',
      message: 'Signál bol úspešne odoslaný 3 krát!',
      status: 'Odoslané 3x cez CC1101'
    };
    showPopup('RF Odosielanie', true, transmitData);
    fetch('/transmit3', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: 'hex=' + encodeURIComponent(hex)
    })
    .then(response => response.text())
    .then(data => {
      console.log('Signál odoslaný 3x:', data);
      showMessage('Signál odoslaný 3x (' + validationResult.length + ' bajtov)');
    })
    .catch(error => {
      console.error('Chyba pri odosielaní:', error);
      showMessage('Chyba pri odosielaní signálu!', true);
    });
  }

  function startTransmitLoopWithValidation() {
    const codeInput = document.getElementById('codeInput');
    const hexStr = codeInput.value;
    const validationResult = validateHex(hexStr);
    if (!validationResult.isValid) {
      showPopup('Chyba pri odosielaní', false, { message: validationResult.message });
      return;
    }
    const hex = validationResult.hex;
    const transmitData = {
      hex: hex,
      length: validationResult.length,
      name: 'Loop odosielania',
      frequency: '433.92 MHz',
      message: 'Loop odosielania bol spustený!',
      status: 'Opakované odosielanie'
    };
    showPopup('RF Odosielanie', true, transmitData);
    fetch('/transmit', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: 'hex=' + encodeURIComponent(hex)
    })
    .then(response => response.text())
    .then(data => {
      console.log('Loop odosielania spustený:', data);
      showMessage('Loop odosielania spustený pre signál ' + hex + '!');
      if (loopInterval) clearInterval(loopInterval);
      loopInterval = setInterval(() => {
        fetch('/transmit', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: 'hex=' + encodeURIComponent(hex)
        })
        .catch(error => {
          console.error('Chyba pri odosielaní v loope:', error);
        });
      }, 1000);
    })
    .catch(error => {
      console.error('Chyba pri spustení loopu:', error);
      showMessage('Chyba pri spustení loopu!', true);
    });
  }

  function stopTransmitLoop() {
    if (loopInterval) clearInterval(loopInterval);
    loopInterval = null;
    showMessage('Loop zastavený');
    const stopData = {
      message: 'Loop odosielania bol zastavený!',
      status: 'Odosielanie zastavené'
    };
    showPopup('RF Odosielanie', true, stopData);
  }

  function receiveAndSave() {
    const name = document.getElementById('nameInput').value.trim() || 'Nezmenovaný';
    const btn = document.getElementById('receiveBtn');
    const fill = document.getElementById('receiveFill');
    isCurrentlyReceiving = true;
    pendingReceiveName = name;
    btn.disabled = true;
    btn.textContent = 'Prijímanie...';
    fill.style.width = '0%';
    let elapsed = 0;
    const startTime = Date.now();
    const interval = setInterval(() => {
      elapsed += 100;
      const percent = Math.round((elapsed / 3000) * 100);
      fill.style.width = percent + '%';
      if (elapsed >= 3000) {
        clearInterval(interval);
        if (isCurrentlyReceiving) {
          isCurrentlyReceiving = false;
          btn.disabled = false;
          btn.textContent = 'Receive & Save';
          checkForNewCodeAfterReceive();
        }
      }
    }, 100);
    fetch('/receive', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: 'name=' + encodeURIComponent(name)
    })
    .then(() => {
      showMessage('Hľadám signál...');
    })
    .catch(() => {
      showMessage('Chyba', true);
      isCurrentlyReceiving = false;
      btn.disabled = false;
      btn.textContent = 'Receive & Save';
      showPopup('RF Prijímanie', false, { message: 'Chyba pri prijímaní signálu!' });
    });
  }

  function checkForNewCodeAfterReceive() {
    fetch('/list')
      .then(res => res.json())
      .then(codes => {
        const currentCodeCount = codes.length;
        if (currentCodeCount > previousCodeCount) {
          previousCodeCount = currentCodeCount;
          updateCodesList();
          const lastCode = codes[codes.length - 1];
          const signalData = {
            hex: lastCode.hex,
            length: lastCode.length,
            name: lastCode.name,
            message: 'Signál úspešne zachytený a uložený!'
          };
          showPopup('RF Prijímanie', true, signalData);
        } else {
          showPopup('RF Prijímanie', false, { 
            message: 'Signál nebol zachytený. Skúste to znova.' 
          });
        }
      })
      .catch(err => {
        console.error('Chyba pri kontrole nového signálu:', err);
        showPopup('RF Prijímanie', false, { 
          message: 'Signál nebol zachytený. Skúste to znova.' 
        });
      });
  }

  function toggleRollingCodes() {
    const btn = document.getElementById('rollBtn');
    if (isRolling) {
      fetch('/rollStop', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: ''
      })
      .then(() => {
        showMessage('Zastavujem Rolling Codes...');
      })
      .catch(err => {
        console.error('Chyba pri zastavovaní:', err);
        resetRollingButton();
      });
    } else {
      const fromHex = document.getElementById('rollFrom').value.trim();
      const toHex = document.getElementById('rollTo').value.trim();
      const delayMs = parseInt(document.getElementById('rollDelay').value);
      if (!fromHex || !toHex || isNaN(delayMs)) {
        showPopup('Chyba', false, { message: 'Všetky polia musia byť vyplnené!' });
        return;
      }
      const fromValid = validateHex(fromHex);
      const toValid = validateHex(toHex);
      if (!fromValid.isValid || !toValid.isValid) {
        showPopup('Chyba', false, { message: 'Neplatný hex kód!' });
        return;
      }
      if (fromValid.length !== toValid.length) {
        showPopup('Chyba', false, { message: 'Dĺžky signálov sa nelíšia!' });
        return;
      }
      if (delayMs < 50 || delayMs > 2000) {
        showPopup('Chyba', false, { message: 'Delay musí byť medzi 50ms a 2000ms!' });
        return;
      }
      clearSendLog();
      isRolling = true;
      btn.textContent = '⏹️ STOP';
      btn.style.background = '#e74c3c';
      btn.style.color = 'white';
      showPopup('Rolling Codes', true, { 
        message: `Spúšťam od ${fromHex} po ${toHex} s oneskorením ${delayMs}ms` 
      });
      fetch('/roll', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: `from_hex=${encodeURIComponent(fromHex)}&to_hex=${encodeURIComponent(toHex)}&delay=${delayMs}`
      })
      .catch(err => {
        console.error('Chyba pri spúšťaní Rolling Codes:', err);
        showPopup('Rolling Codes', false, { message: 'Chyba: ' + err.message });
        resetRollingButton();
      });
    }
  }

  function resetRollingButton() {
    const btn = document.getElementById('rollBtn');
    isRolling = false;
    btn.textContent = '▶️ Start RollingCodes';
    btn.style.background = '#3498db';
    btn.style.color = 'white';
  }

  let sendLogEntries = [];
  let totalCodesToProcess = 0;
  let processedCodesCount = 0;
  function updateProgress(total, current) {
    totalCodesToProcess = total;
    processedCodesCount = current;
    const percentage = total > 0 ? Math.round((current / total) * 100) : 0;
    const circle = document.getElementById('progressCircle');
    const text = document.getElementById('progressText');
    const status = document.getElementById('progressStatus');
    const subtext = document.getElementById('progressSubtext');
    if (circle && text) {
      const circumference = 2 * Math.PI * 45;
      const offset = circumference - (percentage / 100) * circumference;
      circle.style.strokeDasharray = circumference;
      circle.style.strokeDashoffset = offset;
      text.textContent = percentage + '%';
    }
    if (status) {
      if (total === 0) {
        status.textContent = 'Čaká sa na spustenie...';
      } else if (current >= total) {
        status.textContent = '✅ Dokončené!';
        status.style.color = '#27ae60';
      } else {
        status.textContent = '🔄 Prebieha odosielanie...';
        status.style.color = '#3498db';
      }
    }
    if (subtext) {
      subtext.textContent = `Celkom: ${current}/${total} signálov`;
    }
  }

  function addToSendLog(hex, status = 'odoslaný') {
    const logDiv = document.getElementById('sendLog');
    if (!logDiv) return;
    const timestamp = new Date().toLocaleTimeString();
    const entry = {
      hex: hex,
      timestamp: timestamp,
      status: status
    };
    sendLogEntries.push(entry);
    const div = document.createElement('div');
    div.style.padding = '5px 0';
    div.style.borderBottom = '1px solid #eee';
    div.style.color = status === 'chyba' ? '#e74c3c' : '#2c3e50';
    div.innerHTML = `[${timestamp}] <code style="background: #e9ecef; padding: 2px 4px; border-radius: 3px;">${hex}</code> - ${status === 'chyba' ? '❌ Chyba' : '✅ Úspešne'}`;
    logDiv.appendChild(div);
    logDiv.scrollTop = logDiv.scrollHeight;
  }

  function clearSendLog() {
    const logDiv = document.getElementById('sendLog');
    if (logDiv) {
      logDiv.innerHTML = '<div style="color: #666;">Žiadne odoslané signály</div>';
    }
    sendLogEntries = [];
    updateProgress(0, 0);
    showMessage('Log odoslaných signálov bol vymazaný');
  }

  function initializeSendLog() {
    updateProgress(0, 0);
  }

  function updateCodesList() {
    const list = document.getElementById('codesList');
    list.innerHTML = '<p>Načítavam...</p>';
    fetch('/list')
      .then(res => {
        if (!res.ok) throw new Error('HTTP ' + res.status);
        return res.json();
      })
      .then(codes => {
        list.innerHTML = '';
        updateMemoryUsage(codes.length);
        previousCodeCount = codes.length;
        if (codes.length === 0) {
          list.innerHTML = '<p>Žiadne uložené signály.</p>';
          return;
        }
        codes.forEach(item => {
          const div = document.createElement('div');
          div.className = 'code-item';
          div.dataset.hex = item.hex;
          div.innerHTML = `
            <div class="code-info">
              <div><strong id="name-${item.hex}">${item.name}</strong></div>
              <div style="font-family:monospace;color:#c0392b">Hex: ${item.hex}</div>
              <div style="font-size:12px">Dĺžka: ${item.length} bajtov</div>
              <input type="text" id="edit-${item.hex}" value="${item.name}" 
                     style="display:none;margin-top:4px;padding:5px;width:100%" />
            </div>
            <div class="code-actions">
              <button onclick="useCode('${item.hex}')" title="Použiť">📋</button>
              <button onclick="sendStored('${item.hex}')" title="Odoslať">📤</button>
              <button onclick="startEdit('${item.hex}')" title="Upraviť">✎</button>
              <button onclick="saveEdit('${item.hex}')" style="display:none" title="Uložiť">✔️</button>
              <button onclick="deleteCode('${item.hex}')" class="danger" title="Vymazať">🗑️</button>
            </div>
          `;
          list.appendChild(div);
        });
      })
      .catch(err => {
        list.innerHTML = '<p style="color:red">Chyba: ' + err.message + '</p>';
        console.error('Fetch error:', err);
      });
  }

  function startEdit(hex) {
    document.getElementById(`name-${hex}`).style.display = 'none';
    const input = document.getElementById(`edit-${hex}`);
    input.style.display = 'inline-block';
    input.focus();
    document.querySelector(`[onclick="saveEdit('${hex}')"]`).style.display = 'inline-block';
    document.querySelector(`[onclick="startEdit('${hex}')"]`).style.display = 'none';
  }

  function saveEdit(hex) {
    const newName = document.getElementById(`edit-${hex}`).value.trim() || 'Nezmenovaný';
    fetch('/updateName', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: 'hex=' + encodeURIComponent(hex) + '&name=' + encodeURIComponent(newName)
    })
    .then(() => {
      showMessage('Meno zmenené: ' + newName);
      updateCodesList();
    })
    .catch(err => {
      showMessage('Chyba: ' + err.message, true);
    });
  }

  function useCode(hex) {
    document.getElementById('codeInput').value = hex;
    showMessage(`Signál ${hex} použitý`);
  }

  function sendStored(hex) {
    useCode(hex);
    transmitCodeWithValidation();
  }

  function deleteCode(hex) {
    if (confirm('Vymazať tento signál?')) {
      fetch('/delete?hex=' + encodeURIComponent(hex), { method: 'GET' })
        .then(() => {
          showMessage('Signál vymazaný');
          updateCodesList();
          showPopup('Správa systému', true, { 
            message: 'Signál bol úspešne vymazaný!', 
            status: 'Vymazané z pamäte' 
          });
        });
    }
  }

  function clearAllCodes() {
    if (confirm('Vymazať všetky signály?')) {
      fetch('/clear', { method: 'GET' })
        .then(() => {
          showMessage('Všetko vymazané');
          updateCodesList();
          showPopup('Správa systému', true, { 
            message: 'Všetky signály boli úspešne vymazané!', 
            status: 'Pamäť vyčistená' 
          });
        });
    }
  }

  initSpectrum();
  updateCodesList();
  initializeSendLog();

  document.getElementById('popupOverlay').addEventListener('click', function(e) {
    if (e.target === this) {
      closePopup();
    }
  });
  </script>
</body>
</html>
)rawliteral";

// === EEPROM operácie ===
void loadCodesFromEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  codeCount = 0;
  for (int i = 0; i < MAX_CODES; i++) {
    CodeItem item;
    EEPROM.get(i * CODE_ITEM_SIZE, item);
    if (item.length == 0 || item.length > MAX_CODE_LENGTH) break;
    bool validName = false;
    for (int j = 0; j < 32; j++) {
      if (item.name[j] == '\0') break;
      if (item.name[j] >= 32 && item.name[j] <= 126) validName = true;
    }
    if (!validName) strcpy(item.name, "Nezmenovaný");
    savedCodes[codeCount++] = item;
  }
  EEPROM.end();
}

int findCodeIndexByData(uint8_t* data, uint8_t length) {
  for (int i = 0; i < codeCount; i++) {
    if (savedCodes[i].length != length) continue;
    if (memcmp(savedCodes[i].data, data, length) == 0) return i;
  }
  return -1;
}

void saveCodeToEEPROM(uint8_t* data, uint8_t length, const char* name) {
  if (codeCount >= MAX_CODES || length == 0 || length > MAX_CODE_LENGTH) return;
  CodeItem item;
  item.length = length;
  memcpy(item.data, data, length);
  strncpy(item.name, name, 31);
  item.name[31] = '\0';
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(codeCount * CODE_ITEM_SIZE, item);
  EEPROM.commit();
  EEPROM.end();
  savedCodes[codeCount++] = item;
}

void deleteCodeFromEEPROM(int index) {
  if (index < 0 || index >= codeCount) return;
  EEPROM.begin(EEPROM_SIZE);
  for (int i = index; i < codeCount - 1; i++) {
    savedCodes[i] = savedCodes[i + 1];
  }
  codeCount--;
  for (int i = 0; i < MAX_CODES; i++) {
    if (i < codeCount) {
      EEPROM.put(i * CODE_ITEM_SIZE, savedCodes[i]);
    } else {
      CodeItem empty = {0, {0}, ""};
      EEPROM.put(i * CODE_ITEM_SIZE, empty);
    }
  }
  EEPROM.commit();
  EEPROM.end();
}

void clearAllCodesInEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  for (int i = 0; i < MAX_CODES; i++) {
    CodeItem empty = {0, {0}, ""};
    EEPROM.put(i * CODE_ITEM_SIZE, empty);
  }
  EEPROM.commit();
  EEPROM.end();
  codeCount = 0;
}

void printEEPROMStatus() {
  int used = codeCount;
  int total = MAX_CODES;
  float percent = (float)used / total * 100;
  Serial.println("\n--- EEPROM Stav ---");
  Serial.printf("Signály: %d / %d (%.1f %%)\n", used, total, percent);
  Serial.println("-------------------");
}

// === WebSocket Callback ===
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("WebSocket Client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
  } else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("WebSocket Client #%u disconnected\n", client->id());
  }
}

// === Setup ===
void setup() {
  Serial.begin(115200);
  delay(1000);

  // WiFi
  WiFi.softAP(ssid, password);
  Serial.println("");
  Serial.print("WiFi AP: ");
  Serial.println(ssid);
  Serial.print("IP: ");
  Serial.println(WiFi.softAPIP());

  // CC1101 inicializácia
  Serial.println("Inicializujem CC1101...");
  cc1101.init();
  cc1101.setRegisters(cc1101_regs, sizeof(cc1101_regs));
  cc1101.setPacketLengthMode(SmartRC_CC1101::VARIABLE_LENGTH);
  cc1101.setSyncWord(0x43, 0x47);
  cc1101.setModulation(SmartRC_CC1101::MODULATION_GFSK);
  cc1101.setBitRate(2400);
  cc1101.setFrequencyDeviation(2.4);
  cc1101.setChannel(0);
  cc1101.setPA(0);
  cc1101.setPowerUp();
  pinMode(CC1101_GDO0_PIN, INPUT_PULLUP);

  Serial.println("CC1101: Nastavené na 433.92 MHz, GFSK, Variable Length Mode (max 64B)");
  Serial.println("Prijímanie: GDO0 na GPIO21");

  // EEPROM
  loadCodesFromEEPROM();
  Serial.printf("sizeof(CodeItem) = %d\n", sizeof(CodeItem));
  Serial.printf("Debug: codeCount = %d\n", codeCount);
  for (int i = 0; i < codeCount; i++) {
    Serial.printf("Signál[%d]: %d bajtov, Meno: %s\n", i, savedCodes[i].length, savedCodes[i].name);
    Serial.print("Hex: ");
    for (int j = 0; j < savedCodes[i].length; j++) {
      Serial.printf("%02X ", savedCodes[i].data[j]);
    }
    Serial.println();
  }
  printEEPROMStatus();

  // Web server
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/html", index_html);
  });

  server.on("/list", HTTP_GET, [](AsyncWebServerRequest *request){
    String json = "[";
    for (int i = 0; i < codeCount; i++) {
      CodeItem& item = savedCodes[i];
      String hexStr = "";
      for (int j = 0; j < item.length; j++) {
        hexStr += String(item.data[j], HEX);
        if (j < item.length - 1) hexStr += " ";
      }
      String cleanName = "";
      for (int j = 0; j < 32; j++) {
        char c = item.name[j];
        if (c == '\0') break;
        if (c == '"') cleanName += "\\\"";
        else if (c == '\\') cleanName += "\\\\";
        else if (c == '\n') cleanName += "\\n";
        else if (c == '\r') cleanName += "\\r";
        else cleanName += c;
      }
      if (cleanName.length() == 0) cleanName = "Nezmenovaný";
      json += "{\"name\":\"" + cleanName + "\",\"hex\":\"" + hexStr + "\",\"length\":" + String(item.length) + "}";
      if (i < codeCount - 1) json += ",";
    }
    json += "]";
    AsyncWebServerResponse *response = request->beginResponse(200, "application/json", json);
    response->addHeader("Access-Control-Allow-Origin", "*");
    request->send(response);
  });

  server.on("/lastSignal", HTTP_GET, [] (AsyncWebServerRequest *request){
    String json = "{\"received\":" + String(signalReceived ? "true" : "false") + "}";
    signalReceived = false;
    request->send(200, "application/json", json);
  });

  server.on("/receive", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("name", true)) {
      pendingName = request->getParam("name", true)->value();
    } else {
      pendingName = "Nezmenovaný";
    }
    isReceiving = true;
    lastValidLength = 0;
    receiveStartTime = millis();
    request->send(200, "text/plain", "Prijímanie (3s)...");
  });

  server.on("/roll", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!request->hasParam("from_hex", true) || 
        !request->hasParam("to_hex", true) || 
        !request->hasParam("delay", true)) {
      request->send(400, "application/json", "{\"success\":false,\"message\":\"Chýbajúce parametre\"}");
      return;
    }
    String fromHex = request->getParam("from_hex", true)->value();
    String toHex = request->getParam("to_hex", true)->value();
    int delayMs = request->getParam("delay", true)->value().toInt();
    fromHex.replace(" ", "");
    toHex.replace(" ", "");

    if (fromHex.length() % 2 != 0 || toHex.length() % 2 != 0) {
      request->send(400, "application/json", "{\"success\":false,\"message\":\"Hex musí mať párny počet znakov\"}");
      return;
    }

    int fromLen = fromHex.length() / 2;
    int toLen = toHex.length() / 2;
    if (fromLen != toLen || fromLen == 0 || fromLen > MAX_CODE_LENGTH) {
      request->send(400, "application/json", "{\"success\":false,\"message\":\"Dĺžky sa nelíšia alebo sú neplatné\"}");
      return;
    }
    if (delayMs < 50 || delayMs > 2000) {
      request->send(400, "application/json", "{\"success\":false,\"message\":\"Delay 50–2000ms\"}");
      return;
    }

    uint8_t fromData[MAX_CODE_LENGTH], toData[MAX_CODE_LENGTH];
    for (int i = 0; i < fromLen; i++) {
      String byteStr = fromHex.substring(i*2, i*2+2);
      fromData[i] = (uint8_t)strtol(byteStr.c_str(), NULL, 16);
      byteStr = toHex.substring(i*2, i*2+2);
      toData[i] = (uint8_t)strtol(byteStr.c_str(), NULL, 16);
    }

    rollingShouldStop = false;
    struct RollParams {
      uint8_t fromData[MAX_CODE_LENGTH];
      uint8_t toData[MAX_CODE_LENGTH];
      int len;
      int delayMs;
    };
    RollParams *params = new RollParams;
    memcpy(params->fromData, fromData, fromLen);
    memcpy(params->toData, toData, toLen);
    params->len = fromLen;
    params->delayMs = delayMs;

    xTaskCreatePinnedToCore(
      [](void *param) {
        RollParams *p = (RollParams *)param;
        disableCore0WDT();
        disableCore1WDT();
        unsigned long startTime = millis();
        int count = 0;
        int totalCount = 2;
        String startMsg = "{\"type\":\"roll_start\",\"total\":" + String(totalCount) + "}";
        ws.textAll(startMsg);

        for (int i = 0; i < 2; i++) {
          if (rollingShouldStop) {
            String stopMsg = "{\"type\":\"roll_stopped\",\"count\":" + String(count) + "}";
            ws.textAll(stopMsg);
            break;
          }
          uint8_t* txData = (i == 0) ? p->fromData : p->toData;
          String hexStr = "";
          for (int j = 0; j < p->len; j++) {
            hexStr += String(txData[j], HEX) + " ";
          }
          hexStr.trim();
          cc1101.transmit(txData, p->len);
          count++;
          String progressMsg = "{\"type\":\"roll_progress\",\"current\":" + String(count) + ",\"total\":" + String(totalCount) + ",\"hex\":\"" + hexStr + "\"}";
          ws.textAll(progressMsg);
          yield();
          if (p->delayMs > 0 && i < 1) {
            unsigned long startDelay = millis();
            while (millis() - startDelay < p->delayMs) {
              yield();
              delay(1);
              if (rollingShouldStop) break;
            }
            if (rollingShouldStop) {
              String stopMsg = "{\"type\":\"roll_stopped\",\"count\":" + String(count) + "}";
              ws.textAll(stopMsg);
              break;
            }
          }
        }

        enableCore0WDT();
        enableCore1WDT();
        if (!rollingShouldStop) {
          unsigned long duration = millis() - startTime;
          float seconds = duration / 1000.0;
          String resultMsg = "{\"type\":\"roll_complete\",\"success\":true,\"count\":" + String(count) + ",\"duration\":" + String(seconds, 2) + "}";
          ws.textAll(resultMsg);
        }
        vTaskDelete(NULL);
      },
      "RollingTask",
      8192,
      params,
      1,
      NULL,
      1
    );

    request->send(202, "application/json", "{\"success\":true,\"message\":\"Rolling Codes spustené\"}");
  });

  server.on("/rollStop", HTTP_POST, [](AsyncWebServerRequest *request){
    rollingShouldStop = true;
    request->send(200, "application/json", "{\"success\":true,\"message\":\"Zastavujem Rolling Codes...\"}");
  });

  server.on("/transmit", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("hex", true)) {
      String hexStr = request->getParam("hex", true)->value();
      hexStr.replace(" ", "");
      if (hexStr.length() % 2 != 0 || hexStr.length() == 0) {
        request->send(400, "text/plain", "Neplatný hex kód!");
        return;
      }
      int len = hexStr.length() / 2;
      if (len == 0 || len > MAX_CODE_LENGTH) {
        request->send(400, "text/plain", "Dĺžka musí byť 1–64 bajtov!");
        return;
      }
      uint8_t txData[MAX_CODE_LENGTH];
      for (int i = 0; i < len; i++) {
        String byteStr = hexStr.substring(i*2, i*2+2);
        txData[i] = (uint8_t)strtol(byteStr.c_str(), NULL, 16);
      }
      cc1101.transmit(txData, len);
      request->send(200, "text/plain", "Odoslané");
      Serial.print("📤 Odoslaný signál: ");
      for (int i = 0; i < len; i++) Serial.printf("%02X ", txData[i]);
      Serial.println();
    } else {
      request->send(400, "text/plain", "Chýba parameter 'hex'");
    }
  });

  server.on("/transmit3", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("hex", true)) {
      String hexStr = request->getParam("hex", true)->value();
      hexStr.replace(" ", "");
      if (hexStr.length() % 2 != 0 || hexStr.length() == 0) {
        request->send(400, "text/plain", "Neplatný hex kód!");
        return;
      }
      int len = hexStr.length() / 2;
      if (len == 0 || len > MAX_CODE_LENGTH) {
        request->send(400, "text/plain", "Dĺžka musí byť 1–64 bajtov!");
        return;
      }
      uint8_t txData[MAX_CODE_LENGTH];
      for (int i = 0; i < len; i++) {
        String byteStr = hexStr.substring(i*2, i*2+2);
        txData[i] = (uint8_t)strtol(byteStr.c_str(), NULL, 16);
      }
      for (int i = 0; i < 3; i++) {
        cc1101.transmit(txData, len);
        delay(1000);
      }
      request->send(200, "text/plain", "Odoslané 3x");
    } else {
      request->send(400, "text/plain", "Chýba parameter 'hex'");
    }
  });

  server.on("/saveManual", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("hex", true) && request->hasParam("name", true)) {
      String hexStr = request->getParam("hex", true)->value();
      String name = request->getParam("name", true)->value();
      hexStr.replace(" ", "");
      if (hexStr.length() % 2 != 0 || hexStr.length() == 0) {
        request->send(400, "text/plain", "Neplatný hex kód!");
        return;
      }
      int len = hexStr.length() / 2;
      if (len == 0 || len > MAX_CODE_LENGTH) {
        request->send(400, "text/plain", "Dĺžka musí byť 1–64 bajtov!");
        return;
      }
      uint8_t data[MAX_CODE_LENGTH];
      for (int i = 0; i < len; i++) {
        String byteStr = hexStr.substring(i*2, i*2+2);
        data[i] = (uint8_t)strtol(byteStr.c_str(), NULL, 16);
      }
      saveCodeToEEPROM(data, len, name.c_str());
      request->send(200, "text/plain", "OK");
      Serial.printf("💾 Uložený signál: %d bajtov, meno: %s\n", len, name.c_str());
    } else {
      request->send(400, "text/plain", "Chýbajúce parametre");
    }
  });

  server.on("/updateName", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("hex", true) && request->hasParam("name", true)) {
      String hexStr = request->getParam("hex", true)->value();
      String name = request->getParam("name", true)->value();
      hexStr.replace(" ", "");
      if (hexStr.length() % 2 != 0 || hexStr.length() == 0) {
        request->send(400, "text/plain", "Neplatný hex kód!");
        return;
      }
      int len = hexStr.length() / 2;
      uint8_t data[MAX_CODE_LENGTH];
      for (int i = 0; i < len; i++) {
        String byteStr = hexStr.substring(i*2, i*2+2);
        data[i] = (uint8_t)strtol(byteStr.c_str(), NULL, 16);
      }
      int index = findCodeIndexByData(data, len);
      if (index == -1) {
        request->send(404, "text/plain", "Signál nenájdený");
        return;
      }
      strncpy(savedCodes[index].name, name.c_str(), 31);
      savedCodes[index].name[31] = '\0';
      EEPROM.begin(EEPROM_SIZE);
      EEPROM.put(index * CODE_ITEM_SIZE, savedCodes[index]);
      EEPROM.commit();
      EEPROM.end();
      request->send(200, "text/plain", "OK");
    } else {
      request->send(400, "text/plain", "Chýbajúce parametre");
    }
  });

  server.on("/delete", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("hex")) {
      String hexStr = request->getParam("hex")->value();
      hexStr.replace(" ", "");
      if (hexStr.length() % 2 != 0 || hexStr.length() == 0) {
        request->send(400, "text/plain", "Neplatný hex kód!");
        return;
      }
      int len = hexStr.length() / 2;
      uint8_t data[MAX_CODE_LENGTH];
      for (int i = 0; i < len; i++) {
        String byteStr = hexStr.substring(i*2, i*2+2);
        data[i] = (uint8_t)strtol(byteStr.c_str(), NULL, 16);
      }
      int index = findCodeIndexByData(data, len);
      if (index == -1) {
        request->send(404, "text/plain", "Signál nenájdený");
        return;
      }
      deleteCodeFromEEPROM(index);
      request->send(200, "text/plain", "Vymazané");
      printEEPROMStatus();
    } else {
      request->send(200, "text/plain", "Chyba");
    }
  });

  server.on("/clear", HTTP_GET, [](AsyncWebServerRequest *request){
    clearAllCodesInEEPROM();
    request->send(200, "text/plain", "Všetko vymazané");
    printEEPROMStatus();
  });

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.begin();
  Serial.println("Server: http://192.168.4.1");
}

// === Loop ===
void loop() {
  if (isReceiving && (millis() - receiveStartTime) < 3000) {
    if (digitalRead(CC1101_GDO0_PIN) == LOW) {
      uint8_t rxBuffer[MAX_CODE_LENGTH];
      uint8_t rxLen = 0;
      if (cc1101.receive(rxBuffer, MAX_CODE_LENGTH, rxLen)) {
        if (rxLen > 0 && rxLen <= MAX_CODE_LENGTH) {
          memcpy(lastValidPacket, rxBuffer, rxLen);
          lastValidLength = rxLen;
          signalReceived = true;
          Serial.print("📡 Zachytený signál: ");
          for (int i = 0; i < rxLen; i++) Serial.printf("%02X ", rxBuffer[i]);
          Serial.printf(" (dĺžka: %d bajtov)\n", rxLen);
        }
      }
    }
  } else if (isReceiving) {
    isReceiving = false;
    if (lastValidLength > 0) {
      saveCodeToEEPROM(lastValidPacket, lastValidLength, pendingName.c_str());
      Serial.printf("✅ Uložený signál: %d bajtov, meno: %s\n", lastValidLength, pendingName.c_str());
      printEEPROMStatus();
    } else {
      Serial.println("❌ Žiadny signál");
    }
  }
  delay(10);
}
