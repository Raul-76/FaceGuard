# 🔐 FaceGuard — Fechadura Inteligente com Reconhecimento Facial (ESP32-S3)

![ESP32-S3](https://img.shields.io/badge/ESP32--S3-firmware-E7352C?logo=espressif&logoColor=white)
![C++](https://img.shields.io/badge/C%2B%2B-00599C?logo=cplusplus&logoColor=white)
![Dashboard](https://img.shields.io/badge/Dashboard-HTML%20%7C%20CSS%20%7C%20JS-F7DF1E?logo=javascript&logoColor=black)
![Status](https://img.shields.io/badge/Status-Conclu%C3%ADdo-brightgreen)
![License](https://img.shields.io/badge/License-MIT-green)

Este projeto implementa uma **fechadura de segurança com reconhecimento facial**, construída em torno de uma placa **ESP32-S3 (Freenove ESP32-S3-WROOM CAM, sensor OV2640)**. O sistema é composto por três frentes: firmware embarcado (back-end de visão computacional + controle da fechadura), dashboard web para monitoramento/controle e um case modelado e impresso em 3D.

---

## 🧩 Visão Geral do Sistema

O ESP32-S3 atua como servidor HTTP na rede local, capturando vídeo continuamente, executando a detecção/reconhecimento facial e acionando os periféricos de hardware — incluindo o **motor de servo que trava e destrava a fechadura fisicamente**. O Dashboard, aberto localmente no navegador, se conecta ao ESP32 via HTTP para exibir o stream de vídeo ao vivo e permitir o controle remoto do sistema.

O reconhecimento não decide com base em um único frame: cada tentativa de acesso coleta uma **rajada de frames válidos e exige um número mínimo de votos a favor** para liberar a fechadura, o que reduz falsos positivos. Antes de qualquer inferência, um **quality-gate** descarta frames desfocados ou com movimento excessivo, e a exposição da câmera é fixada para manter os embeddings estáveis.

### 🔧 Hardware Utilizado

- **ESP32-S3 (Freenove ESP32-S3-WROOM CAM, OV2640)** — captura de imagem e processamento de reconhecimento facial.
- **Motor de Servo** — trava/destrava fisicamente a fechadura ao final de cada tentativa de acesso.
- **LED Verde** — indica **Acesso Liberado**.
- **LED Vermelho** — indica **Acesso Negado**.
- **LED Azul (RGB "respirando")** — indica que o sistema está processando uma tentativa de reconhecimento.
- **Buzzer** — toca melodias distintas para acesso liberado, acesso negado, alarme/violação e boot do sistema.
- **LDR (Fotorresistor)** — sensor de luminosidade em malha fechada: ajusta automaticamente o brilho de uma luz de apoio (COB via MOSFET) para manter a iluminação do rosto estável durante o reconhecimento.
- **Botão físico** — dispara manualmente uma tentativa de reconhecimento.
- **Switch de Deep Sleep** — coloca o sistema em modo de baixíssimo consumo; a placa acorda automaticamente ao abrir o switch.
- **Case impresso em 3D** — carcaça personalizada, modelada para acomodar a câmera e os demais componentes eletrônicos.

---

## 📊 O Dashboard Web (FaceGuard)

O Dashboard (localizado na pasta `dashboard`) é uma interface moderna construída com HTML, CSS (dark mode com glassmorphism) e JavaScript (Vanilla). Ele se conecta diretamente à câmera do ESP32 através de requisições HTTP para visualizar o stream de vídeo ao vivo e controlar o sistema.

<!-- 📸 Print do Dashboard -->
<p align="center">
  <img src="dashboard/dashboard preview.jpg" alt="Print do Dashboard FaceGuard" width="700"/>
</p>

### Funcionalidades Principais

- **Stream de Vídeo ao Vivo:** visualização em tempo real do stream MJPEG fornecido pelo ESP32, com reconexão automática em caso de queda de conexão.
- **Segurança de Acesso:** tela inicial protegida por login para administradores.
- **Controles da Câmera:**
  - **Teste de Acesso:** dispara uma tentativa de reconhecimento real na câmera.
  - **Cadastrar Novos Rostos (Enrollment):** cadastro simples ou múltiplo (captura de várias amostras) de um mesmo rosto, com opção de cancelar o cadastro em andamento.
  - **Gerenciamento de Rostos:** tela para visualizar cadastros, renomear ou apagar usuários (individualmente ou em lote).
  - **Abertura Remota (Remote Unlock):** libera a fechadura remotamente via dashboard, sem precisar de reconhecimento facial.
- **Registro de Acessos (Logs):** histórico de tentativas de acesso (liberados e negados), com filtros de eventos (Todos, Liberados, Negados) e exportação em CSV.
- **Estatísticas em Tempo Real:** contadores de acessos concedidos e negados exibidos visualmente na interface.

---

## 🖨️ Modelo 3D

O case foi modelado sob medida para acomodar o ESP32-S3, a fechadura com servo e os demais componentes eletrônicos, e posteriormente impresso em impressora 3D.

<!-- 📸 Print/foto do modelo 3D -->
<p align="center">
  <img src="modelo 3d/Modelo 3D Final.png" alt="Modelo 3D do case FaceGuard Visão 1" width="500"/>
</p>

<!-- 📸 Print/foto do modelo 3D -->
<p align="center">
  <img src="modelo 3d/Modelo 3D Final - 2.png" alt="Modelo 3D do case FaceGuard Visão 2" width="500"/>
</p>

---

## ⚙️ Como Funciona

O ESP32 atua como servidor HTTP na rede local e o Dashboard funciona como cliente (frontend).

1. O **Dashboard** requer o endereço IP do ESP32 na rede local para realizar a conexão (o dispositivo também é anunciado via **mDNS** como `FaceGuard.local`).
2. O sistema divide a comunicação em **duas portas** no mesmo IP para não travar os comandos enquanto transmite o vídeo:
   - **Porta 81:** Dedicada exclusivamente ao stream de vídeo.
     - `http://{IP}:81/stream` — stream de vídeo ao vivo (MJPEG).
   - **Porta 80 (Padrão):** Dedicada para a interface, a API e os comandos.
     - `http://{IP}/` — página de debug/monitoramento embarcada na própria placa.
     - `http://{IP}/info` — status em tempo real do sistema (JSON): estado do reconhecimento, leitura do LDR, últimos acessos, etc.
     - `http://{IP}/control?cmd={comando}` — execução de comandos como iniciar cadastro, testar acesso, liberar remotamente, renomear ou apagar um rosto.
     - `http://{IP}/faces` — retorna a lista de todos os rostos salvos na memória do ESP32.
3. **Fluxo de uma tentativa de acesso:** o botão físico (ou o comando remoto) dispara a captura de uma rajada de frames → cada frame passa pelo quality-gate (nitidez e movimento) → frames válidos são comparados contra os rostos cadastrados → ao atingir o número mínimo de votos a favor, o LED verde acende, a melodia de sucesso toca e o servo destrava a fechadura por alguns segundos antes de travar novamente; caso contrário, o LED vermelho acende e a melodia de acesso negado toca.
4. A luz de apoio (COB) é ajustada automaticamente pelo LDR em malha fechada, mantendo a iluminação do rosto estável mesmo em ambientes escuros.
5. Quando o switch de deep sleep é fechado, o sistema entra em baixíssimo consumo e a fechadura permanece travada; ele acorda automaticamente ao abrir o switch.
6. As configurações de IP do ESP32 e os logs de acesso são salvos localmente no navegador (`LocalStorage`), preservando o histórico mesmo após recarregar a página.

---

## 📚 Bibliotecas e Ambiente de Desenvolvimento

O firmware foi desenvolvido na IDE do Arduino (Core ESP32 2.0.14), com o ambiente configurado para a placa **ESP32-S3** (OPI PSRAM habilitada, partição "Huge APP"). Principais bibliotecas utilizadas:

- `eloquent_esp32cam.h` — captura de câmera e pipeline de alto nível para o ESP32-CAM/S3.
- `eloquent_esp32cam/face/detection.h` — detecção de rosto.
- `eloquent_esp32cam/face/recognition.h` — reconhecimento facial (cadastro, comparação e votação por similaridade).
- `WiFi.h` — conexão à rede local.
- `esp_http_server.h` — servidor HTTP nativo do ESP-IDF (mais leve que `WebServer.h`), usado para as rotas de API e o stream MJPEG.
- `ESP32Servo.h` — controle do servo motor da fechadura.
- `ESPmDNS.h` — descoberta do dispositivo na rede local via `FaceGuard.local`.
- `driver/rtc_io.h` / `esp_sleep.h` — configuração do pino de wake-up e do modo deep sleep.

> O código foi baseado na placa **Freenove ESP32-S3-WROOM CAM** com sensor **OV2640**.

---

## 📁 Estrutura do Repositório

```
FaceGuard/
├── reconhecimento_facial/   # Firmware do ESP32-S3 (Back-End) — câmera, detecção, reconhecimento e controle da fechadura
│   ├── reconhecimento_facial.ino
│   ├── dashboard.h          # HTML/CSS/JS da página de debug embarcada na placa
│   └── partitions.csv       # esquema de partições (Huge APP)
├── dashboard/                # Interface Web (Front-End) — stream, controle e logs
│   ├── index.html
│   ├── app.js
│   ├── style.css
│   └── dashboard preview.jpg
├── modelo 3d/                 # Arquivos de modelagem 3D do case (.stl, capturas e esboços)
│   ├── Face Guard.stl
│   ├── esboço 3d.stl
│   ├── esboço 3d.html
│   └── *.png
└── README.md
```

---

## 🚀 Como Utilizar

1. Faça o upload do código presente em `reconhecimento_facial/reconhecimento_facial.ino` para a placa ESP32-S3-WROOM CAM através da IDE do Arduino (Core 2.0.14, OPI PSRAM, partição Huge APP).
2. Anote o endereço IP exibido no Monitor Serial quando o ESP32 se conectar ao Wi-Fi (ou use `FaceGuard.local` via mDNS).
3. Abra o arquivo `dashboard/index.html` em qualquer navegador moderno.
4. Na barra superior, digite o endereço IP (ou `FaceGuard.local`) do seu ESP32 e clique em **Conectar**.
5. Cadastre os rostos autorizados pela aba de gerenciamento de rostos do Dashboard.
6. Aproveite as funcionalidades do sistema controlando a câmera e a fechadura diretamente pelo Dashboard!

---

## 👥 Equipe e Créditos

| Integrante | Responsabilidade | GitHub | Contato |
|---|---|---|---|
| **Raul Jesus dos Santos** | Desenvolvimento Front-End — construção do Dashboard do FaceGuard | [@Raul-76](https://github.com/Raul-76) | [Email](raul.js.fla@gmail.com) |
| **Carlos Eduardo Guimarães** | Modelagem 3D do case da câmera/sistema | [@VoIkmer](https://github.com/VoIkmer) | [Email](cguimaraes03@gmail.com) |
| **Caio Marcelo Mazza** | Back-End — firmware do ESP32-S3 (câmera, reconhecimento facial e controle da fechadura) | [@Caiompmazza](https://github.com/caiompmazza) | [Email](caio.mpmazza@gmail.com) |

---

Projeto concluído de controle de acesso local via hardware embarcado + Web, combinando reconhecimento facial, fechadura motorizada e uma interface de monitoramento em tempo real.
