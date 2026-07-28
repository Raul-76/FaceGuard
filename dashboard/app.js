/**
 * FaceGuard Dashboard — app.js
 * ESP32 Facial Recognition Monitor
 * 
 * ESP32 Endpoints:
 *   GET  http://{IP}/         → Página padrão
 *   GET  http://{IP}/status   → Status JSON da câmera
 *   GET  http://{IP}/control?var=face_detect&val=1  → Controles
 *   GET  http://{IP}:81/stream → MJPEG stream ao vivo
 *   GET  http://{IP}/capture   → Foto JPEG
 */

/* ===================== STATE ===================== */
const state = {
  esp32Ip: localStorage.getItem('esp32ip') || (window.location.protocol.startsWith('http') ? window.location.host : ''),
  connected: false,
  isContinuous: false,
  isSimulation: false,
  simVideoSource: 'canvas', // 'canvas' | 'webcam'
  simWebcamStream: null,
  simCanvasAnimId: null,
  simTelemetryInterval: null,
  logEntries: JSON.parse(localStorage.getItem('faceLogs') || '[]'),
  currentFilter: 'all',
  totalGranted: 0,
  totalDenied: 0,
  statusPollInterval: null,
  lastEnrollStatus: 'idle',
  lastEnrollMsg: '-'
};
let lastAccessStateStr = "-";

/* ===================== INIT ===================== */
window.addEventListener('DOMContentLoaded', () => {
  const savedIp = state.esp32Ip;
  if (savedIp) {
    document.getElementById('espIpInput').value = savedIp;
    setTimeout(connectToESP, 100);
  }

  // Restore logs from localStorage
  recalcCounters();
  renderLog();
  updateCards();
});

