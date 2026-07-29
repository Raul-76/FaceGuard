/**
     * FaceGuard Dashboard — app.js
     * ESP32 Facial Recognition Monitor
     *
     * ESP32 Endpoints:
     *   GET  http://{IP}/         -> Pagina padrao
     *   GET  http://{IP}/info     -> Status JSON da camera
     *   GET  http://{IP}/control?cmd=X&name=Y -> Controles
     *   GET  http://{IP}:81/stream -> MJPEG stream ao vivo
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
  statusPollInterval: null,
  lastEnrollStatus: 'idle',
  lastEnrollMsg: '-',
  facesList: [],
  lastScanning: false,
  pollFailCount: 0,
  enrollActive: false,
  reconnectInterval: null,
  enrollGuardTimer: null,
  pollInFlight: false
};
let lastAccessStateStr = "-";

function checkLogin() {
  const user = document.getElementById('loginUsername').value.trim();
  const pwd = document.getElementById('loginPassword').value;
  const errorMsg = document.getElementById('loginErrorMsg');
  if (user === 'admin' && pwd === 'admin123') {
    sessionStorage.setItem('faceGuardLoggedIn', 'true');
    errorMsg.style.display = 'none';
    document.getElementById('loginOverlay').style.opacity = '0';
    setTimeout(() => document.getElementById('loginOverlay').style.display = 'none', 500);
  } else {
    errorMsg.style.display = 'block';
    showToast('Invalid login or password', 'error');
  }
}

/* ===================== INIT ===================== */
window.addEventListener('DOMContentLoaded', () => {
  if (sessionStorage.getItem('faceGuardLoggedIn') !== 'true') {
    document.getElementById('loginOverlay').style.display = 'flex';
  } else {
    document.getElementById('loginOverlay').style.display = 'none';
  }

  const savedIp = state.esp32Ip;
  if (savedIp) {
    setTimeout(connectToESP, 100);
  }

  // Restore logs from localStorage
  recalcCounters();
  renderLog();
  updateCards();
});

