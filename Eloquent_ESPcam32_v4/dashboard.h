#ifndef DASHBOARD_H
#define DASHBOARD_H

#include <Arduino.h>

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <title>FaceGuard — Sistema de Reconhecimento Facial</title>
  <meta name="description" content="Dashboard de monitoramento em tempo real com câmera ao vivo e registro de acessos do sistema de reconhecimento facial ESP32." />
  <link rel="preconnect" href="https://fonts.googleapis.com" />
  <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin />
  <link href="https://fonts.googleapis.com/css2?family=Inter:wght@300;400;500;600;700;800&family=JetBrains+Mono:wght@400;500&display=swap" rel="stylesheet" />
  <style>
/* =====================================================
   FaceGuard Dashboard — style.css
   Modern dark theme with glassmorphism
   ===================================================== */

*, *::before, *::after {
  box-sizing: border-box;
  margin: 0;
  padding: 0;
}

:root {
  --bg-base:       #080c14;
  --bg-surface:    #0d1320;
  --bg-card:       #111827;
  --bg-hover:      #1a2236;
  --border:        rgba(255,255,255,0.07);
  --border-active: rgba(99,179,237,0.35);

  --accent:        #3b82f6;
  --accent-glow:   rgba(59,130,246,0.25);
  --accent-light:  #60a5fa;

  --green:         #22c55e;
  --green-glow:    rgba(34,197,94,0.2);
  --green-dim:     rgba(34,197,94,0.12);

  --red:           #ef4444;
  --red-glow:      rgba(239,68,68,0.2);
  --red-dim:       rgba(239,68,68,0.12);

  --yellow:        #f59e0b;
  --yellow-dim:    rgba(245,158,11,0.12);

  --text-primary:  #f1f5f9;
  --text-secondary:#94a3b8;
  --text-muted:    #475569;

  --font-sans:     'Inter', sans-serif;
  --font-mono:     'JetBrains Mono', monospace;

  --radius-sm:     6px;
  --radius-md:     12px;
  --radius-lg:     16px;
  --radius-xl:     20px;

  --shadow-sm:     0 2px 8px rgba(0,0,0,0.4);
  --shadow-md:     0 4px 24px rgba(0,0,0,0.5);
  --shadow-lg:     0 8px 48px rgba(0,0,0,0.6);

  --transition:    0.2s cubic-bezier(0.4,0,0.2,1);
}

html { scroll-behavior: smooth; }

body {
  font-family: var(--font-sans);
  background-color: var(--bg-base);
  color: var(--text-primary);
  background: var(--bg-base);
  background-image: 
    linear-gradient(rgba(255,255,255,0.01) 1px, transparent 1px),
    linear-gradient(90deg, rgba(255,255,255,0.01) 1px, transparent 1px);
  background-size: 50px 50px;
  background-position: center top;
  color: var(--text-primary);
  margin: 0;
  height: 100vh;
  overflow: hidden;
  display: flex;
  flex-direction: column;
}

/* Background grid pattern */
body::after {
  content: '';
  position: fixed;
  top: -30%;
  left: -20%;
  width: 60%;
  height: 60%;
  background: radial-gradient(ellipse, rgba(59,130,246,0.06) 0%, transparent 70%);
  pointer-events: none;
  z-index: 0;
}

/* ===================== HEADER ===================== */
.header {
  position: sticky;
  top: 0;
  z-index: 100;
  background: rgba(8,12,20,0.85);
  backdrop-filter: blur(20px);
  -webkit-backdrop-filter: blur(20px);
  border-bottom: 1px solid var(--border);
}

.header-inner {
  max-width: 1400px;
  margin: 0 auto;
  padding: 0 24px;
  height: 64px;
  display: flex;
  align-items: center;
  justify-content: space-between;
}

.logo {
  display: flex;
  align-items: center;
  gap: 10px;
}

.logo-icon {
  width: 36px;
  height: 36px;
  background: linear-gradient(135deg, var(--accent), #8b5cf6);
  border-radius: 10px;
  display: flex;
  align-items: center;
  justify-content: center;
  box-shadow: 0 0 20px var(--accent-glow);
}

.logo-text {
  font-size: 1.15rem;
  font-weight: 700;
  letter-spacing: -0.02em;
  color: var(--text-primary);
}

.logo-badge {
  font-size: 0.65rem;
  font-weight: 600;
  font-family: var(--font-mono);
  background: var(--accent-glow);
  color: var(--accent-light);
  border: 1px solid var(--border-active);
  padding: 2px 7px;
  border-radius: 4px;
  letter-spacing: 0.05em;
}

.header-status {
  display: flex;
  align-items: center;
  gap: 8px;
}

.status-dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: var(--text-muted);
  transition: var(--transition);
}
.status-dot.connected {
  background: var(--green);
  box-shadow: 0 0 10px var(--green);
  animation: pulse 2s infinite;
}
.status-dot.error { background: var(--red); box-shadow: 0 0 10px var(--red); }