/* ===================== CONNECTION ===================== */
function connectToESP() {
  const rawIp = document.getElementById('espIpInput').value.trim();
  if (!rawIp) {
    showToast('Enter the ESP32 IP!', 'warning');
    document.getElementById('espIpInput').focus();
    return;
  }

  // Normalise: strip http:// prefix if user typed it
  const ip = rawIp.replace(/^https?:\/\//i, '').replace(/\/+$/, '');
  state.esp32Ip = ip;
  localStorage.setItem('esp32ip', ip);

  showToast('Connecting to ESP32...', 'info');
  startStream(ip);
}

function disconnectFromESP() {
  stopStream();
  clearStatusPoll();
  setConnectedUI(false);
  showToast('Disconnected from ESP32', 'info');
  addLogEntry('info', 'Session ended', `Disconnected from ${state.esp32Ip}`);
}

function startStream(ip) {
  // Se o usuário digitou uma porta no IP (ex: localhost:3000), usa ela. 
  // Senão, usa a rota padrão /stream
  const streamUrl = `http://${ip}/stream`;

  const img = document.getElementById('cameraStream');
  const placeholder = document.getElementById('cameraPlaceholder');
  const overlay = document.getElementById('cameraOverlay');

  img.onerror = () => {
    // Se falhar o stream de imagem pura, tenta reconectar
    stopStream();
    setConnectedUI(false);
    showToast(`Could not connect to stream.\nCheck IP and if ESP32 is online.`, 'error');
  };

  img.onload = () => {
    placeholder.style.display = 'none';
    img.style.display = 'block';
    overlay.style.display = 'block';
    setConnectedUI(true);
    showToast('Camera connected successfully!', 'success');
    startStatusPoll(ip);
    addLogEntry('info', 'Camera connected', `Stream started at ${streamUrl}`);
  };

  // Dispara o carregamento do stream
  img.src = streamUrl;

  // Verifica a conexão de status
  checkStatusAndConnect(ip, streamUrl);
}

async function checkStatusAndConnect(ip, streamUrl) {
  try {
    const res = await fetch(`http://${ip}/info`, { signal: AbortSignal.timeout(4000) });
    if (res.ok) {
      const status = await res.json();
      applyStatusToUI(status);
    }
  } catch (e) {
    // Ignorado pois o img.onerror cuidará se o stream falhar
  }
}

function stopStream() {
  const img = document.getElementById('cameraStream');
  const placeholder = document.getElementById('cameraPlaceholder');
  const overlay = document.getElementById('cameraOverlay');

  img.onerror = null; // Previne loop infinito
  img.removeAttribute('src');
  img.style.display = 'none';
  overlay.style.display = 'none';
  placeholder.style.display = 'flex';
}

function setConnectedUI(connected) {
  state.connected = connected;

  const dot = document.getElementById('statusDot');
  const label = document.getElementById('statusLabel');
  const liveBadge = document.getElementById('liveBadge');

  if (connected) {
    dot.className = 'status-dot connected';
    label.textContent = `Connected — ${state.esp32Ip}`;
    liveBadge.className = 'live-badge live';
    liveBadge.innerHTML = '<span class="live-dot"></span> LIVE';
  } else {
    dot.className = 'status-dot';
    label.textContent = 'Disconnected';
    liveBadge.className = 'live-badge';
    liveBadge.innerHTML = '<span class="live-dot"></span> OFFLINE';
    document.getElementById('statSharp').textContent = '--';
    document.getElementById('statPeak').textContent = '--';
    document.getElementById('statLDR').textContent = '--';
    state.isContinuous = false;
    updateControlButtons();
  }
}

/* ===================== STATUS POLLING ===================== */
function startStatusPoll(ip) {
  clearStatusPoll();
  state.statusPollInterval = setInterval(() => pollStatus(ip), 3000);
}

function clearStatusPoll() {
  if (state.statusPollInterval) {
    clearInterval(state.statusPollInterval);
    state.statusPollInterval = null;
  }
}

async function pollStatus(ip) {
  try {
    const res = await fetch(`http://${ip}/info`, { signal: AbortSignal.timeout(3000) });
    if (!res.ok) throw new Error('Not OK');
    const data = await res.json();
    applyStatusToUI(data);
  } catch (e) {
    console.error("Erro no pollStatus:", e);
    // If we lose connection
    if (state.connected) {
      stopStream();
      setConnectedUI(false);
      clearStatusPoll();
      showToast('Connection to ESP32 lost!', 'error');
      addLogEntry('denied', 'Connection lost', `ESP32 at ${ip} went offline`);
    }
  }
}

function applyStatusToUI(data) {
  document.getElementById('statSharp').textContent = data.sharp || '0';
  document.getElementById('statPeak').textContent = data.peak || '0';
  document.getElementById('statLDR').textContent = data.ldr || '0';

  state.isContinuous = (data.name !== "(pausado)");
  updateControlButtons();

  if (data.last_acc && data.last_acc !== lastAccessStateStr) {
    if (data.last_acc === 'granted') {
      registerAccessEvent(true, `Face: ${data.last_name}`);
    } else if (data.last_acc === 'denied') {
      registerAccessEvent(false, `Reason: ${data.last_name}`);
    }
    lastAccessStateStr = data.last_acc;
  }

  // Handle enrollment status
  if (data.enroll_status && data.enroll_msg) {
    if (data.enroll_status === 'capturing' && data.enroll_msg !== state.lastEnrollMsg) {
      addLogEntry('info', 'Enrollment Progress', data.enroll_msg);
    } else if (data.enroll_status === 'success' && state.lastEnrollStatus !== 'success') {
      showToast(`Enrollment Successful: ${data.enroll_msg}`, 'success');
      addLogEntry('granted', 'Enrollment Completed', data.enroll_msg);
    } else if (data.enroll_status === 'failed' && state.lastEnrollStatus !== 'failed') {
      showToast(`Enrollment Failed: ${data.enroll_msg}`, 'error');
      addLogEntry('denied', 'Enrollment Failed', data.enroll_msg);
    }
    state.lastEnrollStatus = data.enroll_status;
    state.lastEnrollMsg = data.enroll_msg;
  }
}

/* ===================== SIMULATION MODE ===================== */
function toggleSimulationMode() {
  if (state.isSimulation) {
    stopSimulation();
    showToast('Simulation Mode disabled', 'info');
  } else {
    if (state.connected) disconnectFromESP();
    startSimulation();
    showToast('Simulation Mode Enabled! Testing without ESP32.', 'success');
  }
}

function startSimulation() {
  state.isSimulation = true;
  state.connected = true;

  const btnSim = document.getElementById('btnSim');
  if (btnSim) btnSim.classList.add('active');

  const dot = document.getElementById('statusDot');
  const label = document.getElementById('statusLabel');
  const liveBadge = document.getElementById('liveBadge');
  const btnConnect = document.getElementById('btnConnect');
  const btnDisconnect = document.getElementById('btnDisconnect');
  const simPanel = document.getElementById('simPanel');
  const placeholder = document.getElementById('cameraPlaceholder');
  const overlay = document.getElementById('cameraOverlay');
  const simBadge = document.getElementById('simOverlayBadge');

  dot.className = 'status-dot connected';
  label.textContent = 'Connected (Offline Simulation)';
  liveBadge.className = 'live-badge live';
  liveBadge.innerHTML = '<span class="live-dot"></span> SIMULATION';
  btnConnect.style.display = 'none';
  btnDisconnect.style.display = 'flex';
  if (simPanel) simPanel.style.display = 'block';
  if (simBadge) simBadge.style.display = 'block';

  placeholder.style.display = 'none';
  overlay.style.display = 'block';

  startSimVideoSource();
  startSimTelemetry();

  addLogEntry('info', 'Simulation Mode Enabled', 'Offline test environment ready');
}

function stopSimulation() {
  state.isSimulation = false;
  state.connected = false;

  const btnSim = document.getElementById('btnSim');
  if (btnSim) btnSim.classList.remove('active');

  const simPanel = document.getElementById('simPanel');
  const simBadge = document.getElementById('simOverlayBadge');
  if (simPanel) simPanel.style.display = 'none';
  if (simBadge) simBadge.style.display = 'none';

  stopSimVideoSource();
  stopSimTelemetry();
  setConnectedUI(false);
}

function startSimTelemetry() {
  stopSimTelemetry();
  state.simTelemetryInterval = setInterval(() => {
    if (!state.isSimulation) return;
    const sharp = Math.floor(1200 + Math.random() * 400);
    const peak = Math.floor(1700 + Math.random() * 300);
    const ldr = Math.floor(600 + Math.random() * 250);

    document.getElementById('statSharp').textContent = sharp;
    document.getElementById('statPeak').textContent = peak;
    document.getElementById('statLDR').textContent = ldr;
  }, 1200);
}

function stopSimTelemetry() {
  if (state.simTelemetryInterval) {
    clearInterval(state.simTelemetryInterval);
    state.simTelemetryInterval = null;
  }
}

function startSimVideoSource() {
  stopSimVideoSource();
  const video = document.getElementById('webcamVideo');
  const canvas = document.getElementById('simCanvas');

  if (state.simVideoSource === 'webcam' && navigator.mediaDevices && navigator.mediaDevices.getUserMedia) {
    navigator.mediaDevices.getUserMedia({ video: { width: 640, height: 480 } })
      .then(stream => {
        state.simWebcamStream = stream;
        video.srcObject = stream;
        video.style.display = 'block';
        canvas.style.display = 'none';
        const lbl = document.getElementById('simSourceLabel');
        if (lbl) lbl.textContent = 'WebCam PC';
      })
      .catch(err => {
        console.warn('Webcam não disponível, usando scanner canvas:', err);
        state.simVideoSource = 'canvas';
        initSimCanvas();
      });
  } else {
    initSimCanvas();
  }
}

function stopSimVideoSource() {
  const video = document.getElementById('webcamVideo');
  const canvas = document.getElementById('simCanvas');

  if (state.simWebcamStream) {
    state.simWebcamStream.getTracks().forEach(track => track.stop());
    state.simWebcamStream = null;
  }
  if (video) video.style.display = 'none';
  if (canvas) canvas.style.display = 'none';

  if (state.simCanvasAnimId) {
    cancelAnimationFrame(state.simCanvasAnimId);
    state.simCanvasAnimId = null;
  }
}

function simToggleVideoSource() {
  if (!state.isSimulation) return;
  state.simVideoSource = (state.simVideoSource === 'canvas') ? 'webcam' : 'canvas';
  startSimVideoSource();
  showToast(`Video source: ${state.simVideoSource === 'webcam' ? 'PC WebCam' : 'Simulated Scanner'}`, 'info');
}

function initSimCanvas() {
  const canvas = document.getElementById('simCanvas');
  const video = document.getElementById('webcamVideo');
  if (!canvas || !video) return;

  video.style.display = 'none';
  canvas.style.display = 'block';
  const lbl = document.getElementById('simSourceLabel');
  if (lbl) lbl.textContent = 'Scanner Canvas';

  const ctx = canvas.getContext('2d');
  let angle = 0;

  function render() {
    if (!state.isSimulation || state.simVideoSource !== 'canvas') return;

    if (canvas.width !== canvas.clientWidth || canvas.height !== canvas.clientHeight) {
      canvas.width = canvas.clientWidth || 640;
      canvas.height = canvas.clientHeight || 480;
    }

    const w = canvas.width;
    const h = canvas.height;

    ctx.fillStyle = '#0b0f19';
    ctx.fillRect(0, 0, w, h);

    // Tech Grid
    ctx.strokeStyle = 'rgba(59, 130, 246, 0.08)';
    ctx.lineWidth = 1;
    const gridSize = 40;
    for (let x = 0; x < w; x += gridSize) {
      ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, h); ctx.stroke();
    }
    for (let y = 0; y < h; y += gridSize) {
      ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(w, y); ctx.stroke();
    }

    // Dynamic Face Target Center
    const cx = w / 2 + Math.sin(angle) * 15;
    const cy = h / 2 + Math.cos(angle * 0.7) * 10;
    const boxW = 180;
    const boxH = 220;

    // Face mesh oval
    ctx.strokeStyle = 'rgba(96, 165, 250, 0.4)';
    ctx.lineWidth = 1.5;
    ctx.setLineDash([4, 4]);
    ctx.beginPath();
    ctx.ellipse(cx, cy, boxW / 2.2, boxH / 2.2, 0, 0, Math.PI * 2);
    ctx.stroke();
    ctx.setLineDash([]);

    // Bounding Box
    ctx.strokeStyle = 'rgba(34, 197, 94, 0.8)';
    ctx.lineWidth = 2;
    ctx.strokeRect(cx - boxW / 2, cy - boxH / 2, boxW, boxH);

    // Corner brackets
    const cLen = 20;
    ctx.strokeStyle = '#22c55e';
    ctx.lineWidth = 3;

    ctx.beginPath();
    ctx.moveTo(cx - boxW / 2, cy - boxH / 2 + cLen);
    ctx.lineTo(cx - boxW / 2, cy - boxH / 2);
    ctx.lineTo(cx - boxW / 2 + cLen, cy - boxH / 2);
    ctx.stroke();

    ctx.beginPath();
    ctx.moveTo(cx + boxW / 2 - cLen, cy - boxH / 2);
    ctx.lineTo(cx + boxW / 2, cy - boxH / 2);
    ctx.lineTo(cx + boxW / 2, cy - boxH / 2 + cLen);
    ctx.stroke();

    ctx.beginPath();
    ctx.moveTo(cx - boxW / 2, cy + boxH / 2 - cLen);
    ctx.lineTo(cx - boxW / 2, cy + boxH / 2);
    ctx.lineTo(cx - boxW / 2 + cLen, cy + boxH / 2);
    ctx.stroke();

    ctx.beginPath();
    ctx.moveTo(cx + boxW / 2 - cLen, cy + boxH / 2);
    ctx.lineTo(cx + boxW / 2, cy + boxH / 2);
    ctx.lineTo(cx + boxW / 2, cy + boxH / 2 - cLen);
    ctx.stroke();

    // Facial landmark points
    ctx.fillStyle = '#60a5fa';
    const pts = [
      { x: cx - 35, y: cy - 25 },
      { x: cx + 35, y: cy - 25 },
      { x: cx, y: cy + 5 },
      { x: cx - 25, y: cy + 45 },
      { x: cx + 25, y: cy + 45 },
      { x: cx, y: cy + 50 }
    ];
    pts.forEach(p => {
      ctx.beginPath();
      ctx.arc(p.x, p.y, 3, 0, Math.PI * 2);
      ctx.fill();
    });

    ctx.fillStyle = '#22c55e';
    ctx.font = '12px "JetBrains Mono", monospace';
    ctx.fillText('FACE_DETECTED [99.2%]', cx - boxW / 2, cy - boxH / 2 - 8);

    angle += 0.03;
    state.simCanvasAnimId = requestAnimationFrame(render);
  }

  render();
}

