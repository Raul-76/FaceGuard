# ESP32 - Reconhecimento Facial (FaceGuard)

Este projeto implementa um sistema de segurança com reconhecimento facial utilizando um microcontrolador ESP32-CAM. O sistema inclui um código para o ESP32 e um **Dashboard Web interativo** para monitoramento e controle em tempo real.

## 📊 O Dashboard Web (FaceGuard)

O Dashboard (localizado na pasta `dashboard`) é uma interface moderna construída com HTML, CSS (com design dark mode e glassmorphism) e JavaScript (Vanilla). Ele se conecta diretamente à câmera do ESP32 através de requisições HTTP para visualizar o stream de vídeo ao vivo e controlar o sistema.

### Funcionalidades Principais:
- **Stream de Vídeo ao Vivo:** Visualização em tempo real do MJPEG stream fornecido pelo ESP32-CAM.
- **Controles da Câmera:**
  - Ligar/Desligar a **Detecção de Rostos**.
  - Ligar/Desligar o **Reconhecimento Facial**.
  - **Cadastrar Novos Rostos (Enrollment)** no sistema para que sejam reconhecidos posteriormente.
  - **Capturar Foto:** Tira um snapshot da câmera e permite fazer o download em `.jpg`.
- **Registro de Acessos (Logs):** Mantém um histórico de tentativas de acesso (Acessos Liberados e Acessos Negados).
  - Possui filtros de eventos (Todos, Liberados, Negados).
  - Permite exportar o registro de acessos em formato CSV.
- **Estatísticas em Tempo Real:** Contador de acessos concedidos e negados exibidos visualmente na interface.

## ⚙️ Como funciona

O ESP32 atua como um servidor HTTP na rede local e o Dashboard funciona como o cliente (frontend).

1. O **Dashboard** requer o endereço IP do ESP32 na rede local para realizar a conexão.
2. Quando conectado, o dashboard se conecta às seguintes rotas (endpoints) disponibilizadas pelo firmware do ESP32:
   - `http://{IP}:81/stream`: Para o stream de vídeo (MJPEG).
   - `http://{IP}/status`: Para consultar o status atual de funcionalidades como reconhecimento e detecção.
   - `http://{IP}/control`: Para ativar/desativar as funcionalidades enviando parâmetros.
   - `http://{IP}/capture`: Para obter um snapshot em imagem (JPEG).
3. Todas as configurações do IP do ESP32 e logs são guardados localmente no navegador (LocalStorage), de forma que se você recarregar a página, não perde os registros.

## 🚀 Como Utilizar

1. Faça o upload do código presente em `CameraWebServer_EletronicaFacil` para sua placa ESP32-CAM através da IDE do Arduino.
2. Anote o endereço IP exibido no Monitor Serial quando o ESP32 se conectar ao Wi-Fi.
3. Abra o arquivo `dashboard/index.html` em qualquer navegador moderno.
4. Na barra superior, digite o endereço IP do seu ESP32 e clique em **Conectar**.
5. Aproveite as funcionalidades do sistema controlando a câmera diretamente pelo Dashboard!

---
Desenvolvido como projeto de monitoramento local de hardware via Web.