@keyframes pulse {
  0%, 100% { opacity: 1; }
  50% { opacity: 0.5; }
}

.status-label {
  font-size: 0.8rem;
  font-weight: 500;
  color: var(--text-secondary);
}

/* ===================== CONFIG BAR ===================== */
.config-bar {
  position: relative;
  z-index: 1;
  background: rgba(13,19,32,0.8);
  border-bottom: 1px solid var(--border);
  padding: 16px 24px;
}

.config-inner {
  max-width: 1400px;
  margin: 0 auto;
  display: flex;
  align-items: flex-end;
  gap: 12px;
  flex-wrap: wrap;
}

.config-field {
  display: flex;
  flex-direction: column;
  gap: 6px;
  flex: 1;
  min-width: 240px;
  max-width: 400px;
}

.config-field label {
  font-size: 0.72rem;
  font-weight: 600;
  text-transform: uppercase;
  letter-spacing: 0.08em;
  color: var(--text-muted);
  display: flex;
  align-items: center;
  gap: 5px;
}

.input-group {
  display: flex;
  align-items: center;
  background: var(--bg-card);
  border: 1px solid var(--border);
  border-radius: var(--radius-sm);
  overflow: hidden;
  transition: var(--transition);
}
.input-group:focus-within {
  border-color: var(--accent);
  box-shadow: 0 0 0 3px var(--accent-glow);
}

.input-prefix {
  padding: 0 10px;
  font-family: var(--font-mono);
  font-size: 0.78rem;
  color: var(--text-muted);
  background: rgba(255,255,255,0.03);
  border-right: 1px solid var(--border);
  height: 38px;
  display: flex;
  align-items: center;
  white-space: nowrap;
}

.input-group input {
  flex: 1;
  background: transparent;
  border: none;
  outline: none;
  color: var(--text-primary);
  font-family: var(--font-mono);
  font-size: 0.85rem;
  padding: 0 12px;
  height: 38px;
}
.input-group input::placeholder { color: var(--text-muted); }

/* ===================== BUTTONS ===================== */
.btn-connect, .btn-disconnect {
  display: inline-flex;
  align-items: center;
  gap: 7px;
  padding: 0 18px;
  height: 38px;
  border: none;
  border-radius: var(--radius-sm);
  font-family: var(--font-sans);
  font-size: 0.83rem;
  font-weight: 600;
  cursor: pointer;
  transition: var(--transition);
  white-space: nowrap;
  text-decoration: none;
}

.btn-connect {
  background: linear-gradient(135deg, var(--accent), #6366f1);
  color: #fff;
  box-shadow: 0 4px 15px var(--accent-glow);
}
.btn-connect:hover {
  transform: translateY(-1px);
  box-shadow: 0 6px 20px rgba(59,130,246,0.4);
}
.btn-connect:active { transform: translateY(0); }

.btn-disconnect {
  background: var(--bg-card);
  color: var(--text-secondary);
  border: 1px solid var(--border);
}
.btn-disconnect:hover {
  background: var(--bg-hover);
  color: var(--red);
  border-color: var(--red);
}

.icon-btn {
  width: 30px;
  height: 30px;
  display: flex;
  align-items: center;
  justify-content: center;
  background: transparent;
  border: 1px solid var(--border);
  border-radius: var(--radius-sm);
  color: var(--text-muted);
  cursor: pointer;
  transition: var(--transition);
}
.icon-btn:hover {
  background: var(--bg-hover);
  color: var(--text-primary);
  border-color: rgba(255,255,255,0.15);
}

/* ===================== MAIN / GRID ===================== */
.main {
  position: relative;
  z-index: 1;
  width: 100%;
  max-width: 1400px;
  margin: 0 auto;
  padding: 24px;
  flex: 1;
  display: flex;
  flex-direction: column;
  min-height: 0;
}

.grid {
  flex: 1;
  display: grid;
  grid-template-columns: 1fr 400px;
  gap: 20px;
  min-height: 0;
  align-items: stretch;
}

@media (max-width: 1100px) {
  .grid { grid-template-columns: 1fr; }
}

/* ===================== PANEL ===================== */
.panel {
  background: var(--bg-card);
  border: 1px solid var(--border);
  border-radius: var(--radius-lg);
  overflow: hidden;
  box-shadow: var(--shadow-sm);
  display: flex;
  flex-direction: column;
  min-height: 0;
}

.panel-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 16px 20px;
  border-bottom: 1px solid var(--border);
  background: rgba(255,255,255,0.02);
}