function simTriggerEvent(granted, name) {
  if (!state.isSimulation && !state.connected) {
    showToast('Enable Simulation or Connect to ESP32 first!', 'warning');
    return;
  }
  if (granted) {
    registerAccessEvent(true, `Face: ${name}`);
  } else {
    registerAccessEvent(false, `Reason: ${name}`);
  }
}

/* ===================== CAMERA CONTROLS ===================== */
async function sendControl(cmd, extraParams = '') {
  if (!state.connected) {
    showToast('Connect to ESP32 first!', 'warning');
    return false;
  }
  if (state.isSimulation) {
    if (cmd === 't') {
      setTimeout(() => {
        const randGranted = Math.random() > 0.3;
        simTriggerEvent(randGranted, randGranted ? 'Simulated User' : 'Face Not Recognized');
      }, 800);
    } else if (cmd === 'm' || cmd === 'c') {
      const simulatedName = extraParams ? decodeURIComponent(extraParams.replace('&name=', '')) : 'New Simulated Face';
      setTimeout(() => {
        simTriggerEvent(true, `${simulatedName} Enrolled (Simulation)`);
      }, 1500);
    }
    return true;
  }
  try {
    const url = `http://${state.esp32Ip}/control?cmd=${cmd}${extraParams}`;
    const res = await fetch(url, { signal: AbortSignal.timeout(3000) });
    return res.ok;
  } catch (e) {
    showToast(`Error sending command: ${cmd}`, 'error');
    return false;
  }
}

