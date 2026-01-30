#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <EEPROM.h>
#include <CC1101.h>  // https://github.com/SpaceTeddy/CC1101

// === WiFi nastavenia ===
const char* ssid = "ESP32_CC1101";
const char* password = "12345678";

// === CC1101 piny ===
#define CC1101_CS   5
#define CC1101_GDO0 4

// === Štruktúra pre dlhé kódy (až 128 bitov = 16 bajtov) ===
struct LongCodeItem {
  uint8_t code[16];    // 128 bitov = 16 bajtov
  uint8_t length;      // Dĺžka v BITOCH (nie bajtoch!)
  uint8_t protocol;    // 0=RCSwitch-like, 1=Raw packet
  char name[33];
};

#define MAX_CODES 20
#define CODE_ITEM_SIZE sizeof(LongCodeItem)
#define EEPROM_SIZE (MAX_CODES * CODE_ITEM_SIZE)

LongCodeItem savedCodes[MAX_CODES];
int codeCount = 0;

CC1101 radio(CC1101_CS, CC1101_GDO0);
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

bool isReceiving = false;
unsigned long receiveStartTime = 0;
uint8_t lastCode[16] = {0};
uint8_t lastCodeLength = 0;
String pendingName = "Unknown";

// Forward deklarácie
void loadCodesFromEEPROM();
void saveCodeToEEPROM(uint8_t* code, uint8_t length, uint8_t protocol, const char* name);
void transmitLongCode(uint8_t* code, uint8_t length, uint8_t protocol);

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<title>ESP32 CC1101 Control</title>
<style>
body {font-family: Arial; padding: 20px; background: #f0f2f5;}
.container {max-width: 800px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1);}
.section {margin-bottom: 20px; padding: 15px; border: 1px solid #ddd; border-radius: 8px;}
h2 {color: #333; border-bottom: 2px solid #4CAF50; padding-bottom: 8px;}
input, button {padding: 10px; margin: 5px 0; font-size: 16px;}
button {background: #4CAF50; color: white; border: none; border-radius: 5px; cursor: pointer;}
button:hover {background: #45a049;}
button.danger {background: #f44336;}
button.danger:hover {background: #d32f2f;}
.code-list {max-height: 300px; overflow-y: auto; margin-top: 15px;}
.code-item {padding: 10px; border: 1px solid #eee; margin: 5px 0; border-radius: 5px; display: flex; justify-content: space-between;}
.hex-display {font-family: monospace; background: #f8f8f8; padding: 3px 6px; border-radius: 3px; color: #c0392b;}
</style>
</head>
<body>
<div class="container">
  <h1>📡 ESP32 CC1101 RF Control</h1>
  
  <div class="section">
    <h2>📥 Prijímanie dlhých kódov</h2>
    <input type="text" id="nameInput" placeholder="Názov kódu (napr. Garáž)">
    <button onclick="startReceive()">Start Prijímanie (5s)</button>
    <div id="status">Stav: Čaká sa...</div>
  </div>
  
  <div class="section">
    <h2>📤 Odoslanie kódu</h2>
    <input type="text" id="hexInput" placeholder="Hex kód (napr. 0123456789ABCDEF)">
    <input type="number" id="bitLength" placeholder="Dĺžka v bitoch (napr. 64)" min="1" max="128">
    <button onclick="sendHexCode()">Odoslať Hex kód</button>
  </div>
  
  <div class="section">
    <h2>💾 Uložené kódy (až 128 bitov)</h2>
    <div id="codesList" class="code-list">Načítavam...</div>
  </div>
</div>

<script>
let ws = new WebSocket('ws://' + location.host + '/ws');

ws.onmessage = (event) => {
  const data = JSON.parse(event.data);
  if (data.type === 'signal_received') {
    document.getElementById('status').textContent = '✅ Zachytený kód: ' + data.hex;
    alert('Zachytený kód: ' + data.hex + '\nDĺžka: ' + data.length + ' bitov');
    loadCodes();
  } else if (data.type === 'receiving') {
    document.getElementById('status').textContent = '📡 Prijímanie... ' + data.time + 's';
  }
};

function startReceive() {
  const name = document.getElementById('nameInput').value || 'Nezmenovaný';
  fetch('/receive?name=' + encodeURIComponent(name));
  document.getElementById('status').textContent = '📡 Prijímanie začaté...';
}

function sendHexCode() {
  const hex = document.getElementById('hexInput').value.trim();
  const bits = document.getElementById('bitLength').value;
  fetch('/transmit', {
    method: 'POST',
    headers: {'Content-Type': 'application/x-www-form-urlencoded'},
    body: 'hex=' + hex + '&bits=' + bits
  }).then(() => alert('Odoslané!'));
}

function loadCodes() {
  fetch('/list').then(r => r.json()).then(codes => {
    const list = document.getElementById('codesList');
    list.innerHTML = codes.map(c => 
      `<div class="code-item">
        <div>
          <strong>${c.name}</strong><br>
          <span class="hex-display">${c.hex}</span><br>
          <small>${c.length} bitov</small>
        </div>
        <div>
          <button onclick="useCode('${c.hex}', ${c.length})">📤</button>
          <button class="danger" onclick="deleteCode(${c.index})">🗑️</button>
        </div>
      </div>`
    ).join('');
  });
}

function useCode(hex, bits) {
  document.getElementById('hexInput').value = hex;
  document.getElementById('bitLength').value = bits;
}

function deleteCode(index) {
  if (confirm('Vymazať?')) fetch('/delete?index=' + index).then(loadCodes);
}

window.onload = loadCodes;
</script>
</body>
</html>
)rawliteral";

void loadCodesFromEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  codeCount = 0;
  for (int i = 0; i < MAX_CODES; i++) {
    LongCodeItem item;
    EEPROM.get(i * CODE_ITEM_SIZE, item);
    if (item.length == 0 || item.length > 128) break; // Prázdny alebo neplatný záznam
    savedCodes[codeCount++] = item;
  }
}

void saveCodeToEEPROM(uint8_t* code, uint8_t length, uint8_t protocol, const char* name) {
  if (codeCount >= MAX_CODES) return;
  
  LongCodeItem item;
  memcpy(item.code, code, 16);
  item.length = length;
  item.protocol = protocol;
  strncpy(item.name, name, 31);
  item.name[31] = '\0';
  
  EEPROM.put(codeCount * CODE_ITEM_SIZE, item);
  EEPROM.commit();
  savedCodes[codeCount++] = item;
  
  Serial.printf("✅ Uložený dlhý kód (%d bitov): ", length);
  for (int i = 0; i < (length + 7) / 8; i++) Serial.printf("%02X", code[i]);
  Serial.printf(" (%s)\n", name);
}

// ✅ Kľúčová funkcia: Odoslanie ľubovoľne dlhého kódu (1-128 bitov)
void transmitLongCode(uint8_t* code, uint8_t length, uint8_t protocol) {
  if (length == 0 || length > 128) return;
  
  // Konverzia bitov na bajty
  uint8_t byteLen = (length + 7) / 8;
  uint8_t packet[byteLen];
  memcpy(packet, code, byteLen);
  
  if (protocol == 0) {
    // Jednoduchý režim: priama transmisia (ako RCSwitch)
    radio.setModeIdle();
    radio.transmit(packet, byteLen);
    radio.setModeRX();
  } else {
    // Pokročilý režim: vlastný paket s preamble/sync
    uint8_t fullPacket[byteLen + 4];
    fullPacket[0] = 0xAA; // Preamble
    fullPacket[1] = 0xAA;
    fullPacket[2] = 0x2D; // Sync word
    fullPacket[3] = 0xD4;
    memcpy(fullPacket + 4, packet, byteLen);
    
    radio.setModeIdle();
    radio.transmit(fullPacket, byteLen + 4);
    radio.setModeRX();
  }
  
  Serial.printf("📤 Odoslaný kód (%d bitov): ", length);
  for (int i = 0; i < byteLen; i++) Serial.printf("%02X", packet[i]);
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  
  // Inicializácia CC1101
  if (!radio.begin()) {
    Serial.println("❌ CC1101 inicializácia zlyhala!");
    while (1) delay(1000);
  }
  
  // Nastavenie frekvencie na 433.92 MHz
  radio.setFrequency(433.92);
  radio.setModulation(CCWOR); // OOK/ASK modulácia (kompatibilné s RCSwitch)
  radio.setDeviation(47.6);   // Deviácia pre OOK
  radio.setChannelBW(325.42); // Šírka pásma
  radio.setPower(10);         // Výkon v dBm
  radio.setModeRX();          // Spustiť prijímanie
  
  Serial.println("✅ CC1101 inicializovaný na 433.92 MHz");
  
  // WiFi AP
  WiFi.softAP(ssid, password);
  Serial.print("IP: ");
  Serial.println(WiFi.softAPIP());
  
  // EEPROM
  loadCodesFromEEPROM();
  Serial.printf("Načítané kódy: %d\n", codeCount);
  
  // Web server
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/html", index_html);
  });
  
  server.on("/list", HTTP_GET, [](AsyncWebServerRequest *request){
    String json = "[";
    for (int i = 0; i < codeCount; i++) {
      json += "{\"index\":" + String(i) + ",\"name\":\"" + savedCodes[i].name + "\",\"hex\":\"";
      int byteLen = (savedCodes[i].length + 7) / 8;
      for (int j = 0; j < byteLen; j++) json += String(savedCodes[i].code[j], HEX);
      json += "\",\"length\":" + String(savedCodes[i].length) + "}";
      if (i < codeCount - 1) json += ",";
    }
    json += "]";
    request->send(200, "application/json", json);
  });
  
  server.on("/receive", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("name")) {
      pendingName = request->getParam("name")->value();
    } else {
      pendingName = "Nezmenovaný";
    }
    isReceiving = true;
    receiveStartTime = millis();
    memset(lastCode, 0, 16);
    lastCodeLength = 0;
    request->send(200, "text/plain", "Prijímanie 5s...");
  });
  
  server.on("/transmit", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("hex", true) && request->hasParam("bits", true)) {
      String hex = request->getParam("hex", true)->value();
      uint8_t bits = request->getParam("bits", true)->value().toInt();
      
      // Konverzia hex stringu na bajty
      uint8_t code[16] = {0};
      int byteLen = (bits + 7) / 8;
      for (int i = 0; i < byteLen && i*2 < hex.length(); i++) {
        String byteStr = hex.substring(i*2, min((i+1)*2, hex.length()));
        code[i] = strtol(byteStr.c_str(), NULL, 16);
      }
      
      transmitLongCode(code, bits, 0);
      request->send(200, "text/plain", "Odoslané");
    } else {
      request->send(400, "text/plain", "Chýbajú parametre");
    }
  });
  
  server.on("/delete", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("index")) {
      int idx = request->getParam("index")->value().toInt();
      if (idx >= 0 && idx < codeCount) {
        // Posunutie záznamov
        for (int i = idx; i < codeCount - 1; i++) savedCodes[i] = savedCodes[i+1];
        codeCount--;
        
        // Prepísanie EEPROM
        EEPROM.begin(EEPROM_SIZE);
        for (int i = 0; i < MAX_CODES; i++) {
          if (i < codeCount) EEPROM.put(i * CODE_ITEM_SIZE, savedCodes[i]);
          else {
            LongCodeItem empty = {0};
            EEPROM.put(i * CODE_ITEM_SIZE, empty);
          }
        }
        EEPROM.commit();
      }
    }
    request->send(200, "text/plain", "OK");
  });
  
  ws.onEvent([](AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len){
    if (type == WS_EVT_CONNECT) Serial.printf("WebSocket pripojený\n");
  });
  server.addHandler(&ws);
  server.begin();
  Serial.println("Server beží na http://192.168.4.1");
}

