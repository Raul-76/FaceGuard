# 🔐 FaceGuard — Sistema de Segurança com Reconhecimento Facial (ESP32-CAM)

![ESP32-CAM](https://img.shields.io/badge/ESP32--CAM-firmware-E7352C?logo=espressif&logoColor=white)
![C++](https://img.shields.io/badge/C%2B%2B-00599C?logo=cplusplus&logoColor=white)
![Dashboard](https://img.shields.io/badge/Dashboard-HTML%20%7C%20CSS%20%7C%20JS-F7DF1E?logo=javascript&logoColor=black)
![Status](https://img.shields.io/badge/Status-Em%20Desenvolvimento-orange)
![License](https://img.shields.io/badge/License-MIT-green)


Este projeto implementa um sistema de segurança com reconhecimento facial utilizando um microcontrolador **ESP32-CAM**. O sistema é composto por três frentes: firmware embarcado (backend), dashboard web para monitoramento/controle e um case modelado e impresso em 3D.

---

## 🧩 Visão Geral do Sistema

O ESP32-CAM atua como servidor HTTP na rede local, processando o reconhecimento facial e controlando os periféricos de hardware. O Dashboard, hospedado localmente no navegador, se conecta ao ESP32 via HTTP para exibir o stream de vídeo e permitir o controle remoto do sistema.

### 🔧 Hardware Utilizado

- **ESP32-CAM** — captura de imagem e processamento de reconhecimento facial.
- **LED Verde** — indica **Acesso Liberado**.
- **LED Vermelho** — indica **Acesso Negado**.
- **Buzzer** — emite um bipe de erro em tentativas de acesso negadas.
- **LDR (Photoresistor)** — sensor de luminosidade; acende uma luz de apoio automaticamente quando o ambiente está escuro, auxiliando o reconhecimento facial em condições de pouca luz.
- **Case impresso em 3D** — carcaça personalizada, modelada para acomodar a câmera e os demais componentes eletrônicos.

---

## 📊 O Dashboard Web (FaceGuard)

O Dashboard (localizado na pasta `dashboard`) é uma interface moderna construída com HTML, CSS (dark mode com glassmorphism) e JavaScript (Vanilla). Ele se conecta diretamente à câmera do ESP32 através de requisições HTTP para visualizar o stream de vídeo ao vivo e controlar o sistema.

<!-- 📸 Print do Dashboard -->
<p align="center">
  <img src="./dashboard/preview.png" alt="Print do Dashboard FaceGuard" width="700"/>
</p>

### Funcionalidades Principais

- **Stream de Vídeo ao Vivo:** visualização em tempo real do stream MJPEG fornecido pelo ESP32-CAM.
- **Controles da Câmera:**
  - Ligar/Desligar a **Detecção de Rostos**.
  - Ligar/Desligar o **Reconhecimento Facial**.
  - **Cadastrar Novos Rostos (Enrollment)** no sistema, para que sejam reconhecidos posteriormente.
  - **Capturar Foto:** tira um snapshot da câmera e permite o download em `.jpg`.
- **Registro de Acessos (Logs):** histórico de tentativas de acesso (liberados e negados), com filtros de eventos (Todos, Liberados, Negados) e exportação em CSV.
- **Estatísticas em Tempo Real:** contadores de acessos concedidos e negados exibidos visualmente na interface.

---

## 🖨️ Modelo 3D

O case foi modelado sob medida para acomodar o ESP32-CAM e os demais componentes eletrônicos, e posteriormente impresso em impressora 3D.

<!-- 📸 Print/foto do modelo 3D -->
<p align="center">
  <img src="modelo 3d/esboço 3d c componentes.png" alt="Modelo 3D do case FaceGuard" width="500"/>
</p>

---

## ⚙️ Como Funciona

O ESP32 atua como servidor HTTP na rede local e o Dashboard funciona como cliente (frontend).

1. O **Dashboard** requer o endereço IP do ESP32 na rede local para realizar a conexão.
2. Uma vez conectado, o dashboard consome as seguintes rotas (endpoints) disponibilizadas pelo firmware do ESP32:
   - `http://{IP}:81/stream` — stream de vídeo (MJPEG).
   - `http://{IP}/status` — status atual das funcionalidades (reconhecimento, detecção etc.).
   - `http://{IP}/control` — ativação/desativação de funcionalidades via parâmetros.
   - `http://{IP}/capture` — snapshot em imagem (JPEG).
3. As configurações de IP do ESP32 e os logs de acesso são salvos localmente no navegador (`LocalStorage`), preservando o histórico mesmo após recarregar a página.

---

## 📚 Bibliotecas e Ambiente de Desenvolvimento

O firmware foi desenvolvido na IDE do Arduino, com o ambiente configurado para suportar os parâmetros específicos do ESP32 (board manager do ESP32 instalado). Foram utilizadas bibliotecas nativas do ESP-IDF/Arduino-ESP32, entre elas:

- `esp_camera.h`
- `esp_http_server.h`
- `esp_timer.h`
- `img_converters.h`
- `camera_index.h`
- `camera_pins.h`
- `Arduino.h`
- `WiFi.h`
- `fb_gfx.h`
- `fd_forward.h`
- `fr_forward.h`

> O código foi baseado no modelo de câmera `CAMERA_MODEL_AI_THINKER`, com placa ESP32 Wrover Module (ou outra com PSRAM habilitada).

---

## 📁 Estrutura do Repositório

```
FaceGuard/
├── CameraWebServer_EletronicaFacil/   # Firmware do ESP32 (Back-End) — câmera, detecção e reconhecimento facial
├── dashboard/                         # Interface Web (Front-End) — stream, controle e logs
├── modelo-3d/                         # Arquivos de modelagem 3D do case (ex: .stl, .step)
└── README.md
```

**Sugestão de organização:**
- `CameraWebServer_EletronicaFacil/` → mantém o firmware do ESP32.
- `dashboard/` → mantém o Dashboard web.
- `modelo-3d/` → nova pasta para os arquivos de modelagem 3D do case, incluindo os arquivos de impressão (`.stl`/`.step`) e fotos do case finalizado.

---

## 🚀 Como Utilizar

1. Faça o upload do código presente em `CameraWebServer_EletronicaFacil` para sua placa ESP32-CAM através da IDE do Arduino.
2. Anote o endereço IP exibido no Monitor Serial quando o ESP32 se conectar ao Wi-Fi.
3. Abra o arquivo `dashboard/index.html` em qualquer navegador moderno.
4. Na barra superior, digite o endereço IP do seu ESP32 e clique em **Conectar**.
5. Aproveite as funcionalidades do sistema controlando a câmera diretamente pelo Dashboard!

---

## 👥 Equipe e Créditos

| Integrante | Responsabilidade | GitHub | Contato |
|---|---|---|---|
| **Raul Santos** | Desenvolvimento Front-End — construção do Dashboard do FaceGuard | [@Raul-76](https://github.com/Raul-76) | *[preencher e-mail]* |
| **Carlos Eduardo Guimarães** | Modelagem 3D do case da câmera/sistema | [@VoIkmer](https://github.com/VoIkmer) | *[preencher e-mail]* |
| **Caio Marcelo Mazza** | Back-End — firmware do ESP32 (câmera) e montagem do modelo computacional de reconhecimento facial | *[preencher GitHub]* | *[preencher e-mail]* |

---

Desenvolvido como projeto de monitoramento e controle de acesso local via hardware embarcado + Web.