.panel-title {
  display: flex;
  align-items: center;
  gap: 10px;
  font-size: 0.9rem;
  font-weight: 600;
  color: var(--text-primary);
}

.panel-icon {
  width: 30px;
  height: 30px;
  border-radius: var(--radius-sm);
  display: flex;
  align-items: center;
  justify-content: center;
}

.camera-icon {
  background: linear-gradient(135deg, rgba(59,130,246,0.3), rgba(99,102,241,0.3));
  border: 1px solid rgba(59,130,246,0.2);
  color: var(--accent-light);
}

.log-icon {
  background: linear-gradient(135deg, rgba(245,158,11,0.2), rgba(251,191,36,0.1));
  border: 1px solid rgba(245,158,11,0.2);
  color: var(--yellow);
}

.panel-actions {
  display: flex;
  gap: 6px;
}

/* ===================== LIVE BADGE ===================== */
.live-badge {
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 4px 10px;
  border-radius: 100px;
  font-size: 0.7rem;
  font-weight: 700;
  font-family: var(--font-mono);
  letter-spacing: 0.08em;
  background: var(--bg-hover);
  border: 1px solid var(--border);
  color: var(--text-muted);
  transition: var(--transition);
}
.live-badge.live {
  background: var(--green-dim);
  border-color: rgba(34,197,94,0.3);
  color: var(--green);
}

.live-dot {
  width: 6px;
  height: 6px;
  border-radius: 50%;
  background: currentColor;
}
.live-badge.live .live-dot {
  animation: pulse 1.5s infinite;
}

/* ===================== CAMERA ===================== */
.camera-container {
  position: relative;
  background: #000;
  flex: 1;
  min-height: 0;
  display: flex;
  align-items: center;
  justify-content: center;
  overflow: hidden;
}

.camera-placeholder {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 10px;
  padding: 40px;
  text-align: center;
}

.placeholder-icon {
  color: var(--text-muted);
  opacity: 0.4;
  margin-bottom: 8px;
}

.placeholder-text {
  font-size: 0.9rem;
  color: var(--text-secondary);
}

.placeholder-sub {
  font-size: 0.78rem;
  color: var(--text-muted);
}

#cameraStream {
  width: 100%;
  height: 100%;
  object-fit: contain;
  display: block;
}

/* Camera overlay corners */
.camera-overlay {
  position: absolute;
  inset: 0;
  pointer-events: none;
}

.scan-line {
  position: absolute;
  left: 0; right: 0;
  height: 2px;
  background: linear-gradient(90deg, transparent, var(--accent), transparent);
  animation: scan 3s linear infinite;
  opacity: 0.5;
}

@keyframes scan {
  0%   { top: 0%; opacity: 0; }
  5%   { opacity: 0.5; }
  95%  { opacity: 0.5; }
  100% { top: 100%; opacity: 0; }
}

.corner {
  position: absolute;
  width: 20px;
  height: 20px;
  border-color: var(--accent);
  border-style: solid;
  opacity: 0.7;
}
.corner.tl { top: 12px; left: 12px; border-width: 2px 0 0 2px; }
.corner.tr { top: 12px; right: 12px; border-width: 2px 2px 0 0; }
.corner.bl { bottom: 12px; left: 12px; border-width: 0 0 2px 2px; }
.corner.br { bottom: 12px; right: 12px; border-width: 0 2px 2px 0; }

/* ===================== CONTROLS ===================== */
.controls-grid {
  display: grid;
  grid-template-columns: repeat(4, 1fr);
  gap: 8px;
  padding: 14px 16px;
  border-bottom: 1px solid var(--border);
}

@media (max-width: 700px) {
  .controls-grid { grid-template-columns: repeat(2, 1fr); }
}

