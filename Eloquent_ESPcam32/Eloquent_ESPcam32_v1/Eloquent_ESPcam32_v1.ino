/**
 * ESP32-S3 Face Recognition — DIAGNOSTICO + LIVE FEED
 * Freenove ESP32-S3-WROOM CAM | OV2640
 *
 * Core 2.0.14 | OPI PSRAM | Huge APP | Serial 115200 + New Line
 *
 * O live feed mostra EXATAMENTE o frame usado na inferencia.
 * Use o valor "sharp" (bytes do JPEG) para focar a lente:
 * gire devagar buscando MAXIMIZAR sharpMax.
 */

#include <eloquent_esp32cam.h>
#include <eloquent_esp32cam/face/detection.h>
#include <eloquent_esp32cam/face/recognition.h>

#include <WiFi.h>
#include <esp_http_server.h>

using eloq::camera;
using eloq::face::detection;
using eloq::face::recognition;

// ---------------- VOTACAO POR RAJADA ----------------
#define ALVO_NOME     "caio"   // quem autoriza
#define PISO_SIM      0.95f    // sim minimo pra um frame virar voto valido
#define JANELA_N      7        // frames validos a coletar por tentativa
#define VOTOS_K       5        // votos a favor necessarios (K de N)
#define TIMEOUT_MS    12000     // aborta se nao juntar N validos a tempo

// ---------------- CONFIG ----------------
#define WIFI_SSID "Caio.2g"
#define WIFI_PASS "28460363"
#define JPG_CAP   40000        // 240x240 @ quality high cabe folgado
#define FRAME_W   240          // camera.resolution.face()
#define FRAME_H   240
// ----------------------------------------

String prompt(String message);
void doEnroll();
void runRecognition();
void enrollMultiplo(int alvo);

bool     modoContinuo = false;
uint32_t sharpMax     = 0;
uint32_t lastPrint    = 0;


// resultado de uma tentativa
enum Veredito { PENDENTE, APROVADO, NEGADO, EXPIROU };

Veredito runTentativa() {
    int      validos    = 0;    // frames que passaram e foram reconhecidos
    int      votosFavor = 0;    // subset dos validos que bateu ALVO + piso
    uint32_t t0         = millis();

    Serial.println(">> TENTATIVA iniciada");

    while (validos < JANELA_N) {
        // ----- timeout: rosto nao aparece / some -----
        if (millis() - t0 > TIMEOUT_MS) {
            Serial.printf(">> EXPIROU (so %d/%d validos)\n", validos, JANELA_N);
            return EXPIROU;
        }

        // ----- 1 frame fresco -----
        if (!camera.capture().isOk()) { delay(20); continue; }
        publishFrame(camera.frame->buf, camera.frame->len);  // mantem feed vivo

        // ----- (aqui entraria o quality-gate; por ora so detect) -----
        if (!recognition.detect().isOk())    continue;  // sem rosto: nao conta
        if (!recognition.recognize().isOk()) continue;  // nao reconheceu: nao conta

        // frame VALIDO (tinha rosto e o modelo respondeu)
        validos++;
        const char* nome = recognition.match.name.c_str();
        float       sim  = recognition.match.similarity;

        // ----- o voto: precisa ser o nome-alvo E passar do piso -----
        bool aFavor = (strcmp(nome, ALVO_NOME) == 0) && (sim >= PISO_SIM);
        if (aFavor) votosFavor++;

        Serial.printf("   frame %d/%d: %s sim=%.3f -> %s  (favor=%d)\n",
                      validos, JANELA_N, nome, sim,
                      aFavor ? "VOTO" : "descartado", votosFavor);
    }

    // ----- juiz: fecha a janela e decide -----
    if (votosFavor >= VOTOS_K) {
        Serial.printf(">> APROVADO (%d/%d votos)\n", votosFavor, validos);
        return APROVADO;
    }
    Serial.printf(">> NEGADO (%d/%d votos, precisava %d)\n",
                  votosFavor, validos, VOTOS_K);
    return NEGADO;
}

