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
  logEntries: JSON.parse(localStorage.getItem('faceLogs') || '[]'),
  currentFilter: 'all',
  totalGranted: 0,
  totalDenied: 0,
  statusPollInterval: null
};
let lastAccessStateStr = "-";

/* ===================== INIT ===================== */
window.addEventListener('DOMContentLoaded', () => {
  const savedIp = state.esp32Ip;
  if (savedIp) {
    document.getElementById('espIpInput').value = savedIp;
    // Auto-conecta se o dashboard estiver sendo servido pelo próprio ESP32 (ou servidor local)
    if (window.location.protocol.startsWith('http') && window.location.host === savedIp) {
      setTimeout(connectToESP, 100);
    }
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
    showToast('Digite o IP do ESP32!', 'warning');
    document.getElementById('espIpInput').focus();
    return;
  }

  // Normalise: strip http:// prefix if user typed it
  const ip = rawIp.replace(/^https?:\/\//i, '').replace(/\/+$/, '');
  state.esp32Ip = ip;
  localStorage.setItem('esp32ip', ip);

  showToast('Conectando ao ESP32...', 'info');
  startStream(ip);
}

function disconnectFromESP() {
  stopStream();
  clearStatusPoll();
  setConnectedUI(false);
  showToast('Desconectado do ESP32', 'info');
  addLogEntry('info', 'Sessão encerrada', `Desconectado de ${state.esp32Ip}`);
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
    showToast(`Não foi possível conectar ao stream.\nVerifique o IP e se o ESP32 está online.`, 'error');
  };

  img.onload = () => {
    placeholder.style.display = 'none';
    img.style.display = 'block';
    overlay.style.display = 'block';
    setConnectedUI(true);
    showToast('Câmera conectada com sucesso!', 'success');
    startStatusPoll(ip);
    addLogEntry('info', 'Câmera conectada', `Stream iniciado em ${streamUrl}`);
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
  const btnConnect = document.getElementById('btnConnect');
  const btnDisconnect = document.getElementById('btnDisconnect');

  if (connected) {
    dot.className = 'status-dot connected';
    label.textContent = `Conectado — ${state.esp32Ip}`;
    liveBadge.className = 'live-badge live';
    liveBadge.innerHTML = '<span class="live-dot"></span> AO VIVO';
    btnConnect.style.display = 'none';
    btnDisconnect.style.display = 'flex';
  } else {
    dot.className = 'status-dot';
    label.textContent = 'Desconectado';
    liveBadge.className = 'live-badge';
    liveBadge.innerHTML = '<span class="live-dot"></span> OFFLINE';
    btnConnect.style.display = 'flex';
    btnDisconnect.style.display = 'none';
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
      showToast('Conexão com o ESP32 perdida!', 'error');
      addLogEntry('denied', 'Conexão perdida', `ESP32 em ${ip} ficou offline`);
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
      registerAccessEvent(true, `Rosto: ${data.last_name}`);
    } else if (data.last_acc === 'denied') {
      registerAccessEvent(false, `Motivo: ${data.last_name}`);
    }
    lastAccessStateStr = data.last_acc;
  }
}

/* ===================== CAMERA CONTROLS ===================== */
async function sendControl(cmd) {
  if (!state.connected) {
    showToast('Conecte ao ESP32 primeiro!', 'warning');
    return false;
  }
  try {
    const url = `http://${state.esp32Ip}/control?cmd=${cmd}`;
    const res = await fetch(url, { signal: AbortSignal.timeout(3000) });
    return res.ok;
  } catch (e) {
    showToast(`Erro ao enviar comando: ${cmd}`, 'error');
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
      state.isContinuous ? 'Modo Contínuo Ativado' : 'Inspeção Pausada',
      state.isContinuous ? 'success' : 'info'
    );
  }
}

async function testAccess() {
  const ok = await sendControl('t');
  if (ok) {
    showToast('Iniciando tentativa de acesso...', 'info');
  }
}

async function startEnroll() {
  const ok = await sendControl('c');
  if (ok) {
    showToast('📸 Cadastramento iniciado! Olhe para a câmera.', 'warning');
    addLogEntry('info', 'Cadastramento iniciado', 'Aguardando rosto');
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
  const label = granted ? 'Acesso Liberado' : 'Acesso Negado';
  const fullDetail = detail || (granted ? 'Rosto reconhecido com sucesso' : 'Rosto não encontrado no cadastro');

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
  if (state.logEntries.length === 0) { showToast('Registro já está vazio.', 'info'); return; }
  if (!confirm('Apagar todo o registro de acessos?')) return;
  state.logEntries = [];
  try { localStorage.removeItem('faceLogs'); } catch (e) { }
  recalcCounters();
  renderLog();
  updateCards();
  showToast('Registro apagado.', 'info');
}

function exportLog() {
  if (state.logEntries.length === 0) { showToast('Nenhum registro para exportar.', 'warning'); return; }
  const lines = [
    'Data/Hora,Tipo,Evento,Detalhe',
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
  showToast('Registro exportado como CSV!', 'success');
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
  return new Date().toLocaleTimeString('pt-BR', { hour: '2-digit', minute: '2-digit', second: '2-digit' });
}

function formatTimestamp(iso) {
  const d = new Date(iso);
  return d.toLocaleString('pt-BR', {
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