async function toggleContinuous() {
  const cmd = state.isContinuous ? 'p' : 'r';
  const ok = await sendControl(cmd);
  if (ok) {
    state.isContinuous = !state.isContinuous;
    updateControlButtons();
    showToast(
      state.isContinuous ? 'Continuous Mode Enabled' : 'Inspection Paused',
      state.isContinuous ? 'success' : 'info'
    );
  }
}

async function testAccess() {
  const ok = await sendControl('t');
  if (ok) {
    showToast('Initiating access attempt...', 'info');
  }
}

function openEnrollModal() {
  const modal = document.getElementById('enrollModalBackdrop');
  const input = document.getElementById('enrollNameInput');
  if (modal) {
    modal.style.display = 'flex';
    if (input) {
      input.value = '';
      input.focus();
    }
  }
}

function closeEnrollModal() {
  const modal = document.getElementById('enrollModalBackdrop');
  if (modal) {
    modal.style.display = 'none';
  }
}

async function confirmEnroll() {
  const nameInput = document.getElementById('enrollNameInput');
  const nameVal = nameInput ? nameInput.value.trim() : '';
  const extraParams = nameVal ? `&name=${encodeURIComponent(nameVal)}` : '';
  
  closeEnrollModal();
  
  const ok = await sendControl('m', extraParams);
  if (ok) {
    const nameStr = nameVal ? ` (${nameVal})` : '';
    showToast(`📸 Multiple enrollment started! Look at the camera.${nameStr}`, 'warning');
    addLogEntry('info', 'Enrollment started', `Waiting for face...${nameStr}`);
  }
}