// ---------- buffer compartilhado do frame ----------
static uint8_t*          g_jpg     = nullptr;
static size_t            g_jpgLen  = 0;
static volatile uint32_t g_frameId = 0;
static SemaphoreHandle_t g_mutex   = nullptr;
static char              g_info[220] = "{\"face\":0,\"sharp\":0,\"peak\":0,\"name\":\"-\",\"sim\":\"-\"}";
static httpd_handle_t    g_server  = nullptr;

void publishFrame(const uint8_t* buf, size_t len) {
    if (!g_jpg || !buf || len == 0 || len > JPG_CAP) return;
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
    memcpy(g_jpg, buf, len);
    g_jpgLen = len;
    g_frameId++;
    xSemaphoreGive(g_mutex);
}

// ---------------- HTTP ----------------
static esp_err_t infoHandler(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, g_info, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t streamHandler(httpd_req_t* req) {
    uint8_t* local = (uint8_t*) ps_malloc(JPG_CAP);
    if (!local) return ESP_FAIL;

    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=frame");
    uint32_t lastId = 0;
    char part[96];

    while (true) {
        size_t len = 0;
        if (xSemaphoreTake(g_mutex, portMAX_DELAY) == pdTRUE) {
            if (g_frameId != lastId && g_jpgLen) {
                memcpy(local, g_jpg, g_jpgLen);
                len    = g_jpgLen;
                lastId = g_frameId;
            }
            xSemaphoreGive(g_mutex);
        }
        if (!len) { delay(10); continue; }

        if (httpd_resp_send_chunk(req, "\r\n--frame\r\n", 11) != ESP_OK) break;
        int n = snprintf(part, sizeof(part),
                         "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                         (unsigned) len);
        if (httpd_resp_send_chunk(req, part, n) != ESP_OK) break;
        if (httpd_resp_send_chunk(req, (const char*) local, len) != ESP_OK) break;
    }
    free(local);
    return ESP_OK;
}

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><meta charset=utf-8><title>debugging</title>
<style>
body{background:#111;color:#eee;font-family:monospace;text-align:center;margin:12px}
#wrap{position:relative;display:inline-block;width:480px;height:480px}
img{width:480px;height:480px;display:block;image-rendering:auto}
.cx,.cy{position:absolute;background:#f0f;opacity:.55}
.cx{left:50%;top:0;bottom:0;width:1px}
.cy{top:50%;left:0;right:0;height:1px}
#safe{position:absolute;left:15%;top:15%;width:70%;height:70%;border:1px dashed #ff0;opacity:.5}
pre{font-size:15px;text-align:left;display:inline-block;line-height:1.5}
</style>
<div id=wrap>
  <img src="/stream">
  <div class=cx></div><div class=cy></div><div id=safe></div>
</div>
<pre id=info>conectando...</pre>
<script>
setInterval(async () => {
  try {
    const d = await (await fetch('/info')).json();
    document.getElementById('info').textContent =
      'rosto     : ' + (d.face ? 'DETECTADO' : 'nao')            + '\n' +
      'nitidez   : ' + d.sharp + '   (pico: ' + d.peak + ')'     + '\n' +
      'match     : ' + d.name + '   sim=' + d.sim;
  } catch(e) {}
}, 250);
</script>
)HTML";

static esp_err_t indexHandler(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

void startServer() {
    httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.ctrl_port        = 32768;
    cfg.max_uri_handlers = 8;
    cfg.stack_size       = 8192;

    if (httpd_start(&g_server, &cfg) != ESP_OK) {
        Serial.println("ERRO: httpd falhou");
        return;
    }

    httpd_uri_t u1 = { "/",       HTTP_GET, indexHandler,  nullptr };
    httpd_uri_t u2 = { "/stream", HTTP_GET, streamHandler, nullptr };
    httpd_uri_t u3 = { "/info",   HTTP_GET, infoHandler,   nullptr };
    httpd_register_uri_handler(g_server, &u1);
    httpd_register_uri_handler(g_server, &u2);
    httpd_register_uri_handler(g_server, &u3);
}

// ---------------- SETUP ----------------
void setup() {
    delay(2000);
    Serial.begin(115200);
    Serial.println("\n=== DIAGNOSTICO + LIVE FEED ===");

    camera.pinout.freenove_s3();
    camera.brownout.disable();
    camera.resolution.face();      // 240x240
    camera.quality.high();
    camera.xclk.slow(); 

    detection.accurate();
    detection.confidence(0.7);

    // threshold BAIXO de proposito: queremos VER os numeros, nao filtrar.
    recognition.confidence(0.1);

    while (!camera.begin().isOk())
        Serial.println(camera.exception.toString());

    while (!recognition.begin().isOk())
        Serial.println(recognition.exception.toString());

    Serial.println("Camera OK / Recognizer OK");

// ---------------- SENSOR (depois do begin!) ----------------
    sensor_t* s = esp_camera_sensor_get();
    if (s) {
        s->set_gain_ctrl(s, 1);                                  // AGC on
        s->set_gainceiling(s, (gainceiling_t) GAINCEILING_2X);   // TETO no ganho -> menos ruido
        s->set_exposure_ctrl(s, 1);                              // AEC on
        s->set_ae_level(s, 1);        // -2..2  sobe se estiver escuro
        s->set_brightness(s, 1);      // -2..2
        s->set_contrast(s, 1);        // -2..2  ajuda o detalhe fino
        s->set_saturation(s, 0);
        s->set_whitebal(s, 1);
        s->set_awb_gain(s, 1);
        s->set_lenc(s, 1);            // corrige vinheta da lente
        s->set_hmirror(s, 0);
        s->set_vflip(s, 0);
        s->set_dcw(s, 1);
        Serial.println("Sensor tunado");
    }
    else Serial.println("AVISO: sensor_get falhou");

    // ---- buffer + rede ANTES do prompt (prompt e bloqueante) ----
    g_mutex = xSemaphoreCreateMutex();
    g_jpg   = (uint8_t*) ps_malloc(JPG_CAP);
    if (!g_jpg) {
        Serial.println("ERRO: ps_malloc falhou. PSRAM habilitada? (OPI PSRAM)");
        while (true) delay(1000);
    }

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("WiFi");
    while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print("."); }
    Serial.println();

    startServer();
    Serial.print(">>> ABRA NO NAVEGADOR:  http://");
    Serial.println(WiFi.localIP());
    Serial.println();

    if (prompt("Apagar cadastros? [s|n]").startsWith("s")) {
        recognition.deleteAll();
        Serial.println("Apagado.");
    }

    Serial.println();
    Serial.println("COMANDOS:");
    Serial.println("  c = cadastrar rosto");
    Serial.println("  r = MODO CONTINUO (imprime similaridade)");
    Serial.println("  p = pausar (feed continua rodando)");
    Serial.println("  d = listar cadastrados");
    Serial.println("  z = zerar pico de nitidez (use ao rosquear a lente)");
    Serial.println("  t = para testar a detecção por multiplos frames)");
    Serial.println("  m = para multiplos enrolls)");
    Serial.println();
}

// ---------------- LOOP ----------------
void loop() {
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();

        if      (cmd.startsWith("c")) { modoContinuo = false; doEnroll(); }
        else if (cmd.startsWith("r")) { modoContinuo = true;  Serial.println(">> MODO CONTINUO"); }
        else if (cmd.startsWith("p")) { modoContinuo = false; Serial.println(">> PAUSADO (feed ativo)"); }
        else if (cmd.startsWith("d")) { recognition.dump(); }
        else if (cmd.startsWith("z")) { sharpMax = 0; Serial.println(">> pico zerado"); }
        else if (cmd.startsWith("t")) { runTentativa(); }   // "t" = tentar
        else if (cmd.startsWith("m")) { modoContinuo = false; enrollMultiplo(6); }
    }

    // SEMPRE captura + publica: o feed roda mesmo pausado (pra focar a lente)
    if (!camera.capture().isOk()) {
        delay(100);
        return;
    }

    publishFrame(camera.frame->buf, camera.frame->len);

    // proxy de nitidez: com quality fixa, imagem desfocada comprime mais
    uint32_t sharp = camera.frame->len;
    if (sharp > sharpMax) sharpMax = sharp;

    if (modoContinuo)
        runRecognition();
    else
        snprintf(g_info, sizeof(g_info),
                 "{\"face\":0,\"sharp\":%u,\"peak\":%u,\"name\":\"(pausado)\",\"sim\":\"-\"}",
                 (unsigned) sharp, (unsigned) sharpMax);

    delay(10);
}

/**
 * Detecta + reconhece o MESMO frame que acabou de ir pro navegador.
 */
void runRecognition() {
    uint32_t sharp = camera.frame->len;

    if (!recognition.detect().isOk()) {
        snprintf(g_info, sizeof(g_info),
                 "{\"face\":0,\"sharp\":%u,\"peak\":%u,\"name\":\"-\",\"sim\":\"-\"}",
                 (unsigned) sharp, (unsigned) sharpMax);
        return;
    }

    if (!recognition.recognize().isOk()) {
        snprintf(g_info, sizeof(g_info),
                 "{\"face\":1,\"sharp\":%u,\"peak\":%u,\"name\":\"?\",\"sim\":\"-\"}",
                 (unsigned) sharp, (unsigned) sharpMax);
        return;
    }

    const char* name = recognition.match.name.c_str();
    float       sim  = recognition.match.similarity;

    snprintf(g_info, sizeof(g_info),
             "{\"face\":1,\"sharp\":%u,\"peak\":%u,\"name\":\"%s\",\"sim\":\"%.4f\"}",
             (unsigned) sharp, (unsigned) sharpMax, name, sim);

    // throttle do serial (nao trava o stream)
    if (millis() - lastPrint > 400) {
        lastPrint = millis();
        Serial.printf("MATCH: %-10s sim=%.4f  sharp=%u  (%dms)\n",
                      name, sim, (unsigned) sharp, recognition.benchmark.millis());
    }
}

/**
 * Cadastro: captura o frame NA HORA, depois pergunta o nome.
 */
void doEnroll() {
    String name = prompt("Nome:");

    Serial.println("Posicione o rosto. Cadastrando em 3s...");
    for (int i = 0; i < 30; i++) {          // mantem o feed vivo durante a espera
        if (camera.capture().isOk())
            publishFrame(camera.frame->buf, camera.frame->len);
        delay(100);
    }

    if (!camera.capture().isOk()) {
        Serial.println("ERRO: captura falhou");
        return;
    }
    publishFrame(camera.frame->buf, camera.frame->len);

    if (!recognition.detect().isOk()) {
        Serial.println("ERRO: nenhum rosto detectado");
        return;
    }

    if (recognition.enroll(name).isOk()) {
        Serial.print("OK, cadastrado: ");
        Serial.println(name);
    }
    else {
        Serial.println(recognition.exception.toString());
    }
}

void enrollMultiplo(int alvo) {
    String nome = prompt("Nome para cadastro multiplo:");   // <-- pergunta primeiro

    Serial.printf(">> Multi-enroll de '%s' (meta: %d capturas boas)\n", nome.c_str(), alvo);
    int ok         = 0;
    int tentativas = 0;
    const int MAX_TENTATIVAS = alvo * 6;

    while (ok < alvo) {
        if (tentativas >= MAX_TENTATIVAS) {
            Serial.printf(">> Desisti: so %d/%d salvas em %d tentativas\n",
                          ok, alvo, tentativas);
            return;
        }
        tentativas++;

        Serial.printf("   captura %d/%d (tentativa %d) - posicione o rosto...\n",
                      ok + 1, alvo, tentativas);

        for (int k = 0; k < 20; k++) {
            if (camera.capture().isOk())
                publishFrame(camera.frame->buf, camera.frame->len);
            delay(100);
        }

        if (!camera.capture().isOk()) { Serial.println("   captura falhou, repetindo"); continue; }
        publishFrame(camera.frame->buf, camera.frame->len);
        if (!recognition.detect().isOk()) { Serial.println("   sem rosto, repetindo"); continue; }

        if (recognition.enroll(nome).isOk()) {          // <-- usa a String direto
            ok++;
            Serial.printf("   OK (%d/%d boas)\n", ok, alvo);
        } else {
            Serial.println(recognition.exception.toString());
        }
    }

    Serial.printf(">> Multi-enroll concluido: %d/%d capturas boas para '%s'\n",
                  ok, alvo, nome.c_str());
}

String prompt(String message) {
    String answer;
    do {
        Serial.print(message);
        Serial.print(" ");
        while (!Serial.available())
            delay(1);
        answer = Serial.readStringUntil('\n');
        answer.trim();
    } while (answer.length() == 0);
    Serial.println(answer);
    return answer;
}