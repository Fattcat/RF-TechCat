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

// === ŠTRUKTÚRA MUSÍ BYŤ PRED MAKRAMI! ===
struct CodeItem {
  long code;
  int bitLength;   // Dĺžka signálu v bitoch (1-32)
  char name[33];   // 32 znakov + null terminator
};

// ✅ MAKRÁ PO ŠTRUKTÚRE (kritické pre správnu veľkosť)
#define MAX_CODES 20
#define CODE_ITEM_SIZE sizeof(CodeItem)
#define EEPROM_SIZE (MAX_CODES * CODE_ITEM_SIZE)

// === Globálne premenné ===
CodeItem savedCodes[MAX_CODES];
int codeCount = 0;

RCSwitch mySwitch = RCSwitch();
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// === Pre prijímanie ===
bool isReceiving = false;
unsigned long receiveStartTime = 0;
long lastValidCode = -1;
int lastValidBitLength = 0;
String pendingName = "Unknown";

// === Pre Rolling Codes ===
volatile bool rollingShouldStop = false;

// ✅ FORWARD DEKLARÁCIA (pred setup())
int getBitLengthForCode(long code);

// === HTML stránka ===
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="sk">
<head>
<meta charset="UTF-8" />
<meta name="viewport" content="width=device-width, initial-scale=1.0"/>
<title>ESP32 RF Control</title>
<style>
body {font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;background: linear-gradient(135deg, #6a11cb 0%, #2575fc 100%);color: #333;margin: 0;padding: 20px;min-height: 100vh;transition: filter 0.3s ease;}
.container {max-width: 900px;margin: 0 auto;background: white;border-radius: 15px;box-shadow: 0 10px 30px rgba(0, 0, 0, 0.2);overflow: hidden;}
header {background: #2c3e50;color: white;padding: 20px;text-align: center;}
header h1 {margin: 0;font-size: 28px;}
.content {padding: 20px;}
.section {margin-bottom: 30px;padding: 20px;background: #f9f9f9;border-radius: 10px;border: 1px solid #e0e0e0;}
.section h2 {margin-top: 0;color: #2c3e50;border-bottom: 2px solid #3498db;padding-bottom: 10px;}
input[type="text"], input[type="number"] {width: 100%;padding: 12px;margin: 10px 0;border: 2px solid #ddd;border-radius: 8px;font-size: 16px;box-sizing: border-box;}
.flex {display: flex;gap: 10px;align-items: center;}
.flex input {flex: 1;}
button {background: #3498db;color: white;border: none;padding: 12px 20px;margin: 5px;border-radius: 8px;cursor: pointer;font-size: 16px;transition: background 0.3s;}
button:hover {background: #2980b9;}
button.danger {background: #e74c3c;}
button.danger:hover {background: #c0392b;}
.placeholder {background: #fffde7;padding: 15px;border-radius: 8px;border: 1px solid #ffe082;font-size: 14px;line-height: 1.6;color: #5d4037;}
.progress-container {width: 100%;background: #eee;border-radius: 10px;padding: 2px;margin: 10px 0;}
.progress-bar {height: 20px;border-radius: 8px;background: linear-gradient(90deg, #4caf50, #8bc34a);text-align: center;color: white;font-size: 14px;line-height: 20px;transition: width 0.1s linear;}
.receive-progress {width: 100%;height: 10px;background: #ddd;border-radius: 5px;overflow: hidden;margin: 10px 0 5px;}
.receive-fill {height: 100%;width: 0%;background: #2980b9;transition: width 0.1s linear;}
.codes-list {max-height: 500px;overflow-y: auto;border: 1px solid #ddd;border-radius: 8px;padding: 10px;background: #f8f9fa;}
.code-item {padding: 12px;margin: 8px 0;background: white;border: 1px solid #ddd;border-radius: 8px;display: flex;justify-content: space-between;align-items: center;font-size: 14px;box-shadow: 0 1px 3px rgba(0,0,0,0.1);}
.code-info {flex: 1;}
.code-info strong {font-size: 16px;color: #2c3e50;}
.code-info code {font-family: monospace;background: #f0f0f0;padding: 4px 8px;border-radius: 4px;color: #c0392b;}
.code-actions {display: flex;gap: 5px;}
.code-actions button {padding: 6px 10px;font-size: 14px;}
.message {margin-top: 10px;padding: 10px;background: #d4edda;color: #155724;border: 1px solid #c3e6cb;border-radius: 5px;display: none;}
.popup-overlay {position: fixed;top: 0;left: 0;width: 100%;height: 100%;background-color: rgba(0, 0, 0, 0.5);display: flex;align-items: center;justify-content: center;z-index: 1000;opacity: 0;visibility: hidden;transition: opacity 0.3s, visibility 0.3s;}
.popup-overlay.active {opacity: 1;visibility: visible;}
.popup-content {background: white;border-radius: 15px;padding: 25px;width: 90%;max-width: 400px;box-shadow: 0 10px 30px rgba(0, 0, 0, 0.3);transform: translateY(-50px);opacity: 0;transition: all 0.5s ease;position: relative;}
.popup-overlay.active .popup-content {transform: translateY(0);opacity: 1;}
body.popup-active .container {filter: blur(3px);}
.popup-header {display: flex;justify-content: space-between;align-items: center;margin-bottom: 20px;padding-bottom: 10px;border-bottom: 2px solid #eee;}
.popup-header h3 {margin: 0;color: #333;font-size: 20px;}
.popup-close {background: none;border: none;font-size: 24px;color: #e74c3c;cursor: pointer;width: 30px;height: 30px;display: flex;align-items: center;justify-content: center;border-radius: 50%;transition: background-color 0.3s;}
.popup-close:hover {background-color: #f8f9fa;}
.popup-message {padding: 15px;border-radius: 8px;margin-bottom: 20px;text-align: center;font-weight: bold;}
.popup-success {background-color: #d4edda;color: #155724;border: 1px solid #c3e6cb;}
.popup-error {background-color: #f8d7da;color: #721c24;border: 1px solid #f5c6cb;}
.popup-data {background-color: #f8f9fa;border-radius: 8px;padding: 15px;margin-bottom: 15px;}
.popup-data-item {display: flex;justify-content: space-between;padding: 8px 0;border-bottom: 1px solid #eee;}
.popup-data-item:last-child {border-bottom: none;}
.popup-data-label {font-weight: bold;color: #555;}
.popup-data-value {color: #333;}
.popup-code {font-family: monospace;background-color: #e9ecef;padding: 3px 6px;border-radius: 4px;font-weight: normal;}
.popup-timer {text-align: center;font-size: 12px;color: #6c757d;margin-top: 10px;}
</style>
</head>
<body>
<div class="container">
<header>
<h1>|RF Control Panel</h1>
</header>
<div class="content">
<div class="section">
<h2>📊 Stav pamäte</h2>
<div>Použité: <span id="usedSlots">0</span> / 20 kódov</div>
<div class="progress-container">
<div class="progress-bar" id="progressBar" style="width:0%">0%</div>
</div>
</div>
<div class="section">
<h2>🔧 Manuálny vstup kódu</h2>
<input type="text" id="codeInput" placeholder="Zadaj kód (napr. 1234567)" />
<button onclick="validateAndSaveManualCode()" style="background: #27ae60;">Overiť a uložiť kód</button>
<div class="placeholder">
<strong>Podporované kódy:</strong> Celé čísla od 1 do 4294967295 (32-bitové).<br>
<strong>Nepodporované:</strong> Desatinné čísla, písmená, medzery, znaky ako -, +, @.<br>
<strong>Príklad:</strong> 1234567 – OK | abc123 – ZLE | 0.5 – ZLE
</div>
</div>
<div class="section">
<h2>📥 Prijímanie a ukladanie</h2>
<div class="flex">
<input type="text" id="nameInput" placeholder="Názov (napr. Garáž)" />
<button onclick="receiveAndSave()" id="receiveBtn">Receive & Save</button>
</div>
<button onclick="clearAllCodes()" class="danger">Vymazať všetky kódy</button>
<div class="receive-progress">
<div class="receive-fill" id="receiveFill"></div>
</div>
</div>
<div class="section">
<h2>📡 RF Spektrálna analýza</h2>
<div class="signal-animation" style="background: #000; border: 1px solid #0f0; box-shadow: 0 0 10px rgba(0, 255, 0, 0.3);">
<canvas id="spectrumCanvas"></canvas>
</div>
<div class="frequency-scale" style="color:#0f0;font-family:monospace;font-size:13px;">
<span>433.0</span>
<span>433.4</span>
<span>433.8</span>
<span>433.92</span>
<span>434.2</span>
<span>434.6</span>
</div>
</div>
<div class="section">
<h2>📤 Odoslanie kódu</h2>
<button onclick="transmitCodeWithValidation()">Transmit</button>
<button onclick="transmit3TimesWithValidation()">Transmit 3x (1s medzera)</button>
<button onclick="startTransmitLoopWithValidation()">Transmit Loop (ON)</button>
<button onclick="stopTransmitLoop()" class="danger">Stop Loop</button>
</div>
<div class="section">
<h2>🎲 Rolling Codes</h2>
<div class="flex">
<input type="number" id="rollFrom" placeholder="Od" value="1497000" min="1" max="4294967295" step="1" />
<input type="number" id="rollTo" placeholder="Do" value="1497100" min="1" max="4294967295" step="1" />
</div>
<div class="flex" style="margin-top: 10px;">
<input type="number" id="rollDelay" placeholder="Delay (ms)" value="500" min="50" max="2000" step="50" style="width: 120px;" />
<button onclick="toggleRollingCodes()" id="rollBtn">▶️ Start RollingCodes</button>
</div>
<div class="placeholder" style="font-size: 12px; margin-top: 10px;">
<strong>ℹ️ Pokyny:</strong>
<ul style="margin: 5px 0; padding-left: 20px;">
<li>Minimálny kód: <code>1</code>, Maximálny: <code>4294967295</code> (32-bit)</li>
<li>Delay: min <code>50ms</code> (rýchle), max <code>2000ms</code> (2s - pomalé, ale spoľahlivé)</li>
<li>Odporúčaný delay: <code>100-500ms</code> pre väčšinu zariadení</li>
<li>Po stlačení "Start" sa tlačidlo zmení na "Stop" (červené)</li>
</ul>
</div>
</div>

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
<!-- Log Area - VYLEPŠENÉ -->
<div style="background: #000; color: #0f0; font-family: monospace; font-size: 16px; padding: 15px; border-radius: 8px; border: 2px solid #0f0; height: 300px; overflow-y: auto; margin: 10px 0;">
<div style="color: #888; margin-bottom: 10px;">▼ Posledné odoslané kódy:</div>
<div id="sendLog" style="line-height: 1.6;"></div>
</div>
<button onclick="clearSendLog()" class="danger" style="width: 100%; padding: 10px; font-size: 16px; border-radius: 8px;">
🧹 Clear Send codes
</button>
</div>

<div class="section">
<h2>💾 Uložené kódy</h2>
<div id="codesList" class="codes-list">
Načítavam...
</div>
</div>
<div id="message" class="message"></div>
</div>
</div>
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
<div class="popup-data-item">
<span class="popup-data-label">Dĺžka bitov:</span>
<span class="popup-data-value" id="popupBitLength">24</span>
</div>
</div>
<div class="popup-timer">Okno sa automaticky zatvorí za <span id="popupTimer">5</span> sekúnd</div>
</div>
</div>
<script>
// ✅ IBA JEDNA DEKLARÁCIA PREMENNÝCH
let ws = null;
let popupTimeout = null;
let popupTimerInterval = null;
let popupTimerValue = 5;
let canvas, ctx;
let spectrumData = new Uint8Array(128);
let signalAnimationActive = false;
let signalAnimationEndTime = 0;
let signalBitLength = 24;
let loopInterval = null;
let isRolling = false;

// === WebSocket ===
function initWebSocket() {
  const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
  const url = protocol + '//' + window.location.host + '/ws';
  ws = new WebSocket(url);
  ws.onopen = () => console.log('WebSocket connected');

  ws.onmessage = (event) => {
    try {
      const data = JSON.parse(event.data);
      if (data.type === 'signal_received') {
        document.getElementById('receiveFill').style.width = '100%';
        triggerSignalAnimation(data.bitLength || 24);
        showPopup('RF Prijímanie', true, {
          code: data.code,
          name: data.name,
          frequency: '433.92 MHz',
          message: 'Signál úspešne zachytený a uložený!',
          bitLength: data.bitLength
        });
        updateCodesList();
      } else if (data.type === 'signal_timeout') {
        document.getElementById('receiveFill').style.width = '100%';
        showPopup('RF Prijímanie', false, {
          message: 'Signál nebol zachytený. Skúste to znova.'
        });
      } else if (data.type === 'roll_start') {
        updateProgress(data.total, 0);
        document.getElementById('progressStatus').textContent = '🔄 Prebieha odosielanie...';
        document.getElementById('progressStatus').style.color = '#3498db';
      } else if (data.type === 'roll_progress') {
        console.log('🔄 Progress:', data); // ✅ DEBUG
        updateProgress(data.total, data.current);
        if (data.code !== undefined) {
          addToSendLog(data.code, 'odoslaný');
          //triggerSignalAnimation(24, true);
        }
      } else if (data.type === 'roll_complete') {
        if (data.success) {
          showPopup('Rolling Codes', true, {
            message: `✅ Dokončené! Odoslaných ${data.count} kódov za ${data.duration}s`
          });
          updateProgress(data.count, data.count);
          document.getElementById('progressStatus').textContent = '✅ Dokončené!';
          document.getElementById('progressStatus').style.color = '#27ae60';
        }
        resetRollingButton();
      } else if (data.type === 'roll_stopped') {
        showPopup('Rolling Codes', true, {
          message: `⏹️ Zastavené! Odoslaných ${data.count} kódov`
        });
        updateProgress(data.count, data.count);
        document.getElementById('progressStatus').textContent = '⏹️ Zastavené používateľom';
        document.getElementById('progressStatus').style.color = '#e74c3c';
        resetRollingButton();
      }
    } catch (e) {
      console.error('WebSocket error:', e);
    }
  };
  ws.onclose = () => setTimeout(initWebSocket, 3000);
}

// ✅ SPEKTRUM
function initSpectrum() {
  canvas = document.getElementById('spectrumCanvas');
  if (!canvas) return;
  ctx = canvas.getContext('2d');
  const dpi = window.devicePixelRatio || 1;
  canvas.width = canvas.offsetWidth * dpi;
  canvas.height = 100 * dpi;
  ctx.scale(dpi, dpi);
  requestAnimationFrame(updateSpectrum);
}

function updateSpectrum() {
  if (!canvas || !ctx) return;
  const width = canvas.width / (window.devicePixelRatio || 1);
  const height = canvas.height / (window.devicePixelRatio || 1);
  
  for (let i = 0; i < spectrumData.length; i++) {
    let noise = 5 + Math.random() * 20;
    if (Math.random() > 0.95) noise += Math.random() * 30;
    spectrumData[i] = noise;
  }
  
  if (signalAnimationActive && Date.now() < signalAnimationEndTime) {
    const elapsed = Date.now() - (signalAnimationEndTime - 300);
    const intensity = Math.sin(elapsed * 0.05) * 50 + 70;
    const width = signalBitLength >= 32 ? 24 : 16;
    const center = 64;
    
    for (let i = 0; i < width; i++) {
      const pos = center - width/2 + i;
      if (pos >= 0 && pos < spectrumData.length) {
        const distance = Math.abs(i - width/2);
        const falloff = 1 - (distance / (width/2));
        spectrumData[pos] = Math.max(spectrumData[pos], intensity * falloff);
      }
    }
  } else {
    signalAnimationActive = false;
  }
  
  drawSpectrum();
  requestAnimationFrame(updateSpectrum);
}

function drawSpectrum() {
  if (!canvas || !ctx) return;
  const width = canvas.width / (window.devicePixelRatio || 1);
  const height = canvas.height / (window.devicePixelRatio || 1);
  const barWidth = width / spectrumData.length;
  
  const gradient = ctx.createLinearGradient(0, 0, 0, height);
  gradient.addColorStop(0, '#0a0e17');
  gradient.addColorStop(1, '#1a2332');
  ctx.fillStyle = gradient;
  ctx.fillRect(0, 0, width, height);
  
  for (let i = 0; i < spectrumData.length; i++) {
    const v = spectrumData[i];
    const barHeight = (v / 100) * height;
    const x = i * barWidth;
    
    let r, g, b;
    if (v < 30) { r = 0; g = Math.min(100 + v * 2, 200); b = 50; }
    else if (v < 60) { r = 200 + v; g = 180; b = 50; }
    else { r = 255; g = Math.max(0, 255 - (v - 60) * 2); b = Math.max(0, 200 - (v - 60) * 3); }
    
    if (v > 85) ctx.fillStyle = `rgb(255, 255, 255)`;
    else ctx.fillStyle = `rgb(${r}, ${g}, ${b})`;
    
    ctx.fillRect(x, height - barHeight - 1, barWidth - 1.5, barHeight);
  }
  
  const center = width / 2;
  ctx.strokeStyle = signalAnimationActive ? 'rgba(255, 50, 50, 0.9)' : 'rgba(100, 255, 100, 0.7)';
  ctx.lineWidth = 2;
  ctx.beginPath();
  ctx.moveTo(center, height - 5);
  ctx.lineTo(center, height - 25);
  ctx.stroke();
  
  ctx.fillStyle = signalAnimationActive ? '#ff5555' : '#55ff55';
  ctx.font = '12px monospace';
  ctx.textAlign = 'center';
  ctx.fillText('433.92 MHz', center, height - 30);
}

function triggerSignalAnimation(bitLength = 24) {
  signalAnimationActive = true;
  signalBitLength = bitLength;
  signalAnimationEndTime = Date.now() + 300;
}

// ✅ ZÁKLADNÉ FUNKCIE
function showMessage(text, isError = false) {
  const msg = document.getElementById('message');
  if (!msg) return;
  msg.style.display = 'block';
  msg.textContent = text;
  msg.style.background = isError ? '#f8d7da' : '#d4edda';
  msg.style.color = isError ? '#721c24' : '#155724';
  setTimeout(() => msg.style.display = 'none', 3000);
}

function updateMemoryUsage(used) {
  const percent = Math.round((used / 20) * 100);
  document.getElementById('usedSlots').textContent = used;
  const bar = document.getElementById('progressBar');
  if (bar) {
    bar.style.width = percent + '%';
    bar.textContent = percent + '%';
  }
}

// ✅ PROGRESS BAR FUNKCIE
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
      status.style.color = '#666';
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
  const div = document.createElement('div');
  div.style.padding = '4px 0';
  div.style.borderBottom = '1px solid #080';
  div.style.color = '#0f0';
  div.style.fontFamily = 'monospace';
  div.style.fontSize = '15px';
  div.innerHTML = `<span style="color:#888">[${timestamp}]</span> ${code.toString().padStart(10, ' ')}`;
  
  logDiv.appendChild(div);
  
  // ✅ Automatické skrolovanie na najnovší záznam
  setTimeout(() => {
    logDiv.scrollTop = logDiv.scrollHeight;
  }, 10);
  
  // ✅ Maximálne 100 záznamov (pre výkon)
  const entries = logDiv.querySelectorAll('div');
  if (entries.length > 100) {
    logDiv.removeChild(entries[0]);
  }
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

// ✅ KĽÚČOVÁ FUNKCIA: Načítanie kódov
function updateCodesList() {
  const list = document.getElementById('codesList');
  if (!list) {
    console.error('❌ Element #codesList neexistuje!');
    return;
  }
  
  list.innerHTML = '<p>Načítavam...</p>';
  
  fetch('/list', {
    headers: { 'Cache-Control': 'no-cache' }
  })
  .then(response => {
    if (!response.ok) throw new Error('HTTP ' + response.status);
    return response.json();
  })
  .then(codes => {
    console.log('✅ Načítané kódy:', codes);
    list.innerHTML = '';
    updateMemoryUsage(codes.length);
    
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
          <div><strong>${item.name}</strong></div>
          <div style="font-family:monospace;color:#c0392b">Kód: ${item.code}</div>
          <div style="font-family:monospace;color:#666">Bitov: ${item.bitLength}</div>
        </div>
        <div class="code-actions">
          <button onclick="useCode(${item.code})" title="Použiť">📋</button>
          <button onclick="sendStored(${item.code})" title="Odoslať">📤</button>
          <button onclick="deleteCode(${item.code})" class="danger" title="Vymazať">🗑️</button>
        </div>
      `;
      list.appendChild(div);
    });
  })
  .catch(err => {
    console.error('❌ Chyba pri načítaní kódov:', err);
    list.innerHTML = `<p style="color:red">Chyba: ${err.message}</p>`;
    showMessage('Chyba pri načítaní kódov!', true);
  });
}

// ✅ POPUP FUNKCIE
function showPopup(title, isSuccess, data = null) {
  const overlay = document.getElementById('popupOverlay');
  const messageDiv = document.getElementById('popupMessage');
  const dataDiv = document.getElementById('popupData');
  const titleDiv = document.getElementById('popupTitle');
  const bitLengthDiv = document.getElementById('popupBitLength');
  
  if (!overlay || !messageDiv) return;
  
  titleDiv.textContent = title;
  messageDiv.className = isSuccess ? 'popup-message popup-success' : 'popup-message popup-error';
  messageDiv.textContent = data?.message || (isSuccess ? 'Operácia úspešne dokončená!' : 'Chyba: Operácia zlyhala!');
  
  if (data && data.code !== undefined) {
    dataDiv.style.display = 'block';
    document.getElementById('popupCode').textContent = data.code;
    document.getElementById('popupName').textContent = data.name || 'Nezmenovaný';
    document.getElementById('popupFrequency').textContent = data.frequency || '433.92 MHz';
    bitLengthDiv.textContent = data.bitLength || '24';
  } else {
    dataDiv.style.display = 'none';
  }
  
  document.querySelector('.container').style.filter = 'blur(3px)';
  overlay.classList.add('active');
  
  popupTimerValue = 5;
  const timerEl = document.getElementById('popupTimer');
  if (timerEl) timerEl.textContent = popupTimerValue;
  
  if (popupTimerInterval) clearInterval(popupTimerInterval);
  popupTimerInterval = setInterval(() => {
    popupTimerValue--;
    if (timerEl) timerEl.textContent = popupTimerValue;
    if (popupTimerValue <= 0) closePopup();
  }, 1000);
  
  if (popupTimeout) clearTimeout(popupTimeout);
  popupTimeout = setTimeout(closePopup, 5000);
}

function closePopup() {
  const overlay = document.getElementById('popupOverlay');
  if (!overlay) return;
  overlay.classList.remove('active');
  document.querySelector('.container').style.filter = 'none';
  if (popupTimeout) clearTimeout(popupTimeout);
  if (popupTimerInterval) clearInterval(popupTimerInterval);
}

// ✅ OSTATNÉ FUNKCIE
function validateCode(codeStr) {
  if (!codeStr || codeStr.trim() === '') {
    return { isValid: false, message: 'Kód nesmie byť prázdny!' };
  }
  if (!/^\d+$/.test(codeStr)) {
    return { isValid: false, message: 'Kód môže obsahovať iba číslice (0-9)!' };
  }
  const code = parseInt(codeStr, 10);
  if (code <= 0) {
    return { isValid: false, message: 'Kód musí byť väčší ako 0!' };
  }
  if (code > 4294967295) {
    return { isValid: false, message: 'Kód presahuje maximálnu hodnotu 4294967295 (32-bitový limit)!' };
  }
  return { isValid: true, code: code, message: 'Kód je platný!' };
}

function validateAndSaveManualCode() {
  const codeInput = document.getElementById('codeInput');
  const codeStr = codeInput.value.trim();
  const validationResult = validateCode(codeStr);
  if (!validationResult.isValid) {
    showPopup('Chyba validácie kódu', false, { message: validationResult.message });
    return;
  }
  const code = validationResult.code;
  const savedName = "Saved: " + code;
  showPopup('Manuálny kód', true, {
    code: code,
    name: savedName,
    frequency: '433.92 MHz',
    message: 'Kód bol úspešne overený a uložený!',
    status: 'Uložené do pamäte',
    bitLength: 24
  });
  fetch('/saveManual', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'code=' + code + '&name=' + encodeURIComponent(savedName)
  })
  .then(() => {
    showMessage('Kód ' + code + ' úspešne uložený!');
    updateCodesList();
  })
  .catch(error => {
    console.error('Chyba pri ukladaní:', error);
    showMessage('Chyba pri ukladaní kódu!', true);
  });
}

function transmitCodeWithValidation() {
  const codeInput = document.getElementById('codeInput');
  const codeStr = codeInput.value.trim();
  const validationResult = validateCode(codeStr);
  if (!validationResult.isValid) {
    showPopup('Chyba pri odosielaní', false, { message: validationResult.message });
    return;
  }
  const code = validationResult.code;
  showPopup('RF Odosielanie', true, {
    code: code,
    name: 'Odoslaný kód',
    frequency: '433.92 MHz',
    message: 'Kód bol úspešne odoslaný!',
    status: 'Odoslané cez RF'
  });
  fetch('/transmit', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'code=' + code
  })
  .then(() => showMessage('Kód ' + code + ' úspešne odoslaný!'))
  .catch(error => {
    console.error('Chyba pri odosielaní:', error);
    showMessage('Chyba pri odosielaní kódu!', true);
  });
}

function transmit3TimesWithValidation() {
  const codeInput = document.getElementById('codeInput');
  const codeStr = codeInput.value.trim();
  const validationResult = validateCode(codeStr);
  if (!validationResult.isValid) {
    showPopup('Chyba pri odosielaní', false, { message: validationResult.message });
    return;
  }
  const code = validationResult.code;
  showPopup('RF Odosielanie', true, {
    code: code,
    name: 'Odoslaný kód (3x)',
    frequency: '433.92 MHz',
    message: 'Kód bol úspešne odoslaný 3 krát!',
    status: 'Odoslané 3x cez RF'
  });
  fetch('/transmit3', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'code=' + code
  })
  .then(() => showMessage('Kód ' + code + ' úspešne odoslaný 3x!'))
  .catch(error => {
    console.error('Chyba pri odosielaní:', error);
    showMessage('Chyba pri odosielaní kódu!', true);
  });
}

function startTransmitLoopWithValidation() {
  const codeInput = document.getElementById('codeInput');
  const codeStr = codeInput.value.trim();
  const validationResult = validateCode(codeStr);
  if (!validationResult.isValid) {
    showPopup('Chyba pri odosielaní', false, { message: validationResult.message });
    return;
  }
  const code = validationResult.code;
  showPopup('RF Odosielanie', true, {
    code: code,
    name: 'Loop odosielania',
    frequency: '433.92 MHz',
    message: 'Loop odosielania bol spustený!',
    status: 'Opakované odosielanie'
  });
  fetch('/transmit', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'code=' + code
  })
  .then(() => {
    showMessage('Loop odosielania spustený pre kód ' + code + '!');
    if (loopInterval) clearInterval(loopInterval);
    loopInterval = setInterval(() => {
      fetch('/transmit', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'code=' + code
      }).catch(error => console.error('Chyba pri odosielaní v loope:', error));
    }, 1000);
  })
  .catch(error => {
    console.error('Chyba pri spustení loopu:', error);
    showMessage('Chyba pri spustení loopu!', true);
  });
}

function receiveAndSave() {
  const name = document.getElementById('nameInput').value.trim() || 'Nezmenovaný';
  const btn = document.getElementById('receiveBtn');
  const fill = document.getElementById('receiveFill');
  
  btn.disabled = true;
  btn.textContent = 'Prijímanie...';
  fill.style.width = '0%';
  
  fetch('/receive', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'name=' + encodeURIComponent(name)
  })
  .then(() => console.log('Prijímanie spustené'))
  .catch(() => {
    showMessage('Chyba pri spustení prijímania', true);
    btn.disabled = false;
    btn.textContent = 'Receive & Save';
    fill.style.width = '0%';
  });
  
  // Reset progress baru
  fill.style.transition = 'none';
  fill.style.width = '0%';
  setTimeout(() => {
    fill.style.transition = 'width 0.1s linear';
    fill.style.width = '100%';
  }, 10);
  
  // Automatické zastavenie po 3s
  setTimeout(() => {
    btn.disabled = false;
    btn.textContent = 'Receive & Save';
  }, 3000);
}

function toggleRollingCodes() {
  const btn = document.getElementById('rollBtn');
  if (isRolling) {
    isRolling = false;
    fetch('/rollStop', { method: 'POST' })
      .then(() => showMessage('Zastavujem Rolling Codes...'))
      .catch(err => {
        console.error('Chyba pri zastavovaní:', err);
        btn.textContent = '▶️ Start RollingCodes';
        btn.style.background = '#3498db';
        btn.style.color = 'white';
      });
  } else {
    const fromCode = parseInt(document.getElementById('rollFrom').value);
    const toCode = parseInt(document.getElementById('rollTo').value);
    const delayMs = parseInt(document.getElementById('rollDelay').value);
    
    if (isNaN(fromCode) || isNaN(toCode) || isNaN(delayMs)) {
      showPopup('Chyba', false, { message: 'Všetky polia musia byť vyplnené číslami!' });
      return;
    }
    if (fromCode < 1 || fromCode > 4294967295 || toCode < 1 || toCode > 4294967295) {
      showPopup('Chyba', false, { message: 'Kódy musia byť v rozsahu 1 - 4294967295!' });
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
    
    isRolling = true;
    btn.textContent = '⏹️ STOP';
    btn.style.background = '#e74c3c';
    btn.style.color = 'white';
    
    showPopup('Rolling Codes', true, {
      message: `Spúšťam od ${fromCode} po ${toCode} s oneskorením ${delayMs}ms`
    });
    
    fetch('/roll', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: `from=${fromCode}&to=${toCode}&delay=${delayMs}`
    })
    .catch(err => {
      console.error('Chyba pri spúšťaní Rolling Codes:', err);
      showPopup('Rolling Codes', false, { message: 'Chyba: ' + err.message });
      isRolling = false;
      btn.textContent = '▶️ Start RollingCodes';
      btn.style.background = '#3498db';
      btn.style.color = 'white';
    });
  }
}

function resetRollingButton() {
  const btn = document.getElementById('rollBtn');
  if (btn) {
    btn.textContent = '▶️ Start RollingCodes';
    btn.style.background = '#3498db';
    btn.style.color = 'white';
    isRolling = false;
  }
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
  showPopup('RF Odosielanie', true, {
    message: 'Loop odosielania bol zastavený!',
    status: 'Odosielanie zastavené'
  });
}

function deleteCode(code) {
  if (confirm('Vymazať tento kód?')) {
    fetch('/delete?code=' + code, { method: 'GET' })
      .then(() => {
        showMessage('Kód vymazaný');
        updateCodesList();
        showPopup('Správa systému', true, {
          code: code,
          message: 'Kód bol úspešne vymazaný!',
          status: 'Vymazané z pamäte'
        });
      });
  }
}

function clearAllCodes() {
  if (confirm('Vymazať všetky kódy?')) {
    fetch('/clear', { method: 'GET' })
      .then(() => {
        showMessage('Všetko vymazané');
        updateCodesList();
        showPopup('Správa systému', true, {
          message: 'Všetky kódy boli úspešne vymazané!',
          status: 'Pamäť vyčistená'
        });
      });
  }
}

// ✅ INICIALIZÁCIA
window.addEventListener('load', () => {
  console.log('✅ Stránka načítaná - inicializujem komponenty...');
  initWebSocket();
  initSpectrum();
  updateCodesList();
  initializeSendLog();  // ✅ Pridané
  
  const overlay = document.getElementById('popupOverlay');
  if (overlay) {
    overlay.addEventListener('click', e => { if (e.target === overlay) closePopup(); });
  }
});
</script>
</body>
</html>
)rawliteral";

// === EEPROM OPERÁCIE (BEZ begin()/end() VNÚTRI!) ===
void loadCodesFromEEPROM() {
  codeCount = 0;
  for (int i = 0; i < MAX_CODES; i++) {
    CodeItem item;
    EEPROM.get(i * CODE_ITEM_SIZE, item);
    if (item.code == 0 || item.code == 0xFFFFFFFF) break;
    
    // Validácia mena
    bool valid = false;
    for (int j = 0; j < 32; j++) {
      char c = item.name[j];
      if (c == '\0') break;
      if (c >= 32 && c <= 126) valid = true;
    }
    if (!valid) strcpy(item.name, "Nezmenovaný");
    
    // Validácia bitLength
    if (item.bitLength < 1 || item.bitLength > 32) {
      item.bitLength = 24;
    }
    
    savedCodes[codeCount++] = item;
  }
}

int findCodeIndex(long code) {
  for (int i = 0; i < codeCount; i++) {
    if (savedCodes[i].code == code) return i;
  }
  return -1;
}

void saveCodeToEEPROM(long code, int bitLength, const char* name) {
  if (codeCount >= MAX_CODES) return;
  
  CodeItem item;
  item.code = code;
  item.bitLength = bitLength;
  
  // ✅ BEZPEČNÉ KOPIROVANIE MENA (31 znakov + null terminator)
  strncpy(item.name, name, 31);
  item.name[31] = '\0';
  
  // Uloženie do EEPROM
  EEPROM.put(codeCount * CODE_ITEM_SIZE, item);
  EEPROM.commit();  // ✅ Kritické: uloží zmeny do flash
  
  savedCodes[codeCount++] = item;
}

void updateNameInEEPROM(long code, const char* newName) {
  int index = findCodeIndex(code);
  if (index == -1) return;
  
  strncpy(savedCodes[index].name, newName, 31);
  savedCodes[index].name[31] = '\0';
  
  EEPROM.put(index * CODE_ITEM_SIZE, savedCodes[index]);
  EEPROM.commit();
}

void deleteCodeFromEEPROM(long code) {
  int index = findCodeIndex(code);
  if (index == -1) return;
  
  for (int i = index; i < codeCount - 1; i++) {
    savedCodes[i] = savedCodes[i + 1];
  }
  codeCount--;
  
  // Prepísanie EEPROM
  for (int i = 0; i < MAX_CODES; i++) {
    if (i < codeCount) {
      EEPROM.put(i * CODE_ITEM_SIZE, savedCodes[i]);
    } else {
      CodeItem empty = {0, 0, ""};
      EEPROM.put(i * CODE_ITEM_SIZE, empty);
    }
  }
  EEPROM.commit();
}

void clearAllCodesInEEPROM() {
  for (int i = 0; i < MAX_CODES; i++) {
    CodeItem empty = {0, 0, ""};
    EEPROM.put(i * CODE_ITEM_SIZE, empty);
  }
  EEPROM.commit();
  codeCount = 0;
}

// === Stav EEPROM ===
void printEEPROMStatus() {
  int used = codeCount;
  int total = MAX_CODES;
  float percent = (float)used / total * 100;
  int usedBytes = used * CODE_ITEM_SIZE;
  Serial.println("\n--- EEPROM Stav ---");
  Serial.printf("Kódy: %d / %d (%.1f %%)\n", used, total, percent);
  Serial.printf("Záznam: %d B\n", CODE_ITEM_SIZE);
  Serial.printf("Použité: %d / %d B\n", usedBytes, EEPROM_SIZE);
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
  
  // ✅ JEDINÉ volanie EEPROM.begin() v celom programe - PRED načítaním kódov!
  if (!EEPROM.begin(EEPROM_SIZE)) {
    Serial.println("❌ EEPROM inicializácia zlyhala!");
    return;
  }
  
  WiFi.softAP(ssid, password);
  Serial.println("");
  Serial.print("WiFi AP: ");
  Serial.println(ssid);
  Serial.print("IP: ");
  Serial.println(WiFi.softAPIP());

  mySwitch.enableReceive(RX_PIN);
  mySwitch.enableTransmit(TX_PIN);
  Serial.println("RCSwitch: RX=2, TX=4");

  loadCodesFromEEPROM();  // ✅ Teraz bezpečne volateľné
  
  Serial.printf("\nsizeof(CodeItem) = %d\n", sizeof(CodeItem));
  Serial.printf("Debug: codeCount = %d\n", codeCount);
  for (int i = 0; i < codeCount; i++) {
    Serial.printf("Kód[%d]: %ld (%d bitov), Meno: %s\n", i, savedCodes[i].code, savedCodes[i].bitLength, savedCodes[i].name);
  }
  printEEPROMStatus();

  // === HTTP HANDLERY ===
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/html", index_html);
  });

  server.on("/list", HTTP_GET, [](AsyncWebServerRequest *request){
    String json = "[";
    for (int i = 0; i < codeCount; i++) {
      String cleanName = "";
      bool valid = false;
      for (int j = 0; j < 32; j++) {
        char c = savedCodes[i].name[j];
        if (c == '\0') break;
        if (c >= 32 && c <= 126) {
          valid = true;
          if (c == '"') cleanName += "\\\"";
          else if (c == '\\') cleanName += "\\\\";
          else if (c == '\n') cleanName += "\\n";   // ✅ SPRÁVNE
          else if (c == '\r') cleanName += "\\r";   // ✅ SPRÁVNE
          else cleanName += c;
        }
      }
      if (!valid || cleanName.length() == 0) {
        cleanName = "Nezmenovaný";
      }
      json += "{\"name\":\"" + cleanName + "\",\"code\":" + String(savedCodes[i].code) + ",\"bitLength\":" + String(savedCodes[i].bitLength) + "}";
      if (i < codeCount - 1) json += ",";
    }
    json += "]";
    
    // ✅ KRITICKÉ: Zakázať cachovanie prehliadačom
    AsyncWebServerResponse *response = request->beginResponse(200, "application/json", json);
    response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    response->addHeader("Pragma", "no-cache");
    response->addHeader("Expires", "0");
    request->send(response);
  });

  server.on("/receive", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("name", true)) {
      pendingName = request->getParam("name", true)->value();
    } else {
      pendingName = "Nezmenovaný";
    }
    isReceiving = true;
    lastValidCode = -1;
    lastValidBitLength = 0;
    receiveStartTime = millis();
    request->send(200, "text/plain", "Prijímanie (3s)...");
  });

  server.on("/roll", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!request->hasParam("from", true) || !request->hasParam("to", true) || !request->hasParam("delay", true)) {
      request->send(400, "application/json", "{\"success\":false,\"message\":\"Chýbajúce parametre\"}");
      return;
    }
    long fromCode = request->getParam("from", true)->value().toInt();
    long toCode = request->getParam("to", true)->value().toInt();
    int delayMs = request->getParam("delay", true)->value().toInt();
    if (fromCode < 1 || toCode > 4294967295 || fromCode > toCode || delayMs < 50 || delayMs > 2000) {
      request->send(400, "application/json", "{\"success\":false,\"message\":\"Neplatný rozsah\"}");
      return;
    }
    rollingShouldStop = false;
    struct RollParams { long fromCode; long toCode; int delayMs; };
    RollParams *params = new RollParams{fromCode, toCode, delayMs};
    
    if (xTaskCreatePinnedToCore(
      [](void *param) {
        RollParams *p = (RollParams *)param;
        long fromCode = p->fromCode;
        long toCode = p->toCode;
        int delayMs = p->delayMs;
        delete p;
        
        unsigned long startTime = millis();
        int count = 0;
        int totalCount = toCode - fromCode + 1;
        
        String startMsg = "{\"type\":\"roll_start\",\"total\":" + String(totalCount) + "}";
        ws.textAll(startMsg);
        
        for (long code = fromCode; code <= toCode; code++) {
          if (rollingShouldStop) {
            String stopMsg = "{\"type\":\"roll_stopped\",\"count\":" + String(count) + "}";
            ws.textAll(stopMsg);
            break;
          }
          
          int bitLength = (code <= 16777215) ? 24 : 32;
          mySwitch.send(code, bitLength);
          count++;
          
          // ✅ POSLAŤ SPRÁVU OKAMŽITE pred oneskorením
          String progressMsg = "{\"type\":\"roll_progress\",\"current\":" + String(count) + 
                            ",\"total\":" + String(totalCount) + ",\"code\":" + String(code) + "}";
          ws.textAll(progressMsg);
          ws.cleanupClients();  // ✅ VYSKÚŠAJTE TOTO - vynúti odoslanie správy
          
          yield();
          
          if (delayMs > 0 && code < toCode) {
            unsigned long startDelay = millis();
            while (millis() - startDelay < (unsigned long)delayMs) {
              yield();
              if (rollingShouldStop) break;
            }
            if (rollingShouldStop) {
              String stopMsg = "{\"type\":\"roll_stopped\",\"count\":" + String(count) + "}";
              ws.textAll(stopMsg);
              break;
            }
          }
        }
        
        unsigned long duration = millis() - startTime;
        float seconds = duration / 1000.0;
        if (!rollingShouldStop) {
          String resultMsg = "{\"type\":\"roll_complete\",\"success\":true,\"count\":" + 
                           String(count) + ",\"duration\":" + String(seconds, 2) + "}";
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
    ) != pdPASS) {
      delete params;
      request->send(500, "application/json", "{\"success\":false,\"message\":\"Nedostatok pamäte\"}");
      return;
    }
    request->send(202, "application/json", "{\"success\":true,\"message\":\"Rolling Codes spustené\"}");
  });

  server.on("/rollStop", HTTP_POST, [](AsyncWebServerRequest *request){
    rollingShouldStop = true;
    request->send(200, "application/json", "{\"success\":true,\"message\":\"Zastavujem Rolling Codes\"}");
  });

  server.on("/transmit", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("code", true)) {
      long code = request->getParam("code", true)->value().toInt();
      if (code > 0 && code <= 4294967295) {
        int bitLength = getBitLengthForCode(code);
        mySwitch.send(code, bitLength);
        request->send(200, "text/plain", "Odoslané");
      } else {
        request->send(400, "text/plain", "Neplatný kód");
      }
    } else {
      request->send(400, "text/plain", "Chýba kód");
    }
  });

  server.on("/transmit3", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("code", true)) {
      long code = request->getParam("code", true)->value().toInt();
      if (code > 0 && code <= 4294967295) {
        int bitLength = getBitLengthForCode(code);
        for (int i = 0; i < 3; i++) {
          mySwitch.send(code, bitLength);
          delay(1000);
        }
        request->send(200, "text/plain", "Odoslané 3x");
      } else {
        request->send(400, "text/plain", "Neplatný kód");
      }
    } else {
      request->send(400, "text/plain", "Chýba kód");
    }
  });

  server.on("/saveManual", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("code", true) && request->hasParam("name", true)) {
      long code = request->getParam("code", true)->value().toInt();
      String name = request->getParam("name", true)->value();
      if (code > 0 && code <= 4294967295) {
        saveCodeToEEPROM(code, 24, name.c_str());
        request->send(200, "text/plain", "OK");
        printEEPROMStatus();
      } else {
        request->send(400, "text/plain", "Neplatný kód");
      }
    } else {
      request->send(400, "text/plain", "Missing parameters");
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
      request->send(400, "text/plain", "Chyba");
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

// === LOOP ===
void loop() {
  if (isReceiving && (millis() - receiveStartTime) < 3000) {
    if (mySwitch.available()) {
      long value = mySwitch.getReceivedValue();
      int bits = mySwitch.getReceivedBitlength();
      
      if (value > 0 && bits >= 1 && bits <= 32) {
        lastValidCode = value;
        lastValidBitLength = bits;
        Serial.printf("📡 Zachytený signál: %ld (%d bitov)\n", value, bits);
      }
      mySwitch.resetAvailable();
    }
  } else if (isReceiving) {
    isReceiving = false;
    if (lastValidCode != -1) {
      saveCodeToEEPROM(lastValidCode, lastValidBitLength, pendingName.c_str());
      Serial.printf("✅ Uložený: %ld (%d bitov, meno: %s)\n", lastValidCode, lastValidBitLength, pendingName.c_str());
      printEEPROMStatus();
      
      // ✅ WEBSOCKET SPRÁVA O ÚSPEŠNOM ULOŽENÍ
      String msg = "{\"type\":\"signal_received\",\"code\":" + String(lastValidCode) + 
                   ",\"name\":\"" + pendingName + "\",\"bitLength\":" + String(lastValidBitLength) + "}";
      ws.textAll(msg);
    } else {
      Serial.println("❌ Žiadny signál po 3 sekundách");
      // ✅ WEBSOCKET SPRÁVA O ČASOVOM LIMITE
      ws.textAll("{\"type\":\"signal_timeout\"}");
    }
    lastValidCode = -1;
    lastValidBitLength = 0;
  }
  delay(10);
}

// ✅ DEFINÍCIA FUNKCIE (po loop() pre čitateľnosť)
int getBitLengthForCode(long code) {
  for (int i = 0; i < codeCount; i++) {
    if (savedCodes[i].code == code) {
      return savedCodes[i].bitLength;
    }
  }
  return 24; // Default pre neznáme kódy
}
