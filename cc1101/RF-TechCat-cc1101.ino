#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <cc1101.h>  // https://github.com/SpaceTeddy/CC1101

// ===== NASTAVENIA =====
const char* ssid = "MojWiFi";
const char* password = "heslo123";
const uint16_t webPort = 80;

CC1101 radio;
WebServer server(webPort);

bool radioInitialized = false;
uint8_t rxBuffer[128];
uint8_t txBuffer[128];
size_t lastRxSize = 0;
String lastRxHex = "";
unsigned long lastRxTime = 0;

// ===== WEB ROZHRANIE =====
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <title>CC1101 RF Controller</title>
  <style>
    body { font-family: Arial, sans-serif; max-width: 800px; margin: 20px auto; padding: 20px; background: #f5f5f5; }
    .card { background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); margin-bottom: 20px; }
    h1 { color: #2c3e50; text-align: center; }
    button { padding: 12px 24px; font-size: 16px; margin: 10px 5px; border: none; border-radius: 5px; cursor: pointer; }
    .btn-receive { background: #3498db; color: white; }
    .btn-send { background: #2ecc71; color: white; }
    .btn-clear { background: #e74c3c; color: white; }
    textarea { width: 100%; height: 100px; padding: 10px; font-family: monospace; margin-top: 10px; }
    .status { padding: 15px; border-radius: 5px; margin: 15px 0; text-align: center; font-weight: bold; }
    .status-ok { background: #d4edda; color: #155724; }
    .status-error { background: #f8d7da; color: #721c24; }
    .status-rx { background: #d1ecf1; color: #0c5460; }
    .controls { display: flex; flex-wrap: wrap; gap: 10px; }
    .packet-size { margin: 15px 0; }
    input[type="text"] { width: 100%; padding: 10px; font-family: monospace; margin-top: 5px; }
  </style>
</head>
<body>
  <h1>📡 CC1101 RF Controller</h1>
  
  <div class="card">
    <div id="status" class="status status-ok">Radio ready @ 433.92 MHz</div>
  </div>

  <div class="card">
    <h2>📥 PRIJAŤ SIGNÁL</h2>
    <button class="btn-receive" onclick="receiveSignal()">▶️ Štart prijímania (5s)</button>
    <div class="packet-size">Posledný prijatý paket: <span id="rx-size">žiadny</span></div>
    <textarea id="rx-data" readonly placeholder="Tu sa zobrazí prijatý hexadecimálny signál..."></textarea>
  </div>

  <div class="card">
    <h2>📤 ODOSLAŤ SIGNÁL</h2>
    <div class="packet-size">
      <label><input type="radio" name="pkt-size" value="64" checked> 64-bit (8 B)</label>
      <label><input type="radio" name="pkt-size" value="128"> 128-bit (16 B)</label>
    </div>
    <input type="text" id="tx-data" placeholder="Zadaj hex (napr. A1B2C3D4E5F67890)" value="A1B2C3D4E5F67890">
    <div class="controls">
      <button class="btn-send" onclick="sendSignal()">📤 Odoslať</button>
      <button class="btn-clear" onclick="document.getElementById('tx-data').value=''">🗑️ Vyčistiť</button>
    </div>
  </div>

  <script>
    function updateStatus(msg, cls) {
      const el = document.getElementById('status');
      el.className = 'status ' + cls;
      el.textContent = msg;
    }

    function receiveSignal() {
      updateStatus('Čakám na signál...', 'status-rx');
      document.getElementById('rx-data').value = 'Pripravujem sa na prijatie...\n';
      
      fetch('/receive?duration=5000')
        .then(r => r.json())
        .then(d => {
          if (d.success && d.data) {
            document.getElementById('rx-data').value = d.data;
            document.getElementById('rx-size').textContent = d.size + ' B (' + (d.size*8) + '-bit)';
            updateStatus('✅ Prijatý ' + (d.size*8) + '-bitový signál', 'status-ok');
          } else {
            document.getElementById('rx-data').value = '❌ Žiadny signál neprijatý alebo chyba';
            updateStatus('⚠️ Čas vypršal - žiadny signál', 'status-error');
          }
        })
        .catch(e => {
          updateStatus('❌ Chyba komunikácie', 'status-error');
          console.error(e);
        });
    }

    function sendSignal() {
      const size = document.querySelector('input[name="pkt-size"]:checked').value;
      const data = document.getElementById('tx-data').value.trim();
      
      if (!data) {
        updateStatus('⚠️ Zadaj hexadecimálny signál', 'status-error');
        return;
      }
      
      updateStatus('Odosielam...', 'status-rx');
      fetch(`/send?size=${size}&data=${encodeURIComponent(data)}`)
        .then(r => r.json())
        .then(d => {
          if (d.success) {
            updateStatus(`✅ Odoslaný ${size}-bitový signál`, 'status-ok');
          } else {
            updateStatus(`❌ Chyba: ${d.error}`, 'status-error');
          }
        })
        .catch(e => {
          updateStatus('❌ Chyba odosielania', 'status-error');
          console.error(e);
        });
    }
  </script>
</body>
</html>
)rawliteral";

// ===== INICIALIZÁCIA =====
void setup() {
  Serial.begin(115200);
  SPI.begin();
  delay(100);

  // Pripojenie k WiFi
  WiFi.begin(ssid, password);
  Serial.print("Pripájam sa k WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi pripojené! IP: " + WiFi.localIP().toString());

  // Inicializácia CC1101
  if (radio.begin()) {
    radio.setFrequency(433.92);  // MHz - uprav podľa tvojho modulu
    radio.setPacketLength(128);  // max dĺžka
    radio.setPower(10);          // výkon v dBm
    radioInitialized = true;
    Serial.println("CC1101 inicializovaný");
  } else {
    Serial.println("❌ CC1101 inicializácia zlyhala!");
  }

  // Web server routy
  server.on("/", []() {
    server.send_P(200, "text/html", INDEX_HTML);
  });

  server.on("/receive", []() {
    if (!radioInitialized) {
      server.send(500, "application/json", "{\"success\":false,\"error\":\"Radio not ready\"}");
      return;
    }

    int duration = server.arg("duration").toInt();
    if (duration <= 0) duration = 5000;

    lastRxSize = 0;
    memset(rxBuffer, 0, sizeof(rxBuffer));
    
    radio.receive();  // prepni do RX módu
    unsigned long start = millis();
    
    while (millis() - start < (unsigned long)duration) {
      if (radio.available()) {
        lastRxSize = radio.read(rxBuffer, sizeof(rxBuffer));
        lastRxTime = millis();
        break;
      }
      delay(10);
    }

    radio.idle();  // ukonči RX

    if (lastRxSize > 0) {
      String hex = "";
      for (size_t i = 0; i < lastRxSize; i++) {
        char buf[3];
        sprintf(buf, "%02X", rxBuffer[i]);
        hex += buf;
        if ((i + 1) % 4 == 0 && i < lastRxSize - 1) hex += " ";
      }
      lastRxHex = hex;
      
      String json = "{\"success\":true,\"size\":" + String(lastRxSize) + 
                    ",\"data\":\"" + hex + "\"}";
      server.send(200, "application/json", json);
    } else {
      server.send(200, "application/json", "{\"success\":false,\"error\":\"timeout\"}");
    }
  });

  server.on("/send", []() {
    if (!radioInitialized) {
      server.send(500, "application/json", "{\"success\":false,\"error\":\"Radio not ready\"}");
      return;
    }

    int size = server.arg("size").toInt();
    String data = server.arg("data");
    
    if (size != 64 && size != 128) {
      server.send(400, "application/json", "{\"success\":false,\"error\":\"Invalid size\"}");
      return;
    }

    size_t byteLen = (size == 64) ? 8 : 16;
    memset(txBuffer, 0, sizeof(txBuffer));

    // Konverzia hex → bytes
    bool valid = true;
    data.replace(" ", "");
    if (data.length() < byteLen * 2) valid = false;
    
    for (size_t i = 0; i < byteLen && valid; i++) {
      if (i * 2 + 1 >= data.length()) {
        valid = false;
        break;
      }
      String byteStr = data.substring(i * 2, i * 2 + 2);
      txBuffer[i] = (uint8_t)strtol(byteStr.c_str(), NULL, 16);
    }

    if (!valid) {
      server.send(400, "application/json", "{\"success\":false,\"error\":\"Invalid hex\"}");
      return;
    }

    // Odoslanie
    radio.transmit();
    radio.write(txBuffer, byteLen);
    radio.idle();

    server.send(200, "application/json", "{\"success\":true}");
  });

  server.begin();
  Serial.println("Web server beží na http://" + WiFi.localIP().toString());
}

void loop() {
  server.handleClient();
  delay(1);
}
