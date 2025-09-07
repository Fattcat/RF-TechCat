#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <RCSwitch.h>
#include <EEPROM.h>

// === WiFi nastavenia ===
const char* ssid = "ESP32_Control";
const char* password = "12345678";

// === RCSwitch ===
#define RX_PIN 2
#define TX_PIN 4

// === Ukladanie kódov ===
#define MAX_CODES 20
#define CODE_ITEM_SIZE sizeof(CodeItem)
#define EEPROM_SIZE (MAX_CODES * CODE_ITEM_SIZE)

struct CodeItem {
  long code;
  char name[33]; // 32 znakov + \0
};

CodeItem savedCodes[MAX_CODES];
int codeCount = 0;

// === Globálne premenné ===
RCSwitch mySwitch = RCSwitch();
AsyncWebServer server(80);
// === WebSocket Server ===
AsyncWebSocket ws("/ws");
bool isReceiving = false;
unsigned long receiveStartTime = 0;
long lastValidCode = -1;
String pendingName = "Unknown";
bool signalReceived = false;

// === HTML stránka s frekvenčnou mierkou a progress barom ===
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
        <h2>🔧 Manuálny vstup kódu</h2>
        <input type="text" id="codeInput" placeholder="Zadaj kód (napr. 1234567)" />
        <button onclick="validateAndSaveManualCode()" class="bg-green-600 hover:bg-green-700" style="background: #27ae60;">Overiť a uložiť kód</button>
        <div class="placeholder">
          <strong>Podporované kódy:</strong> Celé čísla od 1 do 16777215 (24-bitové).<br>
          <strong>Nepodporované:</strong> Desatinné čísla, písmená, medzery, znaky ako -, +, @.<br>
          <strong>Príklad:</strong> 1234567 – OK | abc123 – ZLE | 0.5 – ZLE
        </div>
      </div>
      <!-- Prijímanie s menom -->
      <div class="section">
        <h2>📥 Prijímanie a ukladanie</h2>
        <div class="flex">
          <input type="text" id="nameInput" placeholder="Názov (napr. Garáž)" />
          <button onclick="receiveAndSave()" id="receiveBtn">Receive & Save</button>
        </div>
        <button onclick="clearAllCodes()" class="danger">Vymazať všetky kódy</button>
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
        <h2>📤 Odoslanie kódu</h2>
        <button onclick="transmitCodeWithValidation()">Transmit</button>
        <button onclick="transmit3TimesWithValidation()">Transmit 3x (1s medzera)</button>
        <button onclick="startTransmitLoopWithValidation()">Transmit Loop (ON)</button>
        <button onclick="stopTransmitLoop()" class="danger">Stop Loop</button>
      </div>
      <!-- Rolling Codes -->
      <div class="section">
        <h2>🎲 Rolling Codes</h2>
        <div class="flex">
          <input type="number" id="rollFrom" placeholder="Od" value="1497000" min="1" max="16777215" step="1" />
          <input type="number" id="rollTo" placeholder="Do" value="1497100" min="1" max="16777215" step="1" />
        </div>
        <div class="flex" style="margin-top: 10px;">
          <input type="number" id="rollDelay" placeholder="Delay (ms)" value="500" min="50" max="2000" step="50" style="width: 120px;" />
          <button onclick="toggleRollingCodes()" id="rollBtn">▶️ Start RollingCodes</button>
        </div>
        <div class="placeholder" style="font-size: 12px; margin-top: 10px;">
          <strong>ℹ️ Pokyny:</strong>
          <ul style="margin: 5px 0; padding-left: 20px;">
            <li>Minimálny kód: <code>1</code>, Maximálny: <code>16777215</code> (24-bit)</li>
            <li>Delay: min <code>50ms</code> (rýchle), max <code>2000ms</code> (2s - pomalé, ale spoľahlivé)</li>
            <li>Odporúčaný delay: <code>100-500ms</code> pre väčšinu zariadení</li>
            <li>Po stlačení "Start" sa tlačidlo zmení na "Stop" (červené)</li>
          </ul>
        </div>
      </div>
      <!-- Live Log Odoslaných Kódov -->
      <div class="section">
        <h2>📡 Live Log Odoslaných Kódov</h2>
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
            <div id="progressSubtext" style="font-size: 14px; color: #666;">Celkom: 0/0 kódov</div>
          </div>
        </div>
        <!-- Log Area -->
        <div id="sendLog" style="height: 200px; overflow-y: auto; border: 1px solid #ddd; border-radius: 8px; padding: 10px; background: #f8f9fa; font-family: monospace; font-size: 14px; margin: 10px 0;">
          <div style="color: #666;">Žiadne odoslané kódy</div>
        </div>
        <button onclick="clearSendLog()" class="danger" style="width: 100%; padding: 10px; font-size: 16px; border-radius: 8px;">
          🧹 Clear Send codes
        </button>
      </div>
      <!-- Zoznam kódov -->
      <div class="section">
        <h2>💾 Uložené kódy</h2>
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
          <span class="popup-data-label">Kód:</span>
          <span class="popup-data-value"><code id="popupCode" class="popup-code"></code></span>
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
          // Inicializujeme progress bar
          updateProgress(data.total, 0);
          document.getElementById('progressStatus').textContent = '🔄 Prebieha odosielanie...';
          document.getElementById('progressStatus').style.color = '#3498db';
        } else if (data.type === 'roll_progress') {
          // Aktualizujeme progress bar
          updateProgress(data.total, data.current);
          // Voliteľne: pridáme kód do logu
          addToSendLog(data.current + fromCode - 1, 'odoslaný'); // Toto je aproximácia, pretože neposiela presný kód
        } else if (data.type === 'roll_complete') {
          if (data.success) {
            showPopup('Rolling Codes', true, { 
              message: `✅ Dokončené! Odoslaných ${data.count} kódov za ${data.duration}s` 
            });
            updateProgress(data.count, data.count); // Nastavíme na 100%
            document.getElementById('progressStatus').textContent = '✅ Dokončené!';
            document.getElementById('progressStatus').style.color = '#27ae60';
          } else {
            showPopup('Rolling Codes', false, { 
              message: `❌ Chyba: ${data.message}` 
            });
          }
          resetRollingButton();
        }
      } catch (e) {
        console.error('Chyba pri spracovaní WebSocket správy:', e);
      }
    };

    ws.onclose = function(event) {
      console.log('WebSocket disconnected, attempting to reconnect...');
      setTimeout(initWebSocket, 3000); // Skúsi znova pripojiť za 3 sekundy
    };

    ws.onerror = function(error) {
      console.error('WebSocket Error:', error);
    };
  }

  // Spustite WebSocket po načítaní stránky
  window.addEventListener('load', initWebSocket);

    let loopInterval = null;
    let canvas, ctx;
    let spectrumData = new Uint8Array(128);
    window.lastSignalReceived = false;
    let popupTimeout = null;
    let popupTimerInterval = null;
    let popupTimerValue = 5;
    let isCurrentlyReceiving = false;  // Nová premenná na sledovanie stavu prijímania
    let pendingReceiveName = "";  // Uloženie názvu pre aktuálne prijímanie
    let previousCodeCount = 0; // Premenná na sledovanie počtu kódov pred prijímaním

    // === Rolling Codes Control ===
    let isRolling = false;
    let rollingAbortController = null;

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

    // === Canvas spektrálna vizualizácia ===
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
      // Základný šum
      for (let i = 0; i < barCount; i++) {
        data[i] = Math.random() * 20 + 5;
      }
      // Signál bol zachytený
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

    // === Popup functions ===
    function showPopup(title, isSuccess, data = null) {
      const overlay = document.getElementById('popupOverlay');
      const messageDiv = document.getElementById('popupMessage');
      const dataDiv = document.getElementById('popupData');
      const titleDiv = document.getElementById('popupTitle');
      const statusRow = document.getElementById('popupStatusRow');
      const statusDiv = document.getElementById('popupStatus');

      // Set title
      titleDiv.textContent = title;

      // Set message and style
      if (isSuccess) {
        messageDiv.textContent = data?.message || 'Operácia úspešne dokončená!';
        messageDiv.className = 'popup-message popup-success';
      } else {
        messageDiv.textContent = data?.message || 'Chyba: Operácia zlyhala!';
        messageDiv.className = 'popup-message popup-error';
      }

      // Show or hide data section
      if (data && data.code !== undefined) {
        dataDiv.style.display = 'block';
        document.getElementById('popupCode').textContent = data.code;
        document.getElementById('popupName').textContent = data.name || 'Nezmenovaný';
        document.getElementById('popupFrequency').textContent = data.frequency || '433.92 MHz';
        // Show status if available
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

      // Show popup with animation
      document.querySelector('.container').style.filter = 'blur(3px)';
      overlay.classList.add('active');

      // Reset timer
      popupTimerValue = 5;
      document.getElementById('popupTimer').textContent = popupTimerValue;

      // Clear any existing timer
      if (popupTimerInterval) {
        clearInterval(popupTimerInterval);
      }

      // Start countdown timer
      popupTimerInterval = setInterval(() => {
        popupTimerValue--;
        document.getElementById('popupTimer').textContent = popupTimerValue;
        if (popupTimerValue <= 0) {
          closePopup();
        }
      }, 1000);

      // Auto-hide after 5 seconds
      if (popupTimeout) {
        clearTimeout(popupTimeout);
      }
      popupTimeout = setTimeout(closePopup, 5000);
    }

    function closePopup() {
      const overlay = document.getElementById('popupOverlay');
      overlay.classList.remove('active');
      document.querySelector('.container').style.filter = 'none';
      if (popupTimeout) {
        clearTimeout(popupTimeout);
        popupTimeout = null;
      }
      if (popupTimerInterval) {
        clearInterval(popupTimerInterval);
        popupTimerInterval = null;
      }
    }

    // === Validate code function ===
    function validateCode(codeStr) {
      // Check if empty
      if (!codeStr || codeStr.trim() === '') {
        return { isValid: false, message: 'Kód nesmie byť prázdny!' };
      }
      // Check if contains only digits
      if (!/^\d+$/.test(codeStr)) {
        return { isValid: false, message: 'Kód môže obsahovať iba číslice (0-9)!' };
      }
      // Convert to number
      const code = parseInt(codeStr, 10);
      // Check range
      if (code <= 0) {
        return { isValid: false, message: 'Kód musí byť väčší ako 0!' };
      }
      if (code > 16777215) {
        return { isValid: false, message: 'Kód presahuje maximálnu hodnotu 16777215 (24-bitový limit)!' };
      }
      return { isValid: true, code: code, message: 'Kód je platný!' };
    }

    // === Manual code validation and save ===
    function validateAndSaveManualCode() {
      const codeInput = document.getElementById('codeInput');
      const codeStr = codeInput.value.trim();

      // Validate code
      const validationResult = validateCode(codeStr);
      if (!validationResult.isValid) {
        // Show error popup
        showPopup('Chyba validácie kódu', false, { message: validationResult.message });
        return;
      }

      const code = validationResult.code;
      const savedName = "Saved: " + code;  // Názov podľa požiadavky

      // Code is valid, prepare data for popup
      const signalData = {
        code: code,
        name: savedName,
        frequency: '433.92 MHz',
        timestamp: new Date().toLocaleTimeString(),
        message: 'Kód bol úspešne overený a uložený!',
        status: 'Uložené do pamäte'
      };

      // Show success popup
      showPopup('Manuálny kód', true, signalData);

      // Save to server
      fetch('/saveManual', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'code=' + code + '&name=' + encodeURIComponent(savedName)
      })
      .then(response => response.text())
      .then(data => {
        console.log('Kód uložený:', data);
        showMessage('Kód ' + code + ' úspešne uložený!');
        updateCodesList();
      })
      .catch(error => {
        console.error('Chyba pri ukladaní:', error);
        showMessage('Chyba pri ukladaní kódu!', true);
      });
    }

    // === Enhanced transmit functions with validation ===
    function transmitCodeWithValidation() {
      const codeInput = document.getElementById('codeInput');
      const codeStr = codeInput.value.trim();

      // Validate code
      const validationResult = validateCode(codeStr);
      if (!validationResult.isValid) {
        // Show error popup
        showPopup('Chyba pri odosielaní', false, { message: validationResult.message });
        return;
      }

      const code = validationResult.code;

      // Show transmission popup
      const transmitData = {
        code: code,
        name: 'Odoslaný kód',
        frequency: '433.92 MHz',
        timestamp: new Date().toLocaleTimeString(),
        message: 'Kód bol úspešne odoslaný!',
        status: 'Odoslané cez RF'
      };

      showPopup('RF Odosielanie', true, transmitData);

      // Send to server
      fetch('/transmit', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'code=' + code
      })
      .then(response => response.text())
      .then(data => {
        console.log('Kód odoslaný:', data);
        showMessage('Kód ' + code + ' úspešne odoslaný!');
      })
      .catch(error => {
        console.error('Chyba pri odosielaní:', error);
        showMessage('Chyba pri odosielaní kódu!', true);
      });
    }

    function transmit3TimesWithValidation() {
      const codeInput = document.getElementById('codeInput');
      const codeStr = codeInput.value.trim();

      // Validate code
      const validationResult = validateCode(codeStr);
      if (!validationResult.isValid) {
        // Show error popup
        showPopup('Chyba pri odosielaní', false, { message: validationResult.message });
        return;
      }

      const code = validationResult.code;

      // Show transmission popup
      const transmitData = {
        code: code,
        name: 'Odoslaný kód (3x)',
        frequency: '433.92 MHz',
        timestamp: new Date().toLocaleTimeString(),
        message: 'Kód bol úspešne odoslaný 3 krát!',
        status: 'Odoslané 3x cez RF'
      };

      showPopup('RF Odosielanie', true, transmitData);

      // Send to server
      fetch('/transmit3', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'code=' + code
      })
      .then(response => response.text())
      .then(data => {
        console.log('Kód odoslaný 3x:', data);
        showMessage('Kód ' + code + ' úspešne odoslaný 3x!');
      })
      .catch(error => {
        console.error('Chyba pri odosielaní:', error);
        showMessage('Chyba pri odosielaní kódu!', true);
      });
    }

    function startTransmitLoopWithValidation() {
      const codeInput = document.getElementById('codeInput');
      const codeStr = codeInput.value.trim();

      // Validate code
      const validationResult = validateCode(codeStr);
      if (!validationResult.isValid) {
        // Show error popup
        showPopup('Chyba pri odosielaní', false, { message: validationResult.message });
        return;
      }

      const code = validationResult.code;

      // Show transmission popup
      const transmitData = {
        code: code,
        name: 'Loop odosielania',
        frequency: '433.92 MHz',
        timestamp: new Date().toLocaleTimeString(),
        message: 'Loop odosielania bol spustený!',
        status: 'Opakované odosielanie'
      };

      showPopup('RF Odosielanie', true, transmitData);

      // Send to server
      fetch('/transmit', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'code=' + code
      })
      .then(response => response.text())
      .then(data => {
        console.log('Loop odosielania spustený:', data);
        showMessage('Loop odosielania spustený pre kód ' + code + '!');
        // Start local loop simulation
        if (loopInterval) clearInterval(loopInterval);
        loopInterval = setInterval(() => {
          fetch('/transmit', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: 'code=' + code
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

    // === Progress bar pri prijímaní ===
    function receiveAndSave() {
      const name = document.getElementById('nameInput').value.trim() || 'Nezmenovaný';
      const btn = document.getElementById('receiveBtn');
      const fill = document.getElementById('receiveFill');

      // Nastavíme globálne premenné pre aktuálne prijímanie
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

        // Po 3 sekundách ukončíme prijímanie a zrušíme progress bar
        if (elapsed >= 3000) {
          clearInterval(interval);
          if (isCurrentlyReceiving) {
            isCurrentlyReceiving = false;
            btn.disabled = false;
            btn.textContent = 'Receive & Save';
            // NEZOBRAZUJEME CHYBOVÝ POPUP TU, PRETOŽE SIGNÁL SA MOHOL ZACHYTIŤ NESKÔR
            // Namiesto toho spustíme kontrolu zoznamu kódov
            checkForNewCodeAfterReceive();
          }
        }
      }, 100);

      // Odoslanie požiadavky na server
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

    // === NOVÁ FUNKCIA NA KONTROLU, ČI SA PO PRIJÍMANÍ ULOŽIL NOVÝ KÓD ===
    function checkForNewCodeAfterReceive() {
      // Najskôr získame aktuálny počet kódov
      fetch('/list')
        .then(res => res.json())
        .then(codes => {
          const currentCodeCount = codes.length;
          if (currentCodeCount > previousCodeCount) {
            // Počet kódov sa zvýšil, znamená to, že sa niečo uložilo
            previousCodeCount = currentCodeCount; // Aktualizujeme počítadlo
            updateCodesList(); // Aktualizujeme UI
            // Získame posledný kód (predpokladáme, že je nový)
            const lastCode = codes[codes.length - 1];
            const signalData = {
              code: lastCode.code,
              name: lastCode.name,
              frequency: '433.92 MHz',
              message: 'Signál úspešne zachytený a uložený!'
            };
            showPopup('RF Prijímanie', true, signalData);
          } else {
            // Po 3 sekundách sa nič neuložilo, zobrazíme chybu
            showPopup('RF Prijímanie', false, { 
              message: 'Signál nebol zachytený. Skúste to znova.' 
            });
          }
        })
        .catch(err => {
          console.error('Chyba pri kontrole nového kódu:', err);
          showPopup('RF Prijímanie', false, { 
            message: 'Signál nebol zachytený. Skúste to znova.' 
          });
        });
    }

    // === Rolling Codes Control ===
    function toggleRollingCodes() {
      const btn = document.getElementById('rollBtn');
      if (isRolling) {
        // Zastaviť
        isRolling = false;
        if (rollingAbortController) {
          rollingAbortController.abort();
          rollingAbortController = null;
        }
        btn.textContent = '▶️ Start RollingCodes';
        btn.style.background = '#3498db';
        btn.style.color = 'white';
        showMessage('Rolling Codes zastavené');
        showPopup('Rolling Codes', true, { message: 'Rolling Codes boli úspešne zastavené!' });
        updateProgress(totalCodesToProcess, processedCodesCount);
      } else {
        // Spustiť
        const fromCode = parseInt(document.getElementById('rollFrom').value);
        const toCode = parseInt(document.getElementById('rollTo').value);
        const delayMs = parseInt(document.getElementById('rollDelay').value);

        // Validácia
        if (isNaN(fromCode) || isNaN(toCode) || isNaN(delayMs)) {
          showPopup('Chyba', false, { message: 'Všetky polia musia byť vyplnené číslami!' });
          return;
        }
        if (fromCode < 1 || fromCode > 16777215 || toCode < 1 || toCode > 16777215) {
          showPopup('Chyba', false, { message: 'Kódy musia byť v rozsahu 1 - 16777215!' });
          return;
        }
        if (fromCode > toCode) {
          showPopup('Chyba', false, { message: '"Od" nesmie byť väčšie ako "Do"!' });
          return;
        }
        if (delayMs < 50 || delayMs > 2000) {
          showPopup('Chyba', false, { message: 'Delay musí byť medzi 50ms a 2000ms!' });
          return;
        }

        // Vymazanie predchádzajúceho logu a inicializácia
        clearSendLog();
        const totalCount = toCode - fromCode + 1;
        updateProgress(totalCount, 0);

        // Spustenie
        isRolling = true;
        btn.textContent = '⏹️ Stop';
        btn.style.background = '#e74c3c';
        btn.style.color = 'white';

        showPopup('Rolling Codes', true, { 
          message: `Spúšťam od ${fromCode} po ${toCode} s oneskorením ${delayMs}ms` 
        });

        rollingAbortController = new AbortController();

        fetch('/roll', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: `from=${fromCode}&to=${toCode}&delay=${delayMs}`,
          signal: rollingAbortController.signal
        })
        .then(response => response.json())
        .then(data => {
          if (data.success) {
            showPopup('Rolling Codes', true, { 
              message: `✅ Dokončené! Odoslaných ${data.count} kódov za ${data.duration}s` 
            });
          } else {
            showPopup('Rolling Codes', false, { 
              message: `❌ Chyba: ${data.message}` 
            });
          }
          resetRollingButton();
          updateProgress(data.total || 0, data.count || 0);
        })
        .catch(err => {
          if (err.name !== 'AbortError') {
            showPopup('Rolling Codes', false, { message: 'Chyba: ' + err.message });
          }
          resetRollingButton();
          updateProgress(totalCodesToProcess, processedCodesCount);
        });
      }
    }

    function resetRollingButton() {
      const btn = document.getElementById('rollBtn');
      isRolling = false;
      rollingAbortController = null;
      btn.textContent = '▶️ Start RollingCodes';
      btn.style.background = '#3498db';
      btn.style.color = 'white';
    }

    // === Live Log Functions ===
    let sendLogEntries = [];
    let totalCodesToProcess = 0;
    let processedCodesCount = 0;

    function updateProgress(total, current) {
      totalCodesToProcess = total;
      processedCodesCount = current;
      
      const percentage = total > 0 ? Math.round((current / total) * 100) : 0;
      
      // Update progress circle
      const circle = document.getElementById('progressCircle');
      const text = document.getElementById('progressText');
      const status = document.getElementById('progressStatus');
      const subtext = document.getElementById('progressSubtext');
      
      if (circle && text) {
        const circumference = 2 * Math.PI * 45; // 2πr
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
        subtext.textContent = `Celkom: ${current}/${total} kódov`;
      }
    }

    function addToSendLog(code, status = 'odoslaný') {
      const logDiv = document.getElementById('sendLog');
      if (!logDiv) return;
      
      const timestamp = new Date().toLocaleTimeString();
      const entry = {
        code: code,
        timestamp: timestamp,
        status: status
      };
      
      sendLogEntries.push(entry);
      
      const div = document.createElement('div');
      div.style.padding = '5px 0';
      div.style.borderBottom = '1px solid #eee';
      div.style.color = status === 'chyba' ? '#e74c3c' : '#2c3e50';
      
      div.innerHTML = `[${timestamp}] <code style="background: #e9ecef; padding: 2px 4px; border-radius: 3px;">${code}</code> - ${status === 'chyba' ? '❌ Chyba' : '✅ Úspešne'}`;
      
      logDiv.appendChild(div);
      logDiv.scrollTop = logDiv.scrollHeight; // Auto-scroll to bottom
    }

    function clearSendLog() {
      const logDiv = document.getElementById('sendLog');
      if (logDiv) {
        logDiv.innerHTML = '<div style="color: #666;">Žiadne odoslané kódy</div>';
      }
      sendLogEntries = [];
      updateProgress(0, 0);
      showMessage('Log odoslaných kódov bol vymazaný');
    }

    function initializeSendLog() {
      updateProgress(0, 0);
    }

    // === Zvyšok funkcii ===
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
          previousCodeCount = codes.length; // <-- DÔLEŽITÉ: Aktualizujeme globálne počítadlo
          if (codes.length === 0) {
            list.innerHTML = '<p>Žiadne uložené kódy.</p>';
            return;
          }
          codes.forEach(item => {
            const div = document.createElement('div');
            div.className = 'code-item';
            div.dataset.code = item.code;
            div.innerHTML = `
              <div class="code-info">
                <div><strong id="name-${item.code}">${item.name}</strong></div>
                <div style="font-family:monospace;color:#c0392b">Kód: ${item.code}</div>
                <input type="text" id="edit-${item.code}" value="${item.name}" 
                       style="display:none;margin-top:4px;padding:5px;width:100%" />
              </div>
              <div class="code-actions">
                <button onclick="useCode(${item.code})" title="Použiť">📋</button>
                <button onclick="sendStored(${item.code})" title="Odoslať">📤</button>
                <button onclick="startEdit(${item.code})" title="Upraviť">✎</button>
                <button onclick="saveEdit(${item.code})" style="display:none" title="Uložiť">✔️</button>
                <button onclick="deleteCode(${item.code})" class="danger" title="Vymazať">🗑️</button>
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

    function startEdit(code) {
      document.getElementById(`name-${code}`).style.display = 'none';
      const input = document.getElementById(`edit-${code}`);
      input.style.display = 'inline-block';
      input.focus();
      document.querySelector(`[onclick="saveEdit(${code})"]`).style.display = 'inline-block';
      document.querySelector(`[onclick="startEdit(${code})"]`).style.display = 'none';
    }

    function saveEdit(code) {
      const newName = document.getElementById(`edit-${code}`).value.trim() || 'Nezmenovaný';
      fetch('/updateName', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'code=' + code + '&name=' + encodeURIComponent(newName)
      })
      .then(() => {
        showMessage('Meno zmenené: ' + newName);
        updateCodesList();
      })
      .catch(err => {
        showMessage('Chyba: ' + err.message, true);
      });
    }

    function useCode(code) {
      document.getElementById('codeInput').value = code;
      showMessage(`Kód ${code} použitý`);
    }

    function sendStored(code) {
      useCode(code);
      transmitCodeWithValidation();
    }

    function stopTransmitLoop() {
      if (loopInterval) clearInterval(loopInterval);
      loopInterval = null;
      showMessage('Loop zastavený');
      // Show popup for stopping loop
      const stopData = {
        message: 'Loop odosielania bol zastavený!',
        status: 'Odosielanie zastavené'
      };
      showPopup('RF Odosielanie', true, stopData);
    }

    function deleteCode(code) {
      if (confirm('Vymazať tento kód?')) {
        fetch('/delete?code=' + code, { method: 'GET' })
          .then(() => {
            showMessage('Kód vymazaný');
            updateCodesList();
            // Show popup for deletion
            const deleteData = {
              code: code,
              message: 'Kód bol úspešne vymazaný!',
              status: 'Vymazané z pamäte'
            };
            showPopup('Správa systému', true, deleteData);
          });
      }
    }

    function clearAllCodes() {
      if (confirm('Vymazať všetky kódy?')) {
        fetch('/clear', { method: 'GET' })
          .then(() => {
            showMessage('Všetko vymazané');
            updateCodesList();
            // Show popup for clearing all codes
            const clearData = {
              message: 'Všetky kódy boli úspešne vymazané!',
              status: 'Pamäť vyčistená'
            };
            showPopup('Správa systému', true, clearData);
          });
      }
    }

    // Spusti
    initSpectrum();
    updateCodesList();
    initializeSendLog(); // <-- Pridané: Inicializácia logu
    
    // Add event listener for popup close button
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
    if (item.code == 0 || item.code == 0xFFFFFFFF) break;
    bool valid = false;
    for (int j = 0; j < 32; j++) {
      char c = item.name[j];
      if (c == '\0') break;
      if (c >= 32 && c <= 126) valid = true;
    }
    if (!valid) strcpy(item.name, "Nezmenovaný");
    savedCodes[codeCount++] = item;
  }
  EEPROM.end();
}

int findCodeIndex(long code) {
  for (int i = 0; i < codeCount; i++) {
    if (savedCodes[i].code == code) return i;
  }
  return -1;
}

void saveCodeToEEPROM(long code, const char* name) {
  if (codeCount >= MAX_CODES) return;
  CodeItem item;
  item.code = code;
  strncpy(item.name, name, 32);
  item.name[32] = '\0';
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(codeCount * CODE_ITEM_SIZE, item);
  EEPROM.commit();
  EEPROM.end();
  savedCodes[codeCount++] = item;
}

void updateNameInEEPROM(long code, const char* newName) {
  int index = findCodeIndex(code);
  if (index == -1) return;
  strncpy(savedCodes[index].name, newName, 32);
  savedCodes[index].name[32] = '\0';
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(index * CODE_ITEM_SIZE, savedCodes[index]);
  EEPROM.commit();
  EEPROM.end();
}

void deleteCodeFromEEPROM(long code) {
  int index = findCodeIndex(code);
  if (index == -1) return;
  EEPROM.begin(EEPROM_SIZE);
  for (int i = index; i < codeCount - 1; i++) {
    savedCodes[i] = savedCodes[i + 1];
  }
  codeCount--;
  for (int i = 0; i < MAX_CODES; i++) {
    if (i < codeCount) {
      EEPROM.put(i * CODE_ITEM_SIZE, savedCodes[i]);
    } else {
      CodeItem empty = {0, ""};
      EEPROM.put(i * CODE_ITEM_SIZE, empty);
    }
  }
  EEPROM.commit();
  EEPROM.end();
}

void clearAllCodesInEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  for (int i = 0; i < MAX_CODES; i++) {
    CodeItem empty = {0, ""};
    EEPROM.put(i * CODE_ITEM_SIZE, empty);
  }
  EEPROM.commit();
  EEPROM.end();
  codeCount = 0;
}

// === Stav EEPROM ===
void printEEPROMStatus() {
  int used = codeCount;
  int total = MAX_CODES;
  float percent = (float)used / total * 100;
  int usedBytes = used * CODE_ITEM_SIZE;
  int totalBytes = EEPROM_SIZE;
  Serial.println("\n--- EEPROM Stav ---");
  Serial.printf("Kódy: %d / %d (%.1f %%)\n", used, total, percent);
  Serial.printf("Záznam: %d B\n", CODE_ITEM_SIZE);
  Serial.printf("Použité: %d / %d B\n", usedBytes, totalBytes);
  Serial.println("-------------------");
}

// === WebSocket Callback ===
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("WebSocket Client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
  } else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("WebSocket Client #%u disconnected\n", client->id());
  }
  // Ostatné eventy nás momentálne nezaujímajú
}

// === Setup ===
void setup() {
  Serial.begin(115200);

  // WiFi
  WiFi.softAP(ssid, password);
  Serial.println("");
  Serial.print("WiFi AP: ");
  Serial.println(ssid);
  Serial.print("IP: ");
  Serial.println(WiFi.softAPIP());

  // RCSwitch
  mySwitch.enableReceive(RX_PIN);
  mySwitch.enableTransmit(TX_PIN);
  Serial.println("RCSwitch: RX=2, TX=4");

  // EEPROM
  loadCodesFromEEPROM();
  Serial.printf("sizeof(CodeItem) = %d\n", sizeof(CodeItem));
  Serial.printf("Debug: codeCount = %d\n", codeCount);
  for (int i = 0; i < codeCount; i++) {
    Serial.printf("Kód[%d]: %ld, Meno: %s\n", i, savedCodes[i].code, savedCodes[i].name);
  }
  printEEPROMStatus();

  // Web server
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/html", index_html);
  });

  server.on("/list", HTTP_GET, [](AsyncWebServerRequest *request){
    String json = "[";
    for (int i = 0; i < codeCount; i++) {
      String cleanName = "";
      for (int j = 0; j < 32; j++) {
        char c = savedCodes[i].name[j];
        if (c == '\0') break;
        if (c >= 32 && c <= 126) {
          if (c == '"') cleanName += "\\\"";
          else if (c == '\\') cleanName += "\\\\";
          else if (c == '\n') cleanName += "\\n";
          else if (c == '\r') cleanName += "\\r";
          else cleanName += c;
        }
      }
      if (cleanName.length() == 0) cleanName = "Nezmenovaný";
      json += "{\"name\":\"" + cleanName + "\",\"code\":" + String(savedCodes[i].code) + "}";
      if (i < codeCount - 1) json += ",";
    }
    json += "]";
    AsyncWebServerResponse *response = request->beginResponse(200, "application/json", json);
    response->addHeader("Access-Control-Allow-Origin", "*");
    response->addHeader("Content-Type", "application/json");
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
    lastValidCode = -1;
    receiveStartTime = millis();
    request->send(200, "text/plain", "Prijímanie (3s)...");
  });

  // === NOVÝ HANDLER PRE ULOŽENIE MANUÁLNEHO KÓDU ===
  server.on("/saveManual", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("code", true) && request->hasParam("name", true)) {
      long code = request->getParam("code", true)->value().toInt();
      String name = request->getParam("name", true)->value();
      // Overenie kódu (voliteľné, ale odporúčané)
      if (code > 0 && code <= 16777215) {
        saveCodeToEEPROM(code, name.c_str());
        Serial.printf("✅ Manuálne uložený kód: %ld (%s)\n", code, name.c_str());
        printEEPROMStatus();
        request->send(200, "text/plain", "OK"); // Dôležité: Pošleme OK, aby JS vedel, že to prebehlo úspešne
      } else {
        request->send(400, "text/plain", "Neplatný kód");
      }
    } else {
      request->send(400, "text/plain", "Chýbajúce parametre");
    }
  });

  server.on("/transmit", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("code", true)) {
      long code = request->getParam("code", true)->value().toInt();
      if (code > 0 && code <= 16777215) {
        mySwitch.send(code, 24);
        request->send(200, "text/plain", "Odoslané");
      } else {
        request->send(200, "text/plain", "Neplatný kód");
      }
    } else {
      request->send(200, "text/plain", "Chýba kód");
    }
  });

  server.on("/transmit3", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("code", true)) {
      long code = request->getParam("code", true)->value().toInt();
      if (code > 0 && code <= 16777215) {
        for (int i = 0; i < 3; i++) {
          mySwitch.send(code, 24);
          delay(1000);
        }
        request->send(200, "text/plain", "Odoslané 3x");
      } else {
        request->send(200, "text/plain", "Neplatný kód");
      }
    } else {
      request->send(200, "text/plain", "Chýba kód");
    }
  });

  server.on("/updateName", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("code", true) && request->hasParam("name", true)) {
      long code = request->getParam("code", true)->value().toInt();
      String newName = request->getParam("name", true)->value();
      updateNameInEEPROM(code, newName.c_str());
      request->send(200, "text/plain", "OK");
    } else {
      request->send(400, "text/plain", "Missing parameters");
    }
  });

  server.on("/delete", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("code")) {
      long code = request->getParam("code")->value().toInt();
      deleteCodeFromEEPROM(code);
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

  // === HANDLER PRE ROLLING CODES === (OPRAVENÝ PRE ASYNCHRÓNNE SPRACOVANIE)
  server.on("/roll", HTTP_POST, [](AsyncWebServerRequest *request){
      if (!request->hasParam("from", true) || 
          !request->hasParam("to", true) || 
          !request->hasParam("delay", true)) {
        request->send(400, "application/json", "{\"success\":false,\"message\":\"Chýbajúce parametre\"}");
        return;
      }
      long fromCode = request->getParam("from", true)->value().toInt();
      long toCode = request->getParam("to", true)->value().toInt();
      int delayMs = request->getParam("delay", true)->value().toInt();
      if (fromCode < 1 || toCode > 16777215 || fromCode > toCode || delayMs < 50 || delayMs > 2000) {
        request->send(400, "application/json", "{\"success\":false,\"message\":\"Neplatný rozsah\"}");
        return;
      }

      // Vytvoríme kópiu parametrov pre novú úlohu
      struct RollParams {
          long fromCode;
          long toCode;
          int delayMs;
      };

      RollParams *params = new RollParams{fromCode, toCode, delayMs};

      // Vytvoríme a spustíme novú FreeRTOS úlohu
      xTaskCreatePinnedToCore(
          [](void *param) {
              RollParams *p = (RollParams *)param;
              long fromCode = p->fromCode;
              long toCode = p->toCode;
              int delayMs = p->delayMs;
              delete p; // Uvoľníme pamäť

              // Vypneme watchdog na oboch jadrách
              disableCore0WDT();
              disableCore1WDT();

              unsigned long startTime = millis();
              int count = 0;
              int totalCount = toCode - fromCode + 1;

              // Odosleme správu cez WebSocket, že úloha začala
              String startMsg = "{\"type\":\"roll_start\",\"total\":" + String(totalCount) + "}";
              ws.textAll(startMsg);

              for (long code = fromCode; code <= toCode; code++) {
                  mySwitch.send(code, 24);
                  count++;

                  // Odosleme aktualizáciu priebehu cez WebSocket
                  String progressMsg = "{\"type\":\"roll_progress\",\"current\":" + String(count) + ",\"total\":" + String(totalCount) + "}";
                  ws.textAll(progressMsg);

                  yield();

                  if (delayMs > 0 && code < toCode) {
                      unsigned long startDelay = millis();
                      while (millis() - startDelay < delayMs) {
                          yield();
                          delay(1);
                      }
                  }
              }

              unsigned long duration = millis() - startTime;
              float seconds = duration / 1000.0;

              // Zapneme watchdog späť
              enableCore0WDT();
              enableCore1WDT();

              // Odosleme finálnu správu cez WebSocket
              String resultMsg = "{\"type\":\"roll_complete\",\"success\":true,\"count\":" + String(count) + ",\"duration\":" + String(seconds, 2) + "}";
              ws.textAll(resultMsg);

              // Odstránime úlohu
              vTaskDelete(NULL);
          },
          "RollingTask",   // Názov úlohy
          8192,            // Zväčšený zásobník kvôli reťazcom
          params,          // Parameter pre úlohu
          1,               // Priorita úlohy
          NULL,            // Handle úlohy
          1                // Core (1 = druhé jadro)
      );

      // Okamžite pošleme odpoveď, že proces bol spustený.
      request->send(202, "application/json", "{\"success\":true,\"message\":\"Rolling Codes spustené na pozadí\"}");
  });

  // Nastavte WebSocket callback
  ws.onEvent(onWsEvent);
  // Pripojte WebSocket k serveru
  server.addHandler(&ws);
  // Spustite server

  server.begin();
  Serial.println("Server: http://192.168.4.1");
}

// === Loop ===
void loop() {
  if (isReceiving && (millis() - receiveStartTime) < 3000) {
    if (mySwitch.available()) {
      long value = mySwitch.getReceivedValue();
      int bits = mySwitch.getReceivedBitlength();
      if (bits == 24 && value > 0) {
        lastValidCode = value;
        signalReceived = true;
        Serial.printf("📡 Zachytený signál: %ld\n", value);
      }
      mySwitch.resetAvailable();
    }
  } else if (isReceiving) {
    isReceiving = false;
    if (lastValidCode != -1) {
      saveCodeToEEPROM(lastValidCode, pendingName.c_str());
      Serial.printf("✅ Uložený: %ld (%s)\n", lastValidCode, pendingName.c_str());
      printEEPROMStatus();
    } else {
      Serial.println("❌ Žiadny signál");
    }
  }
  delay(10);
}