.ctrl-btn {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 5px;
  padding: 10px 8px;
  background: var(--bg-surface);
  border: 1px solid var(--border);
  border-radius: var(--radius-sm);
  color: var(--text-secondary);
  font-family: var(--font-sans);
  font-size: 0.72rem;
  font-weight: 500;
  cursor: pointer;
  transition: var(--transition);
}
.ctrl-btn:hover {
  background: var(--bg-hover);
  color: var(--text-primary);
  border-color: rgba(255,255,255,0.15);
  transform: translateY(-1px);
}
.ctrl-btn.active {
  background: var(--accent-glow);
  border-color: var(--accent);
  color: var(--accent-light);
}
.ctrl-btn.active-green {
  background: var(--green-dim);
  border-color: rgba(34,197,94,0.4);
  color: var(--green);
}
.enroll-btn.enrolling {
  background: var(--yellow-dim);
  border-color: rgba(245,158,11,0.4);
  color: var(--yellow);
  animation: pulse 1s infinite;
}

/* ===================== STATS BAR ===================== */
.stats-bar {
  display: flex;
  align-items: center;
  padding: 12px 20px;
  gap: 0;
}

.stat-item {
  flex: 1;
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 2px;
}

.stat-val {
  font-family: var(--font-mono);
  font-size: 1.1rem;
  font-weight: 600;
  color: var(--text-primary);
}

.stat-lbl {
  font-size: 0.68rem;
  font-weight: 500;
  text-transform: uppercase;
  letter-spacing: 0.06em;
  color: var(--text-muted);
}

.stat-divider {
  width: 1px;
  height: 32px;
  background: var(--border);
}

/* ===================== LOG PANEL ===================== */
.right-column {
  display: flex;
  flex-direction: column;
  gap: 16px;
  min-height: 0;
}

.log-filters {
  display: flex;
  gap: 6px;
  padding: 12px 16px;
  border-bottom: 1px solid var(--border);
}

.filter-btn {
  display: flex;
  align-items: center;
  gap: 5px;
  padding: 4px 12px;
  background: transparent;
  border: 1px solid var(--border);
  border-radius: 100px;
  color: var(--text-muted);
  font-family: var(--font-sans);
  font-size: 0.75rem;
  font-weight: 500;
  cursor: pointer;
  transition: var(--transition);
}
.filter-btn:hover {
  background: var(--bg-hover);
  color: var(--text-secondary);
}
.filter-btn.active {
  background: var(--accent-glow);
  border-color: var(--accent);
  color: var(--accent-light);
}

.filter-dot {
  width: 6px;
  height: 6px;
  border-radius: 50%;
}
.filter-dot.granted { background: var(--green); }
.filter-dot.denied  { background: var(--red); }

/* ===================== LOG LIST ===================== */
.log-list {
  flex: 1;
  min-height: 0;
  overflow-y: auto;
  padding: 10px 12px;
  scrollbar-width: thin;
  scrollbar-color: var(--border) transparent;
}

.log-list::-webkit-scrollbar { width: 4px; }
.log-list::-webkit-scrollbar-track { background: transparent; }
.log-list::-webkit-scrollbar-thumb { background: var(--border); border-radius: 4px; }

.log-empty {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  height: 100%;
  gap: 10px;
  color: var(--text-muted);
  text-align: center;
}
.log-empty p { font-size: 0.85rem; font-weight: 500; }
.log-empty span { font-size: 0.75rem; color: var(--text-muted); opacity: 0.6; }

/* LOG ENTRY */
.log-entry {
  display: flex;
  align-items: flex-start;
  gap: 10px;
  padding: 10px 10px;
  border-radius: var(--radius-sm);
  margin-bottom: 6px;
  border: 1px solid transparent;
  transition: var(--transition);
  animation: slideIn 0.3s ease;
}
.log-entry:hover {
  background: var(--bg-hover);
  border-color: var(--border);
}
.log-entry:last-child { margin-bottom: 0; }

@keyframes slideIn {
  from { opacity: 0; transform: translateX(12px); }
  to   { opacity: 1; transform: translateX(0); }
}

.log-icon-wrap {
  width: 32px;
  height: 32px;
  border-radius: 50%;
  flex-shrink: 0;
  display: flex;
  align-items: center;
  justify-content: center;
  margin-top: 2px;
}
.log-icon-wrap.granted {
  background: var(--green-dim);
  border: 1px solid rgba(34,197,94,0.3);
  color: var(--green);
}
.log-icon-wrap.denied {
  background: var(--red-dim);
  border: 1px solid rgba(239,68,68,0.3);
  color: var(--red);
}

.log-body { flex: 1; min-width: 0; }