void loop() {
  // Prijímanie dlhých kódov
  if (isReceiving && (millis() - receiveStartTime) < 5000) {
    uint8_t packet[64];
    uint8_t len = radio.receiveData(packet, sizeof(packet));
    
    if (len > 0) {
      // Detekcia dĺžky kódu (jednoduchá heuristika)
      lastCodeLength = len * 8; // Predpokladáme plný bajt
      memcpy(lastCode, packet, min(len, 16));
      
      // Odoslanie WebSocket správy
      String hex = "";
      for (int i = 0; i < min(len, 16); i++) hex += String(packet[i], HEX);
      
      String msg = "{\"type\":\"signal_received\",\"hex\":\"" + hex + "\",\"length\":" + String(lastCodeLength) + "}";
      ws.textAll(msg);
      
      Serial.printf("📡 Zachytený paket (%d bajtov): ", len);
      for (int i = 0; i < len; i++) Serial.printf("%02X ", packet[i]);
      Serial.println();
    }
    
    // Aktualizácia stavu prijímania
    if (millis() % 500 == 0) {
      int elapsed = (millis() - receiveStartTime) / 1000;
      String msg = "{\"type\":\"receiving\",\"time\":" + String(5 - elapsed) + "}";
      ws.textAll(msg);
    }
  } else if (isReceiving) {
    isReceiving = false;
    if (lastCodeLength > 0) {
      saveCodeToEEPROM(lastCode, lastCodeLength, 0, pendingName.c_str());
    }
  }
  
  delay(10);
}