/* ===================== CONNECTION ===================== */
function connectToESP() {
  const rawIp = state.esp32Ip;
  if (!rawIp) {
    showToast('IP not configured', 'warning');
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
  if (state.reconnectInterval) { clearInterval(state.reconnectInterval); state.reconnectInterval = null; }
  setConnectedUI(false);
  showToast('Disconnected from ESP32', 'info');
  addLogEntry('info', 'Session ended', `Disconnected from ${state.esp32Ip}`);
}

function startStream(ip) {
  // O feed de video do ESP32 roda na porta 81, separada da porta 80 (API)
  const baseIp = ip.split(':')[0]; // Remove qualquer porta caso o usuario tenha digitado
  const streamUrl = `http://${baseIp}:81/stream`;

  const img = document.getElementById('cameraStream');
  const placeholder = document.getElementById('cameraPlaceholder');
  const overlay = document.getElementById('cameraOverlay');

  img.onerror = () => {
    if (state.connected) {
      // Durante o cadastro o ESP fica preso na inferencia (detect/enroll) e
      // para de publicar frame por varios segundos. O navegador acha que o
      // stream morreu e dispara este onerror -> mas NAO e queda real, e so
      // o feed pausado. Recarregar rapido (1.5s) atropela o ESP ocupado e
      // pode derrubar o dashboard. Entao: durante o enroll, espera bem mais
      // antes de tentar recarregar, dando tempo do ESP terminar a captura.
      const espera = state.enrollActive ? 5000 : 8000;
      state.streamRetries = (state.streamRetries || 0) + 1;
      console.warn(`Stream pausou (retry ${state.streamRetries}), recarregando em ${espera}ms`);
      setTimeout(() => {
        // so recarrega se ainda estiver conectado (evita recarregar depois de sair)
        if (state.connected) img.src = streamUrl + '?t=' + Date.now();
      }, espera);
    } else {
      stopStream();
      setConnectedUI(false);
      showToast(`Could not connect to stream.\nCheck IP and if ESP32 is online.`, 'error');
    }
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

  // Verifica a conexao de status
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
    // Ignorado pois o img.onerror cuidara se o stream falhar
  }
}

function stopStream() {
  const img = document.getElementById('cameraStream');
  const placeholder = document.getElementById('cameraPlaceholder');
  const overlay = document.getElementById('cameraOverlay');

  img.onerror = null; // Previne loop infinito
  img.removeAttribute('src');
  img.style.display = 'none';
  overlay.classList.remove('scanning');
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
  if (state.pollInFlight) return;        // nunca empilha /info -> nao esgota sockets do ESP
  state.pollInFlight = true;
  try {
    const timeoutMs = state.enrollActive ? 15000 : 10000;
    const res = await fetch(`http://${ip}/info`, { signal: AbortSignal.timeout(timeoutMs) });
    if (!res.ok) throw new Error('Not OK');
    const text = await res.text();
    let data;
    try {
      data = JSON.parse(text);
    } catch (parseErr) {
      console.warn('JSON invalido, ignorando este poll', parseErr);
      return;
    }
    state.pollFailCount = 0;
    applyStatusToUI(data);
  } catch (e) {
    // MUDANCA CRITICA: falha de /info NAO derruba mais a conexao.
    // O stream MJPEG (porta 81) e a fonte de verdade. Um /info lento
    // porque o ESP esta ocupado no enroll nao pode matar o feed. Se o
    // ESP morrer DE VERDADE, o proprio <img> do stream dispara onerror
    // e tenta recarregar sozinho.
    state.pollFailCount = (state.pollFailCount || 0) + 1;
    console.warn(`pollStatus falhou (${state.pollFailCount}) - mantendo stream vivo`, e);
  } finally {
    state.pollInFlight = false;
  }
}

/* Perdeu a conexão: para tudo, mas NÃO desiste — começa a tentar voltar. */
function handleConnectionLost(ip) {
  clearStatusPoll();
  stopStream();
  setConnectedUI(false);
  showToast('Conexão perdida — tentando reconectar...', 'warning');
  addLogEntry('denied', 'Connection lost', `ESP32 at ${ip} indisponível`);
  startReconnectLoop(ip);
}

/* Fica batendo no /info a cada 3s; quando responder, re-arma o stream. */
function startReconnectLoop(ip) {
  if (state.reconnectInterval) return; // já tem um loop rodando
  state.reconnectInterval = setInterval(async () => {
    try {
      const res = await fetch(`http://${ip}/info`, { signal: AbortSignal.timeout(4000) });
      if (res.ok) {
        clearInterval(state.reconnectInterval);
        state.reconnectInterval = null;
        state.pollFailCount = 0;
        showToast('ESP32 de volta — reconectando...', 'success');
        startStream(ip); // religa stream + polling
      }
    } catch (_) {
      // ainda fora: segue tentando no próximo tick
    }
  }, 3000);
}

function applyStatusToUI(data) {
  document.getElementById('statSharp').textContent = data.sharp || '0';
  document.getElementById('statPeak').textContent = data.peak || '0';
  document.getElementById('statLDR').textContent = data.ldr || '0';

  state.isContinuous = (data.name !== "(pausado)");
  updateControlButtons();

  // ---- Barra de scan: liga/desliga com base no campo "scanning" ----
  // reportado pelo ESP no /info. Isso funciona tanto para o disparo
  // pela web (botao "Test Access") quanto pelo botao fisico (GPIO21),
  // porque em ambos os casos o ESP muda esse campo durante a tentativa.
  if (typeof data.scanning !== 'undefined') {
    const scanningNow = (data.scanning === true || data.scanning === 1 || data.scanning === "1" || data.scanning === "true");
    if (scanningNow !== state.lastScanning) {
      const overlay = document.getElementById('cameraOverlay');
      if (overlay) {
        if (scanningNow) overlay.classList.add('scanning');
        else overlay.classList.remove('scanning');
      }
      state.lastScanning = scanningNow;
    }
  }

  if (data.last_acc && data.last_acc !== lastAccessStateStr) {
    if (data.last_acc === 'granted') {
      registerAccessEvent(true, `Face: ${data.last_name}`);
    } else if (data.last_acc === 'denied') {
      registerAccessEvent(false, `Reason: ${data.last_name}`);
    }
    lastAccessStateStr = data.last_acc;
  }

  // Mantem o modo enroll (polling tolerante) enquanto o ESP estiver capturando.
  // CORRECAO: so encerra o guard na TRANSICAO para um estado final. Sem isto,
  // o "success" que sobra do cadastro ANTERIOR (o ESP nunca volta pra "idle")
  // chegava numa poll logo apos o confirmEnroll e chamava endEnrollGuard(),
  // desarmando a tolerancia no comeco do 2o cadastro -> desconexao.
  const enrollStatusChanged = (data.enroll_status !== state.lastEnrollStatus);
  if (data.enroll_status === 'capturing') {
    beginEnrollGuard();               // re-arma tolerancia enquanto captura
  } else if (enrollStatusChanged &&
    (data.enroll_status === 'success' ||
      data.enroll_status === 'failed' ||
      data.enroll_status === 'cancelled')) {
    endEnrollGuard();                 // so no MOMENTO em que terminou
  }
  // "success"/"idle" repetido (estado de repouso) NAO desarma mais o guard.
  // (se vier 'idle' ou vazio, respeita o flag otimista setado no confirmEnroll)

  // Handle enrollment status
  if (data.enroll_status && data.enroll_msg) {
    if (data.enroll_status === 'capturing') {
      document.getElementById('btnCancelEnroll').style.display = 'inline-flex';
      document.getElementById('btnOpenEnrollModal').disabled = true;
      document.getElementById('btnOpenEnrollModal').style.opacity = '0.5';
      if (data.enroll_msg !== state.lastEnrollMsg) {
        addLogEntry('info', 'Enrollment Progress', data.enroll_msg);
      }
    } else {
      document.getElementById('btnCancelEnroll').style.display = 'none';
      document.getElementById('btnOpenEnrollModal').disabled = false;
      document.getElementById('btnOpenEnrollModal').style.opacity = '1';
      if (data.enroll_status === 'success' && state.lastEnrollStatus !== 'success') {
        showToast(`Enrollment Successful: ${data.enroll_msg}`, 'success');
        addLogEntry('granted', 'Enrollment Completed', data.enroll_msg);
      } else if (data.enroll_status === 'failed' && state.lastEnrollStatus !== 'failed') {
        showToast(`Enrollment Failed: ${data.enroll_msg}`, 'error');
        addLogEntry('denied', 'Enrollment Failed', data.enroll_msg);
      } else if (data.enroll_status === 'cancelled' && state.lastEnrollStatus !== 'cancelled') {
        showToast(`Enrollment Cancelled`, 'warning');
        addLogEntry('info', 'Enrollment Cancelled', data.enroll_msg);
      }
    }
    state.lastEnrollStatus = data.enroll_status;
    state.lastEnrollMsg = data.enroll_msg;
  }
}



/* ===================== CAMERA CONTROLS ===================== */
async function sendControl(cmd, extraParams = '') {
  if (!state.connected) {
    showToast('Connect to ESP32 first!', 'warning');
    return false;
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
    // Liga a barra de scan otimisticamente; o poll do /info confirma
    // (ou corrige) o estado real em ate 3s, e desliga sozinha quando
    // a tentativa terminar (last_acc muda ou scanning volta a false).
    const overlay = document.getElementById('cameraOverlay');
    if (overlay) overlay.classList.add('scanning');
    state.lastScanning = true;
  }
}

async function remoteUnlock() {
  if (!confirm('Unlock the door remotely?')) return;
  const ok = await sendControl('o');
  if (ok) {
    showToast('🔓 Door unlocked remotely', 'success');
    addLogEntry('granted', 'Remote Unlock', 'Opened via dashboard');
  }
}

function openEnrollModal() {
  if (state.lastEnrollStatus === 'capturing') {
    showToast('Enrollment already in progress!', 'warning');
    return;
  }
  fetchFaces(false).then(() => {
    const modal = document.getElementById('enrollModalBackdrop');
    const input = document.getElementById('enrollNameInput');
    if (modal) {
      modal.style.display = 'flex';
      if (input) {
        input.value = '';
        input.focus();
      }
    }
  });
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
  if (!nameVal) {
    showToast('Name cannot be empty', 'warning');
    return;
  }
  if (state.facesList.includes(nameVal)) {
    showToast('Name already exists!', 'error');
    return;
  }
  const extraParams = `&name=${encodeURIComponent(nameVal)}`;

  closeEnrollModal();

  const ok = await sendControl('m', extraParams);
  if (ok) {
    // >>> CORRECAO PRINCIPAL <<<
    // Marca enroll como ATIVO agora mesmo, ANTES da primeira poll do /info.
    // Sem isto, a primeira ronda de status durante a captura (quando o ESP
    // esta mais ocupado e nao responde o /info) ainda usava o limite curto
    // (5s / 3 falhas) e derrubava o dashboard antes de descobrir que era um
    // cadastro em andamento.
    beginEnrollGuard();
    showToast(`📸 Multiple enrollment started! Look at the camera. (${nameVal})`, 'warning');
    addLogEntry('info', 'Enrollment started', `Waiting for face... (${nameVal})`);
  }
}

async function cancelEnroll() {
  const ok = await sendControl('x');
  if (ok) {
    showToast('Cancelling enrollment...', 'info');
  }
}

/* Liga o "modo enroll": polling tolerante (timeout 15s / 30 falhas).
   Zera falhas acumuladas e arma uma trava de seguranca de 120s, caso o ESP
   nunca reporte o fim do cadastro. */
function beginEnrollGuard() {
  state.enrollActive = true;
  state.pollFailCount = 0;
  if (state.enrollGuardTimer) clearTimeout(state.enrollGuardTimer);
  state.enrollGuardTimer = setTimeout(() => {
    state.enrollActive = false;
    state.enrollGuardTimer = null;
  }, 120000);
}

/* Desliga o "modo enroll" e cancela a trava de seguranca. */
function endEnrollGuard() {
  state.enrollActive = false;
  if (state.enrollGuardTimer) { clearTimeout(state.enrollGuardTimer); state.enrollGuardTimer = null; }
}

/* ===================== FACES MANAGEMENT ===================== */
async function fetchFaces(render = true) {
  if (!state.connected) return;
  if (state.enrollActive) return;
  try {
    const res = await fetch(`http://${state.esp32Ip}/faces`, { signal: AbortSignal.timeout(3000) });
    if (res.ok) {
      state.facesList = await res.json();
      if (render) renderFacesList();
    }
  } catch (e) {
    console.error('Error fetching faces:', e);
  }
}

function openFacesModal() {
  document.getElementById('facesModalBackdrop').style.display = 'flex';
  document.getElementById('facesListLoading').style.display = 'block';
  document.getElementById('facesListContainer').style.display = 'none';
  document.getElementById('btnDeleteSelectedFaces').style.display = 'none';
  fetchFaces();
}

function closeFacesModal() {
  document.getElementById('facesModalBackdrop').style.display = 'none';
}

function renderFacesList() {
  document.getElementById('facesListLoading').style.display = 'none';
  const container = document.getElementById('facesListContainer');
  container.style.display = 'block';
  if (state.facesList.length === 0) {
    container.innerHTML = '<div style="text-align:center; color:var(--text-muted); padding: 10px;">No faces registered.</div>';
    document.getElementById('btnDeleteSelectedFaces').style.display = 'none';
    return;
  }
  document.getElementById('btnDeleteSelectedFaces').style.display = 'inline-block';
  container.innerHTML = state.facesList.map((name, i) => `
        <div class="face-item">
          <input type="checkbox" class="face-checkbox" value="${escapeHtml(name)}" />
          <div class="face-item-name">
            <input type="text" id="faceName_${i}" value="${escapeHtml(name)}" onblur="renameFace('${escapeHtml(name)}', 'faceName_${i}')" onkeydown="if(event.key === 'Enter') this.blur()" />
          </div>
          <button class="btn-danger" style="padding: 4px 8px; font-size: 12px; border-radius: 4px;" onclick="deleteFace('${escapeHtml(name)}')">Delete</button>
        </div>
      `).join('');
}

async function deleteFace(name) {
  if (!confirm(`Delete face '${name}'?`)) return;
  document.getElementById('facesListLoading').style.display = 'block';
  document.getElementById('facesListContainer').style.display = 'none';
  await sendControl('k', `&name=${encodeURIComponent(name)}`);
  showToast(`Face '${name}' deleted`, 'success');
  setTimeout(fetchFaces, 500);
}

async function renameFace(oldName, inputId) {
  const newName = document.getElementById(inputId).value.trim();
  if (!newName || newName === oldName) {
    document.getElementById(inputId).value = oldName;
    return;
  }
  if (state.facesList.includes(newName)) {
    showToast('Name already exists!', 'error');
    document.getElementById(inputId).value = oldName;
    return;
  }
  document.getElementById('facesListLoading').style.display = 'block';
  document.getElementById('facesListContainer').style.display = 'none';
  await sendControl('e', `&name=${encodeURIComponent(oldName)}&newname=${encodeURIComponent(newName)}`);
  showToast(`Face renamed to '${newName}'`, 'success');
  setTimeout(fetchFaces, 500);
}

async function deleteSelectedFaces() {
  const checkboxes = document.querySelectorAll('.face-checkbox:checked');
  if (checkboxes.length === 0) return;
  if (!confirm(`Delete ${checkboxes.length} selected face(s)?`)) return;

  document.getElementById('facesListLoading').style.display = 'block';
  document.getElementById('facesListContainer').style.display = 'none';

  for (const cb of checkboxes) {
    await sendControl('k', `&name=${encodeURIComponent(cb.value)}`);
    await new Promise(r => setTimeout(r, 600));
  }
  showToast('Selected faces deleted', 'success');
  fetchFaces();
}

function updateControlButtons() {
  const btnC = document.getElementById('btnContinuous');
  if (btnC) btnC.className = 'ctrl-btn' + (state.isContinuous ? ' active' : '');
}

function toggleStats() {
  const bar = document.getElementById('statsBar');
  const btn = document.getElementById('btnToggleStats');
  if (!bar) return;
  const showing = bar.classList.toggle('visible');
  if (btn) btn.classList.toggle('active', showing);
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