.log-type {
  font-size: 0.78rem;
  font-weight: 700;
  text-transform: uppercase;
  letter-spacing: 0.05em;
  margin-bottom: 2px;
}
.log-entry.granted-entry .log-type { color: var(--green); }
.log-entry.denied-entry  .log-type { color: var(--red); }

.log-detail {
  font-size: 0.8rem;
  color: var(--text-secondary);
  margin-bottom: 3px;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}

.log-time {
  font-family: var(--font-mono);
  font-size: 0.68rem;
  color: var(--text-muted);
}

/* ===================== SUMMARY CARDS ===================== */
.cards-row {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 12px;
}

.summary-card {
  background: var(--bg-card);
  border: 1px solid var(--border);
  border-radius: var(--radius-md);
  padding: 16px;
  display: flex;
  align-items: center;
  gap: 12px;
  position: relative;
  overflow: hidden;
  transition: var(--transition);
}
.summary-card:hover { border-color: rgba(255,255,255,0.12); }

.granted-card { border-left: 3px solid var(--green); }
.denied-card  { border-left: 3px solid var(--red); }

.card-icon {
  width: 40px;
  height: 40px;
  border-radius: var(--radius-sm);
  display: flex;
  align-items: center;
  justify-content: center;
  flex-shrink: 0;
}
.granted-card .card-icon {
  background: var(--green-dim);
  color: var(--green);
}
.denied-card .card-icon {
  background: var(--red-dim);
  color: var(--red);
}

.card-info {
  flex: 1;
}

.card-num {
  display: block;
  font-size: 1.6rem;
  font-weight: 800;
  font-family: var(--font-mono);
  line-height: 1;
  margin-bottom: 3px;
}
.granted-card .card-num { color: var(--green); }
.denied-card  .card-num { color: var(--red); }

.card-label {
  font-size: 0.72rem;
  font-weight: 500;
  color: var(--text-muted);
  text-transform: uppercase;
  letter-spacing: 0.06em;
}