function updateControlButtons() {
  const btnC = document.getElementById('btnContinuous');
  if (btnC) btnC.className = 'ctrl-btn' + (state.isContinuous ? ' active' : '');
}



/* ===================== MANUAL LOG (for demonstration) ===================== */
// Since the ESP32 doesn't expose access events via HTTP by default,
// the dashboard provides manual log buttons for testing.

/**
 * Public function — can be called from browser console for testing:
 *   registerAccessEvent(true, "ID 1 - João")
 *   registerAccessEvent(false, "Rosto desconhecido")
 */
function registerAccessEvent(granted, detail = '') {
  const type = granted ? 'granted' : 'denied';
  const label = granted ? 'Access Granted' : 'Access Denied';
  const fullDetail = detail || (granted ? 'Face successfully recognized' : 'Face not found in database');

  // Adiciona ao log e exibe a notificação toast na tela
  addLogEntry(type, label, fullDetail);

  showToast(
    `${granted ? '✅' : '❌'} ${label}${detail ? ' — ' + detail : ''}`,
    granted ? 'success' : 'error'
  );
}

/* ===================== LOG MANAGEMENT ===================== */
function addLogEntry(type, label, detail = '') {
  const entry = {
    id: Date.now() + Math.random(),
    type,   // 'granted' | 'denied' | 'info'
    label,
    detail,
    timestamp: new Date().toISOString(),
  };
  state.logEntries.unshift(entry);

  // Cap at 500 entries
  if (state.logEntries.length > 500) state.logEntries.pop();

  // Persist
  try { localStorage.setItem('faceLogs', JSON.stringify(state.logEntries)); } catch (e) { }

  recalcCounters();
  renderLog();
  updateCards();
}

