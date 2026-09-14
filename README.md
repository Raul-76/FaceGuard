# 🔐 FaceGuard - Smart Lock with Facial Recognition (ESP32-S3)

![ESP32-S3](https://img.shields.io/badge/ESP32--S3-firmware-E7352C?logo=espressif&logoColor=white)
![C++](https://img.shields.io/badge/C%2B%2B-00599C?logo=cplusplus&logoColor=white)
![Dashboard](https://img.shields.io/badge/Dashboard-HTML%20%7C%20CSS%20%7C%20JS-F7DF1E?logo=javascript&logoColor=black)
![Status](https://img.shields.io/badge/Status-Conclu%C3%ADdo-brightgreen)
![License](https://img.shields.io/badge/License-MIT-green)

<p align="center"> <img src="modelo 3d/foto-faceguard.jpeg" alt="FaceGuard schematic" width="400"/> </p>

This project implements a **facial recognition security lock** built around an **ESP32-S3 board (Freenove ESP32-S3-WROOM CAM, OV2640 sensor)**. The system comprises three components: embedded firmware (handling computer vision and lock control), a web dashboard for monitoring and control, and a custom 3D-printed enclosure.

**Video presentation:** A full walkthrough of the project (team introduction, live demo, and the challenges we faced) is available [on YouTube](https://youtu.be/lPA_LZTQk2s).

---

## 🧩 System Overview

The ESP32-S3 acts as a local network HTTP server, continuously capturing video, performing facial detection and recognition, and driving hardware peripherals—including the **servo motor that physically engages and disengages the lock**. The dashboard, accessed via a local web browser, connects to the ESP32 over HTTP to display the live video stream and enable remote system control.

Recognition decisions are not based on a single frame; instead, each access attempt collects a **burst of valid frames and requires a minimum number of positive votes** to unlock the mechanism, thereby reducing false positives. Before inference takes place, a **quality gate** discards blurred frames or those with excessive motion, and camera exposure is fixed to ensure stable embeddings.

### 🔧 Hardware Used

<!-- Schematic --> 
<p align="center"> <img src="modelo 3d/esquematico circuito.png" alt="FaceGuard schematic" width="700"/> </p>

- **ESP32-S3 (Freenove ESP32-S3-WROOM CAM, OV2640)** — image capture and facial recognition processing.
- **Servo Motor** — physically locks/unlocks the mechanism at the end of each access attempt.
- **Green LED** — indicates **Access Granted**.
- **Red LED** — indicates **Access Denied**.
- **Blue LED ("breathing" RGB effect)** — indicates the system is processing a recognition attempt.
- **COB Led strip** — dynamic lighting, for low brightness environment.
- **LDR (Photoresistor)** — sensor used to determine the dutycycle fed to the led strip.
- **Buzzer** — plays distinct melodies for access granted, access denied, alarm/tamper alerts, and system startup. 
- **Physical button** — manually triggers a recognition attempt.
- **Deep Sleep switch** — puts the system into ultra-low power mode; the board wakes up automatically when the switch is toggled.
- **3D-printed case** — custom enclosure designed to house the camera and other electronic components.

---
## The Firmware

The firmware is written in C++ on the Arduino framework and uses EloquentEsp32cam for face detection, recognition and enrollment, ESP32Servo (adapted for the ESP32-S3) for the latch, and esp_http_server for the web interface. It runs face recognition entirely on the board, with no server or cloud service involved.

### Architecture

- **Dual-Core Task Separation:** FreeRTOS splits the workload across both cores. Core 0 serves HTTP requests and the video stream, while core 1 runs the recognition pipeline, so that network traffic never stalls inference.
- **Shared Frame Buffer:** Camera capture happens exclusively in the main loop, writing to a PSRAM buffer. The HTTP server only reads from it, guarded by a FreeRTOS mutex, which avoids frame buffer contention between recognition and streaming.
- **Memory Configuration:** A Huge APP partition scheme and OPI PSRAM are required to fit the recognition model alongside the camera pipeline on the target board.
- **Network Interfaces:** An MJPEG stream on port 81 and a REST endpoint on port 80, which serve the dashboard described above.

### Recognition Pipeline

<!-- Recognition decision flow -->
<p align="center">
  <img src="modelo 3d/Diagrama-votacao-temporal.png" alt="FaceGuard recognition decision flow" width="700"/>
</p>

<p align="center">
  <img src="modelo 3d/diagrama_multiplo_cadastro.png" alt="FaceGuard recognition decision flow" width="700"/>
</p>

- **K-of-N Voting:** A decision layer sits around the model. Instead of unlocking on a single frame, the firmware requires agreement across consecutive frames, which reduces both false accepts and false rejects caused by momentary bad frames.
- **Multi-Sample Enrollment:** A user can be enrolled from several captures, covering a wider region of the embedding space and improving recognition across angles and lighting.
- **Quality Gating:** Captures that do not meet the quality criteria are rejected at enrollment, so that poor samples never enter the database.
- **Illumination Control:** The LDR feedback loop adjusts illumination, keeping the face adequately lit before inference runs.

---

## The Web Dashboard

The Dashboard (located in the `dashboard` folder) is a modern interface built using HTML, CSS (dark mode with glassmorphism), and JavaScript (Vanilla). It connects directly to the ESP32 camera via HTTP requests to view the live video stream and control the system. And it is accessible from both desktop and mobile browsers.
<!-- 📸 Print do Dashboard -->

<p align="center">
  <img src="dashboard/dashboard preview.jpg" alt="FaceGuard Dashboard" width="45%"/>
  <img src="modelo 3d/dashboard_mobile.jpeg" alt="FaceGuard Dashboard on mobile" width="45%"/>
</p>

### Key Features

- **Live Video Stream:** Real-time viewing of the MJPEG stream provided by the ESP32, with automatic reconnection in the event of a connection loss.
- **Access Security:** Login-protected startup screen for administrators.
- **Camera Controls:**
- **Access Test:** Triggers a live recognition attempt on the camera. 
- **Face Enrollment:** Single or multiple registration (capturing several samples) of the same face, with an option to cancel an ongoing enrollment. 
- **Face Management:** Screen to view registered faces and rename or delete users (individually or in batches). 
- **Remote Unlock:** Remotely releases the lock via the dashboard, without requiring facial recognition.
- **Access Logs:** History of access attempts (granted and denied), with event filters (All, Granted, Denied) and CSV export functionality.
- **Real-Time Statistics:** Counters for granted and denied access attempts displayed visually on the interface.

---

## 3D Model

The enclosure was custom-designed to house the ESP32-S3, the servo-operated lock, and other electronic components, and was subsequently 3D printed. <!-- 📸 3D model screenshot/photo -->
<p align="center">
<img src="modelo 3d/Modelo 3D Final.png" alt="3D model of the FaceGuard case - View 1" width="500"/>
</p>

<!-- 📸 3D model screenshot/photo -->
<p align="center">
<img src="modelo 3d/Modelo 3D Final - 2.png" alt="3D model of the FaceGuard case - View 2" width="500"/>
</p>

<p align="center">
<img src="modelo 3d/Modelo 3D Final -3.png" alt="3D model of the Servo Motor case" width="500"/>
</p>


---

## ⚙️ How It Works

The ESP32 acts as an HTTP server on the local network, while the Dashboard functions as the client (frontend).

1. The **Dashboard** requires the ESP32's local network IP address to establish a connection (the device is also advertised via **mDNS** as `FaceGuard.local`).
2. The system splits communication across **two ports** on the same IP to prevent command processing from stalling during video transmission:
- **Port 81:** Dedicated exclusively to the video stream. 
- `http://{IP}:81/stream` — live video stream (MJPEG). 
- **Port 80 (Default):** Dedicated to the interface, API, and commands. 
- `http://{IP}/` — debug/monitoring page hosted on the board itself. 
- `http://{IP}/info` — real-time system status (JSON): recognition state, LDR reading, recent access logs, etc.
- `http://{IP}/control?cmd={command}` — executes commands such as starting registration, testing access, remote unlocking, or renaming/deleting a face. 
- `http://{IP}/faces` — returns the list of all faces stored in the ESP32's memory. 3. **Access attempt workflow:** the physical button (or remote command) triggers the capture of a burst of frames → each frame passes through a quality gate (sharpness and motion) → valid frames are compared against registered faces → upon reaching the minimum number of positive matches, the green LED lights up, the success melody plays, and the servo unlocks the lock for a few seconds before locking it again; otherwise, the red LED lights up and the access-denied melody plays.
4. The fill light (COB) is automatically adjusted via a closed-loop LDR circuit, maintaining stable facial illumination even in dark environments.
5. When the deep sleep switch is closed, the system enters an ultra-low power state and the lock remains engaged; it wakes up automatically when the switch is opened.
6. ESP32 IP settings and access logs are saved locally in the browser (`LocalStorage`), preserving the history even after the page is reloaded.

---

## 📚 Libraries and Development Environment

The firmware was developed using the Arduino IDE (ESP32 Core 2.0.14), with the environment configured for the **ESP32-S3** board (OPI PSRAM enabled, "Huge APP" partition scheme). Key libraries used:

- `eloquent_esp32cam.h` — camera capture and high-level pipeline for ESP32-CAM/S3.
- `eloquent_esp32cam/face/detection.h` — face detection.
- `eloquent_esp32cam/face/recognition.h` — face recognition (registration, comparison, and similarity voting).
- `WiFi.h` — local network connection.
- `esp_http_server.h` — native ESP-IDF HTTP server (lighter than `WebServer.h`), used for API routes and the MJPEG stream.
- `ESP32Servo.h` — control of the lock's servo motor.
- `ESPmDNS.h` — device discovery on the local network via `FaceGuard.local`.
- `driver/rtc_io.h` / `esp_sleep.h` — configuration of the wake-up pin and deep sleep mode.

> The code was based on the **Freenove ESP32-S3-WROOM CAM** board with the **OV2640** sensor. ---

## 📁 Repository Structure

```
FaceGuard/
├── reconhecimento_facial/   # ESP32-S3 Firmware (Back-End) — camera, detection, recognition, and lock control
│   ├── reconhecimento_facial.ino
│   ├── dashboard.h          # HTML/CSS/JS for the debug page embedded on the board
│   └── partitions.csv       # Partition scheme (Huge APP)
├── dashboard/                # Web Interface (Front-End) — stream, control, and logs
│   ├── index.html
│   ├── app.js
│   ├── style.css
│   └── dashboard preview.jpg
├── modelo 3d/                 # 3D modeling files for the case (.stl, screenshots, and sketches)
│   ├── Face Guard.stl
│   ├── esboço 3d.stl
│   ├── esboço 3d.html
│   └── *.png
└── README.md
```

---

## 🚀 How to Use

1. Upload the code found in `reconhecimento_facial/reconhecimento_facial.ino` to the ESP32-S3-WROOM CAM board using the Arduino IDE (Core 2.0.14, OPI PSRAM, Huge APP partition).
2. Note the IP address displayed in the Serial Monitor when the ESP32 connects to Wi-Fi (or use `FaceGuard.local` via mDNS).
3. Open the `dashboard/index.html` file in any modern web browser.
4. In the top bar, enter your ESP32's IP address (or `FaceGuard.local`) and click **Connect**.
5. Register authorized faces using the Dashboard's face management tab.
6. Enjoy the system's features by controlling the camera and lock directly from the Dashboard!

---

## 👥 Team and Credits

| Member | Responsibility | GitHub | Contact |
|---|---|---|---|
| **Raul Jesus dos Santos** | Front-End Development — building the FaceGuard Dashboard | [@Raul-76](https://github.com/Raul-76) | [Email](raul.js.fla@gmail.com) |
| **Carlos Eduardo Guimarães** | 3D modeling of the camera/system enclosure | [@VoIkmer](https://github.com/VoIkmer) | [Email](cguimaraes03@gmail.com) |
| **Caio Marcelo Mazza** | Back-End — ESP32-S3 firmware (camera, facial recognition, and lock control) | [@Caiompmazza](https://github.com/caiompmazza) | [Email](caio.mpmazza@gmail.com) |

---

Completed local access control project using embedded hardware and a web interface, combining facial recognition, a motorized lock, and a real-time monitoring interface.