.card-progress {
  position: absolute;
  bottom: 0; left: 0; right: 0;
  height: 3px;
  background: var(--border);
}
.progress-bar {
  height: 100%;
  transition: width 0.6s cubic-bezier(0.4,0,0.2,1);
}
.granted-bar { background: linear-gradient(90deg, var(--green), #4ade80); }
.denied-bar  { background: linear-gradient(90deg, var(--red), #f87171); }

/* ===================== TOAST ===================== */
.toast-container {
  position: fixed;
  bottom: 24px;
  right: 24px;
  display: flex;
  flex-direction: column;
  gap: 10px;
  z-index: 9999;
}

.toast {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 12px 16px;
  background: var(--bg-card);
  border: 1px solid var(--border);
  border-radius: var(--radius-md);
  box-shadow: var(--shadow-lg);
  font-size: 0.83rem;
  font-weight: 500;
  min-width: 280px;
  max-width: 380px;
  animation: toastIn 0.35s cubic-bezier(0.34,1.56,0.64,1);
  position: relative;
  overflow: hidden;
}
.toast::before {
  content: '';
  position: absolute;
  left: 0; top: 0; bottom: 0;
  width: 3px;
}
.toast.success { color: var(--green); border-color: rgba(34,197,94,0.25); }
.toast.success::before { background: var(--green); }
.toast.error   { color: var(--red);   border-color: rgba(239,68,68,0.25); }
.toast.error::before { background: var(--red); }
.toast.info    { color: var(--accent-light); border-color: var(--border-active); }
.toast.info::before { background: var(--accent); }
.toast.warning { color: var(--yellow); border-color: rgba(245,158,11,0.25); }
.toast.warning::before { background: var(--yellow); }

@keyframes toastIn {
  from { opacity: 0; transform: translateX(30px) scale(0.9); }
  to   { opacity: 1; transform: translateX(0) scale(1); }
}

.toast-icon { flex-shrink: 0; }
.toast-msg  { flex: 1; color: var(--text-primary); }

/* ===================== MODAL ===================== */
.modal-overlay {
  position: fixed;
  inset: 0;
  background: rgba(0,0,0,0.75);
  backdrop-filter: blur(8px);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 9000;
  animation: fadeIn 0.2s ease;
}

@keyframes fadeIn {
  from { opacity: 0; }
  to   { opacity: 1; }
}

.modal-content {
  background: var(--bg-card);
  border: 1px solid var(--border);
  border-radius: var(--radius-xl);
  overflow: hidden;
  max-width: 640px;
  width: 90%;
  box-shadow: var(--shadow-lg);
  animation: modalIn 0.3s cubic-bezier(0.34,1.2,0.64,1);
}

@keyframes modalIn {
  from { opacity: 0; transform: scale(0.9) translateY(20px); }
  to   { opacity: 1; transform: scale(1) translateY(0); }
}

.modal-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 16px 20px;
  border-bottom: 1px solid var(--border);
}

.modal-header h3 {
  font-size: 0.95rem;
  font-weight: 600;
}

.modal-close {
  background: transparent;
  border: none;
  color: var(--text-muted);
  font-size: 1.4rem;
  cursor: pointer;
  line-height: 1;
  padding: 0 4px;
  transition: var(--transition);
}
.modal-close:hover { color: var(--text-primary); }

.modal-content img {
  width: 100%;
  display: block;
  max-height: 400px;
  object-fit: contain;
  background: #000;
}

.modal-footer {
  display: flex;
  gap: 10px;
  padding: 16px 20px;
  border-top: 1px solid var(--border);
  justify-content: flex-end;
}

/* ===================== RESPONSIVE ===================== */
@media (max-width: 768px) {
  .header-inner { padding: 0 16px; }
  .config-bar { padding: 12px 16px; }
  .main { padding: 16px 16px 40px; }
  .cards-row { grid-template-columns: 1fr; }
  .config-field { max-width: 100%; }
}



/* ===================== MODAL ===================== */
.modal-backdrop {
  position: fixed;
  inset: 0;
  background: rgba(0,0,0,0.6);
  backdrop-filter: blur(4px);
  -webkit-backdrop-filter: blur(4px);
  z-index: 1000;
  display: flex;
  align-items: center;
  justify-content: center;
}

.modal-box {
  background: var(--bg-card);
  border: 1px solid var(--border);
  border-radius: var(--radius-md);
  width: 90%;
  max-width: 400px;
  box-shadow: var(--shadow-lg);
  animation: modalFadeIn 0.2s ease-out;
}

@keyframes modalFadeIn {
  from { opacity: 0; transform: translateY(10px) scale(0.98); }
  to { opacity: 1; transform: translateY(0) scale(1); }
}

.modal-header {
  padding: 16px 20px;
  border-bottom: 1px solid var(--border);
  display: flex;
  align-items: center;
  justify-content: space-between;
}
.modal-header h3 {
  font-size: 1rem;
  font-weight: 600;
  color: var(--text-primary);
}
.modal-close {
  background: transparent;
  border: none;
  color: var(--text-muted);
  cursor: pointer;
  display: flex;
  transition: var(--transition);
}
.modal-close:hover { color: var(--red); }

.modal-body {
  padding: 20px;
  display: flex;
  flex-direction: column;
  gap: 8px;
}
.modal-body label {
  font-size: 0.8rem;
  font-weight: 500;
  color: var(--text-secondary);
}
.modal-body input {
  background: var(--bg-surface);
  border: 1px solid var(--border);
  border-radius: var(--radius-sm);
  color: var(--text-primary);
  font-family: var(--font-sans);
  font-size: 0.9rem;
  padding: 10px 12px;
  outline: none;
  transition: var(--transition);
}
.modal-body input:focus {
  border-color: var(--accent);
  box-shadow: 0 0 0 3px var(--accent-glow);
}

.modal-footer {
  padding: 16px 20px;
  border-top: 1px solid var(--border);
  display: flex;
  justify-content: flex-end;
  gap: 10px;
  background: rgba(0,0,0,0.2);
  border-radius: 0 0 var(--radius-md) var(--radius-md);
}

.btn-secondary, .btn-primary {
  padding: 8px 16px;
  border-radius: var(--radius-sm);
  font-size: 0.85rem;
  font-weight: 600;
  cursor: pointer;
  transition: var(--transition);
  border: none;
}
.btn-secondary {
  background: transparent;
  color: var(--text-secondary);
  border: 1px solid var(--border);
}
.btn-secondary:hover {
  background: var(--bg-hover);
  color: var(--text-primary);
}
.btn-primary {
  background: var(--accent);
  color: #fff;
}
.btn-primary:hover {
  background: var(--accent-light);
}

</style>
</head>
<body>

  <!-- ===== HEADER ===== -->
  <header class="header" id="header">
    <div class="header-inner">
      <div class="logo">
        <div class="logo-icon">
          <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
            <path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"/>
            <circle cx="12" cy="12" r="3"/>
          </svg>
        </div>
        <span class="logo-text">FaceGuard</span>
        <span class="logo-badge">ESP32</span>
      </div>
      <div class="header-status">
        <div class="status-dot" id="statusDot"></div>
        <span class="status-label" id="statusLabel">Desconectado</span>
      </div>
    </div>
  </header>

  <!-- ===== MAIN ===== -->
  <main class="main">

    <!-- CONFIG BAR -->
    <section class="config-bar" id="configBar">
      <div class="config-inner">
        <div class="config-field">
          <label for="espIpInput">
            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><polyline points="12 6 12 12 16 14"/></svg>
            Endereço IP do ESP32
          </label>
          <div class="input-group" style="opacity: 0.7; cursor: not-allowed;">
            <span class="input-prefix">http://</span>
            <input type="text" id="espIpInput" placeholder="192.168.1.xxx" value="" readonly autocomplete="off" spellcheck="false" style="cursor: not-allowed;" />
          </div>
        </div>

      </div>
    </section>

    <!-- GRID LAYOUT -->
    <div class="grid">

      <!-- ===== LEFT: CAMERA PANEL ===== -->
      <div class="panel camera-panel">
        <div class="panel-header">
          <div class="panel-title">
            <div class="panel-icon camera-icon">
              <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M23 7l-7 5 7 5V7z"/><rect x="1" y="5" width="15" height="14" rx="2" ry="2"/></svg>
            </div>
            Câmera ao Vivo
          </div>
          <div class="live-badge" id="liveBadge">
            <span class="live-dot"></span> OFFLINE
          </div>
        </div>

        <div class="camera-container" id="cameraContainer">
          <div class="camera-placeholder" id="cameraPlaceholder">
            <div class="placeholder-icon">
              <svg width="64" height="64" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1"><path d="M23 7l-7 5 7 5V7z"/><rect x="1" y="5" width="15" height="14" rx="2" ry="2"/></svg>
            </div>
            <p class="placeholder-text">Configure o IP do ESP32 para testar a interface</p>
          </div>
          <img id="cameraStream" src="" alt="Stream da câmera ESP32" style="display:none;" />

          
          <div class="camera-overlay" id="cameraOverlay" style="display:none;">
            <div class="scan-line"></div>
            <div class="corner tl"></div>
            <div class="corner tr"></div>
            <div class="corner bl"></div>
            <div class="corner br"></div>

          </div>
        </div>

        <!-- Camera Controls -->
        <div class="controls-grid" id="controlsGrid">
          <button class="ctrl-btn" id="btnContinuous" onclick="toggleContinuous()" title="Ativar Inspecção Contínua">
            <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"/><circle cx="12" cy="12" r="3"/></svg>
            Inspecionar
          </button>
          <button class="ctrl-btn" id="btnTestAccess" onclick="testAccess()" title="Testar acesso (Tentativa)">
            <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M20 21v-2a4 4 0 0 0-4-4H8a4 4 0 0 0-4 4v2"/><circle cx="12" cy="7" r="4"/></svg>
            Testar Acesso
          </button>
          <button class="ctrl-btn enroll-btn" style="grid-column: span 2;" id="btnOpenEnrollModal" onclick="openEnrollModal()" title="Cadastrar rosto">
            <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M16 21v-2a4 4 0 0 4 4H5a4 4 0 0 4 4v2"/><circle cx="8.5" cy="7" r="4"/><line x1="20" y1="8" x2="20" y2="14"/><line x1="23" y1="11" x2="17" y2="11"/></svg>
            Cadastrar
          </button>
        </div>



        <!-- Stats Bar -->
        <div class="stats-bar" id="statsBar">
          <div class="stat-item">
            <span class="stat-val" id="statSharp">--</span>
            <span class="stat-lbl">Nitidez</span>
          </div>
          <div class="stat-divider"></div>
          <div class="stat-item">
            <span class="stat-val" id="statPeak">--</span>
            <span class="stat-lbl">Pico Nitidez</span>
          </div>
          <div class="stat-divider"></div>
          <div class="stat-item">
            <span class="stat-val" id="statLDR">--</span>
            <span class="stat-lbl">Luz (LDR)</span>
          </div>
        </div>
      </div>

      <!-- ===== RIGHT COLUMN ===== -->
      <div class="right-column">

        <!-- ACCESS LOG PANEL -->
        <div class="panel log-panel">
          <div class="panel-header">
            <div class="panel-title">
              <div class="panel-icon log-icon">
                <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><polyline points="14 2 14 8 20 8"/><line x1="16" y1="13" x2="8" y2="13"/><line x1="16" y1="17" x2="8" y2="17"/><polyline points="10 9 9 9 8 9"/></svg>
              </div>
              Registro de Acessos
            </div>
            <div class="panel-actions">
              <button class="icon-btn" onclick="clearLog()" title="Limpar registro">
                <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="3 6 5 6 21 6"/><path d="M19 6l-1 14H6L5 6"/><path d="M10 11v6"/><path d="M14 11v6"/><path d="M9 6V4h6v2"/></svg>
              </button>
              <button class="icon-btn" onclick="exportLog()" title="Exportar registro">
                <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" y1="15" x2="12" y2="3"/></svg>
              </button>
            </div>
          </div>

          <!-- Log Filters -->
          <div class="log-filters">
            <button class="filter-btn active" data-filter="all" onclick="filterLog('all', this)">Todos</button>
            <button class="filter-btn" data-filter="granted" onclick="filterLog('granted', this)">
              <span class="filter-dot granted"></span> Liberado
            </button>
            <button class="filter-btn" data-filter="denied" onclick="filterLog('denied', this)">
              <span class="filter-dot denied"></span> Negado
            </button>
          </div>

          <!-- Log List -->
          <div class="log-list" id="logList">
            <div class="log-empty" id="logEmpty">
              <svg width="40" height="40" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1"><path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><polyline points="14 2 14 8 20 8"/></svg>
              <p>Nenhum registro ainda</p>
              <span>Os eventos aparecerão aqui conforme ocorrerem</span>
            </div>
          </div>
        </div>

        <!-- SUMMARY CARDS -->
        <div class="cards-row">
          <div class="summary-card granted-card">
            <div class="card-icon">
              <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="20 6 9 17 4 12"/></svg>
            </div>
            <div class="card-info">
              <span class="card-num" id="cardGranted">0</span>
              <span class="card-label">Acessos Liberados</span>
            </div>
            <div class="card-progress">
              <div class="progress-bar granted-bar" id="progressGranted" style="width:0%"></div>
            </div>
          </div>
          <div class="summary-card denied-card">
            <div class="card-icon">
              <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/></svg>
            </div>
            <div class="card-info">
              <span class="card-num" id="cardDenied">0</span>
              <span class="card-label">Acessos Negados</span>
            </div>
            <div class="card-progress">
              <div class="progress-bar denied-bar" id="progressDenied" style="width:0%"></div>
            </div>
          </div>
        </div>

      </div>
    </div>
  </main>

  <!-- ENROLL MODAL -->
  <div class="modal-backdrop" id="enrollModalBackdrop" style="display:none;">
    <div class="modal-box">
      <div class="modal-header">
        <h3>Cadastrar Novo Rosto</h3>
        <button class="modal-close" onclick="closeEnrollModal()">
          <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/></svg>
        </button>
      </div>
      <div class="modal-body">
        <label for="enrollNameInput">Nome da Pessoa</label>
        <input type="text" id="enrollNameInput" placeholder="Ex: João da Silva" />
      </div>
      <div class="modal-footer">
        <button class="btn-secondary" onclick="closeEnrollModal()">Cancelar</button>
        <button class="btn-primary" onclick="confirmEnroll()">Iniciar Cadastro</button>
      </div>
    </div>
  </div>

  <!-- TOAST NOTIFICATION -->
  <div class="toast-container" id="toastContainer"></div>



  <script>
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
  // O feed de vídeo do ESP32 roda na porta 81, separada da porta 80 (API)
  const baseIp = ip.split(':')[0]; // Remove qualquer porta caso o usuário tenha digitado
  const streamUrl = `http://${baseIp}:81/stream`;

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

  if (connected) {
    dot.className = 'status-dot connected';
    label.textContent = `Conectado — ${state.esp32Ip}`;
    liveBadge.className = 'live-badge live';
    liveBadge.innerHTML = '<span class="live-dot"></span> AO VIVO';
  } else {
    dot.className = 'status-dot';
    label.textContent = 'Desconectado';
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
async function sendControl(cmd, extraParams = '') {
  if (!state.connected) {
    showToast('Conecte ao ESP32 primeiro!', 'warning');
    return false;
  }

  try {
    const url = `http://${state.esp32Ip}/control?cmd=${cmd}${extraParams}`;
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
    showToast(`📸 Cadastramento múltiplo iniciado! Olhe para a câmera.${nameStr}`, 'warning');
    addLogEntry('info', 'Cadastramento iniciado', `Aguardando rosto...${nameStr}`);
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

</script>
</body>
</html>

)HTML";

#endif