function recalcCounters() {
  state.totalGranted = state.logEntries.filter(e => e.type === 'granted').length;
  state.totalDenied = state.logEntries.filter(e => e.type === 'denied').length;
  const statTotalEl = document.getElementById('statTotal');
  if (statTotalEl) {
    statTotalEl.textContent = state.logEntries.length;
  }
}

function renderLog() {
  const list = document.getElementById('logList');
  const empty = document.getElementById('logEmpty');

  const filtered = state.currentFilter === 'all'
    ? state.logEntries
    : state.logEntries.filter(e => e.type === state.currentFilter);

  if (filtered.length === 0) {
    empty.style.display = 'flex';
    // Remove all entry divs
    list.querySelectorAll('.log-entry').forEach(el => el.remove());
    return;
  }

  empty.style.display = 'none';
  list.querySelectorAll('.log-entry').forEach(el => el.remove());

  filtered.forEach(entry => {
    const el = createLogElement(entry);
    list.appendChild(el);
  });
}

function createLogElement(entry) {
  const div = document.createElement('div');
  const isGranted = entry.type === 'granted';
  const isInfo = entry.type === 'info';

  div.className = `log-entry ${isGranted ? 'granted-entry' : isInfo ? '' : 'denied-entry'}`;
  div.dataset.type = entry.type;
  div.dataset.id = entry.id;

  const icon = isGranted
    ? `<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5"><polyline points="20 6 9 17 4 12"/></svg>`
    : isInfo
      ? `<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><line x1="12" y1="8" x2="12" y2="12"/><line x1="12" y1="16" x2="12.01" y2="16"/></svg>`
      : `<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5"><line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/></svg>`;

  const iconClass = isGranted ? 'granted' : isInfo ? '' : 'denied';

  div.innerHTML = `
    <div class="log-icon-wrap ${iconClass}" style="${isInfo ? 'background:rgba(59,130,246,0.1);border-color:rgba(59,130,246,0.2);color:#60a5fa;' : ''}">
      ${icon}
    </div>
    <div class="log-body">
      <div class="log-type">${escapeHtml(entry.label)}</div>
      ${entry.detail ? `<div class="log-detail">${escapeHtml(entry.detail)}</div>` : ''}
      <div class="log-time">${formatTimestamp(entry.timestamp)}</div>
    </div>
  `;

  return div;
}

function filterLog(filter, btn) {
  state.currentFilter = filter;
  document.querySelectorAll('.filter-btn').forEach(b => b.classList.remove('active'));
  btn.classList.add('active');
  renderLog();
}

function clearLog() {
  if (state.logEntries.length === 0) { showToast('Log is already empty.', 'info'); return; }
  if (!confirm('Clear all access logs?')) return;
  state.logEntries = [];
  try { localStorage.removeItem('faceLogs'); } catch (e) { }
  recalcCounters();
  renderLog();
  updateCards();
  showToast('Log cleared.', 'info');
}

function exportLog() {
  if (state.logEntries.length === 0) { showToast('No log to export.', 'warning'); return; }
  const lines = [
    'Date/Time,Type,Event,Detail',
    ...state.logEntries.map(e =>
      `"${formatTimestamp(e.timestamp)}","${e.type}","${e.label}","${e.detail || ''}"`
    )
  ];
  const blob = new Blob([lines.join('\n')], { type: 'text/csv;charset=utf-8;' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = `faceguard_log_${new Date().toISOString().slice(0, 10)}.csv`;
  a.click();
  URL.revokeObjectURL(url);
  showToast('Log exported as CSV!', 'success');
}

/* ===================== SUMMARY CARDS ===================== */
function updateCards() {
  const g = state.totalGranted;
  const d = state.totalDenied;
  const total = g + d || 1;

  document.getElementById('cardGranted').textContent = g;
  document.getElementById('cardDenied').textContent = d;
  document.getElementById('progressGranted').style.width = `${(g / total) * 100}%`;
  document.getElementById('progressDenied').style.width = `${(d / total) * 100}%`;
}

/* ===================== TOAST ===================== */
function showToast(message, type = 'info') {
  const container = document.getElementById('toastContainer');
  const toast = document.createElement('div');
  toast.className = `toast ${type}`;

  const icons = {
    success: `<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="20 6 9 17 4 12"/></svg>`,
    error: `<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><line x1="15" y1="9" x2="9" y2="15"/><line x1="9" y1="9" x2="15" y2="15"/></svg>`,
    warning: `<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M10.29 3.86L1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z"/><line x1="12" y1="9" x2="12" y2="13"/><line x1="12" y1="17" x2="12.01" y2="17"/></svg>`,
    info: `<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><line x1="12" y1="8" x2="12" y2="12"/><line x1="12" y1="16" x2="12.01" y2="16"/></svg>`,
  };

  toast.innerHTML = `
    <span class="toast-icon">${icons[type] || icons.info}</span>
    <span class="toast-msg">${escapeHtml(message)}</span>
  `;

  container.appendChild(toast);

  // Auto-remove
  setTimeout(() => {
    toast.style.transition = 'opacity 0.3s, transform 0.3s';
    toast.style.opacity = '0';
    toast.style.transform = 'translateX(30px)';
    setTimeout(() => toast.remove(), 300);
  }, 4000);
}

/* ===================== KEYBOARD SHORTCUTS ===================== */
document.addEventListener('keydown', (e) => {
  // Ctrl+Enter: connect/disconnect
  if (e.ctrlKey && e.key === 'Enter') {
    if (state.connected) disconnectFromESP();
    else connectToESP();
  }
});

/* ===================== HELPERS ===================== */
function getTimestamp() {
  return new Date().toLocaleTimeString('en-US', { hour: '2-digit', minute: '2-digit', second: '2-digit' });
}

function formatTimestamp(iso) {
  const d = new Date(iso);
  return d.toLocaleString('en-US', {
    day: '2-digit', month: '2-digit', year: 'numeric',
    hour: '2-digit', minute: '2-digit', second: '2-digit',
  });
}

function escapeHtml(str) {
  if (!str) return '';
  return String(str)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#039;');
}

/* ===================== EXPOSE FOR CONSOLE TESTING ===================== */
window.registerAccessEvent = registerAccessEvent;
window.addLogEntry = addLogEntry;
window.toggleSimulationMode = toggleSimulationMode;
window.simTriggerEvent = simTriggerEvent;
window.simToggleVideoSource = simToggleVideoSource;
