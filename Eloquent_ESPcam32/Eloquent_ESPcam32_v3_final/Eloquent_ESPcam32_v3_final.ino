#include <eloquent_esp32cam.h>
#include <eloquent_esp32cam/face/detection.h>
#include <eloquent_esp32cam/face/recognition.h>

#include <WiFi.h>
#include <esp_http_server.h>
#include <math.h>

#include "esp_sleep.h"
#include "driver/rtc_io.h"

using eloq::camera;
using eloq::face::detection;
using eloq::face::recognition;

const int pinoVermelho = 1;
const int pinoVerde    = 2;
const int pinoAzul     = 47;   
const int pinoBuzzer   = 41;


const int iniciarReconhecimento = 21;   


const gpio_num_t pinoDeepSleep = GPIO_NUM_14;

struct Nota {
  int frequencia;
  int duracao;
};

const Nota somPortaAberta[] = {
  {523, 50}, {659, 50}, {784, 50}, {1047, 200},
  {0, 0}
};

const Nota somAlarme[] = {
  {2500, 300}, {2000, 300}, {2500, 300}, {2000, 300},
  {0, 0}
};

const Nota somBoot[] = {
  {1800, 80},   
  {0, 40},    
  {2300, 200}, 
  {0, 0}
};

const Nota somAcessoNegado[] = {
  {600, 150},   // ataque imediato
  {450, 150},   // transicao rapida
  {300, 300},   // finalizacao grave e seca
  {0, 0}
};

#define ALVO_NOME     "caio"  
#define PISO_SIM      0.92f   
#define JANELA_N      7      
#define VOTOS_K       4      
#define TIMEOUT_MS    12000    

#define MIN_SHARP        3000  // nitidez minima (bytes do JPEG)
#define MAX_SHARP        38000 // teto de seguranca: frame gigante = anomalia
#define BRILHO_MIN       40    // brilho medio (0-255) minimo -> MUITO ESCURO
#define BRILHO_MAX       215   // brilho medio (0-255) maximo -> MUITO CLARO
#define MOV_MAX_DELTA    9000  // variacao de tamanho do JPEG entre 2 frames

#define EXPOSICAO_FIXA   true
#define AEC_VALOR_FIXO   300    // 0..1200
#define AGC_GANHO_FIXO   0      // 0..30

// ---------------- CONFIG ----------------
#define WIFI_SSID "Caio.2g"
#define WIFI_PASS "28460363"
#define JPG_CAP   40000
#define FRAME_W   240
#define FRAME_H   240
// ----------------------------------------

// resultado de uma tentativa
// (fica AQUI EM CIMA: o Arduino IDE gera prototipos automaticos no topo
//  do arquivo e eles referenciam Veredito antes desta linha se o enum
//  ficar la embaixo -> erro "does not name a type")
enum Veredito { PENDENTE, APROVADO, NEGADO, EXPIROU };

String   prompt(String message);
String   promptTimeout(String message, uint32_t ms);   // (H)
void     entrarEmDeepSleep();                          // (G)
void     doEnroll();
void     runRecognition();
void     enrollMultiplo(int alvo);
void     publishFrame(const uint8_t* buf, size_t len);
uint32_t brilhoMedioJPEG(const uint8_t* buf, size_t len);
bool     frameOk(const char* &motivoOut);
void     pulsaLEDEspera();
void     sinalizaResultado(int pino, const Nota melodia[]);
Veredito runTentativa();
void     tocarMelodia(const Nota melodia[]);

bool     modoContinuo = false;
uint32_t sharpMax     = 0;
uint32_t lastPrint    = 0;
uint32_t ultimoSharp  = 0;     // (A) para medir movimento entre frames

void tocarMelodia(const Nota melodia[]) {
  int i = 0;
  while (melodia[i].frequencia != 0 || melodia[i].duracao != 0) {
    if (melodia[i].frequencia == 0) {
      noTone(pinoBuzzer);
      delay(melodia[i].duracao);
    } else {
      tone(pinoBuzzer, melodia[i].frequencia, melodia[i].duracao);
      delay(melodia[i].duracao + 10);
    }
    i++;
  }
  noTone(pinoBuzzer);
}

void entrarEmDeepSleep() {
    Serial.println(">> Entrando em DEEP SLEEP (abra o switch para acordar)");
    Serial.flush();   // garante que a UART terminou de enviar antes de dormir

    // apaga toda a sinalizacao
    digitalWrite(pinoVermelho, LOW);
    digitalWrite(pinoVerde, LOW);
    analogWrite(pinoAzul, 0);     // mata o PWM residual do pulsaLEDEspera
    digitalWrite(pinoAzul, LOW);
    noTone(pinoBuzzer);

    // pull-up do dominio RTC (o do pinMode nao sobrevive ao sono)
    rtc_gpio_pullup_en(pinoDeepSleep);
    rtc_gpio_pulldown_dis(pinoDeepSleep);

    // acorda quando o pino for a HIGH = switch aberto
    esp_sleep_enable_ext0_wakeup(pinoDeepSleep, 1);

    esp_deep_sleep_start();
}

void sinalizaResultado(int pino, const Nota melodia[]) {
    analogWrite(pinoAzul, 0);     // para o PWM do azul
    digitalWrite(pinoAzul, LOW);  // garante LOW
    digitalWrite(pino, HIGH);
    tocarMelodia(melodia);
    delay(3000);
    digitalWrite(pino, LOW);
}

void pulsaLEDEspera() {
    float onda   = (sin(millis() / 300.0) + 1) / 2;
    int   brilho = onda * 255;
    analogWrite(pinoAzul, brilho);
}

bool frameOk(const char* &motivoOut) {
    const uint8_t* buf   = camera.frame->buf;
    size_t         len   = camera.frame->len;
    uint32_t       sharp = len;

    // 1) NITIDEZ ------------------------------------------------------
    if (sharp < MIN_SHARP) { motivoOut = "desfocado (sharp baixo)";    return false; }
    if (sharp > MAX_SHARP) { motivoOut = "anomalo (sharp alto demais)"; return false; }

    // 2) MOVIMENTO ----------------------------------------------------
    if (ultimoSharp != 0) {
        uint32_t delta = (sharp > ultimoSharp) ? (sharp - ultimoSharp)
                                               : (ultimoSharp - sharp);
        if (delta > MOV_MAX_DELTA) {
            ultimoSharp = sharp;
            motivoOut = "movimento excessivo";
            return false;
        }
    }
    ultimoSharp = sharp;

    // 3) BRILHO -------------------------------------------------------
    uint32_t brilho = brilhoMedioJPEG(buf, len);
    if (brilho < BRILHO_MIN) { motivoOut = "muito escuro"; return false; }
    if (brilho > BRILHO_MAX) { motivoOut = "muito claro";  return false; }

    motivoOut = "ok";
    return true;
}

uint32_t brilhoMedioJPEG(const uint8_t* buf, size_t len) {
    if (!buf || len < 200) return 128;   // sem dado -> assume neutro
    const size_t inicio = 100;           // pula cabecalho JPEG
    const size_t passo  = 32;            // amostra 1 a cada 32 bytes
    uint32_t soma = 0, n = 0;
    for (size_t i = inicio; i < len; i += passo) { soma += buf[i]; n++; }
    return n ? (soma / n) : 128;
}

Veredito runTentativa() {
    int      validos    = 0;    // frames que passaram no gate + reconhecidos
    int      votosFavor = 0;    // subset dos validos que bateu ALVO + piso
    uint32_t t0         = millis();
    const char* motivo  = "";

    Serial.println(">> TENTATIVA iniciada");

    while (validos < JANELA_N) {

        pulsaLEDEspera();

        // ----- timeout: rosto nao aparece / some -----
        if (millis() - t0 > TIMEOUT_MS) {
            Serial.printf(">> EXPIROU (so %d/%d validos)\n", validos, JANELA_N);
            sinalizaResultado(pinoVermelho, somAcessoNegado);
            return EXPIROU;
        }

        // ----- 1 frame fresco -----
        if (!camera.capture().isOk()) { delay(20); continue; }
        publishFrame(camera.frame->buf, camera.frame->len);

        // ----- (A) QUALITY-GATE -----
        if (!frameOk(motivo)) {
            Serial.printf("   frame descartado: %s\n", motivo);
            continue;
        }

        // ----- detect + recognize -----
        if (!recognition.detect().isOk())    continue;
        if (!recognition.recognize().isOk()) continue;

        validos++;
        const char* nome = recognition.match.name.c_str();
        float       sim  = recognition.match.similarity;

        bool aFavor = (strcmp(nome, ALVO_NOME) == 0) && (sim >= PISO_SIM);
        if (aFavor) votosFavor++;

        Serial.printf("   frame %d/%d: %s sim=%.3f -> %s  (favor=%d)\n",
                      validos, JANELA_N, nome, sim,
                      aFavor ? "VOTO" : "descartado", votosFavor);

        // ----- (B) EARLY-EXIT -----
        if (votosFavor >= VOTOS_K) {
            Serial.printf(">> APROVADO (early-exit: %d votos em %d frames)\n",
                          votosFavor, validos);
            sinalizaResultado(pinoVerde, somPortaAberta);
            return APROVADO;
        }

        // ----- (B) EARLY-FAIL -----
        int restantes = JANELA_N - validos;
        if (votosFavor + restantes < VOTOS_K) {
            Serial.printf(">> NEGADO (early-fail: %d votos, faltam %d frames)\n",
                          votosFavor, restantes);
            sinalizaResultado(pinoVermelho, somAcessoNegado);
            return NEGADO;
        }
    }

    // ----- rede de seguranca -----
    if (votosFavor >= VOTOS_K) {
        Serial.printf(">> APROVADO (%d/%d votos)\n", votosFavor, validos);
        sinalizaResultado(pinoVerde, somPortaAberta);
        return APROVADO;
    }

    Serial.printf(">> NEGADO (%d/%d votos, precisava %d)\n",
                  votosFavor, validos, VOTOS_K);
    sinalizaResultado(pinoVermelho, somAcessoNegado);
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
      'brilho    : ' + d.bri                                     + '\n' +
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
    Serial.println("\n=== DIAGNOSTICO + LIVE FEED (v3) ===");

    // (G) informa POR QUE a placa ligou: boot normal ou volta do sono
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0)
        Serial.println(">> Acordei do DEEP SLEEP (switch aberto)");
    else
        Serial.println(">> Boot normal (energia/reset)");

    pinMode(pinoVermelho, OUTPUT);
    pinMode(pinoVerde, OUTPUT);
    pinMode(pinoAzul, OUTPUT);
    pinMode(pinoBuzzer, OUTPUT);
    pinMode(iniciarReconhecimento, INPUT_PULLUP);
    pinMode(pinoDeepSleep, INPUT_PULLUP);

    // (G) libera o hold do RTC no pino do switch. Sem isso, dependendo do
    // core, o pino pode continuar "congelado" no estado em que dormiu.
    rtc_gpio_deinit(pinoDeepSleep);
    pinMode(pinoDeepSleep, INPUT_PULLUP);

    camera.pinout.freenove_s3();
    camera.brownout.disable();
    camera.resolution.face();      // 240x240
    camera.quality.high();
    camera.xclk.slow();            // 10MHz: estavel na OV2640

    detection.accurate();
    detection.confidence(0.7);

    recognition.confidence(0.85);

    while (!camera.begin().isOk())
        Serial.println(camera.exception.toString());

    while (!recognition.begin().isOk())
        Serial.println(recognition.exception.toString());

    Serial.println("Camera OK / Recognizer OK");

    // ---------------- SENSOR (depois do begin!) ----------------
    sensor_t* s = esp_camera_sensor_get();
    if (s) {
        s->set_hmirror(s, 1);
        s->set_gainceiling(s, (gainceiling_t) GAINCEILING_2X);
        s->set_brightness(s, 1);      // -2..2
        s->set_contrast(s, 1);        // -2..2
        s->set_saturation(s, 0);
        s->set_whitebal(s, 1);
        s->set_awb_gain(s, 1);
        s->set_lenc(s, 1);            // corrige vinheta da lente
        s->set_vflip(s, 0);
        s->set_dcw(s, 1);

        // --- GAMMA CORRECTION ---
        s->set_raw_gma(s, 1);
        s->set_bpc(s, 1);
        s->set_wpc(s, 1);

        // --- EXPOSICAO / GANHO ---
        if (EXPOSICAO_FIXA) {
            s->set_gain_ctrl(s, 0);                 // AGC off
            s->set_exposure_ctrl(s, 0);             // AEC off
            s->set_aec2(s, 0);                      // AEC DSP off
            s->set_agc_gain(s, AGC_GANHO_FIXO);
            s->set_aec_value(s, AEC_VALOR_FIXO);
            Serial.println("Sensor: EXPOSICAO FIXA (auto desligado)");
        } else {
            s->set_gain_ctrl(s, 1);
            s->set_exposure_ctrl(s, 1);
            s->set_ae_level(s, 1);
            Serial.println("Sensor: exposicao AUTO");
        }

        Serial.println("Sensor tunado");
    }
    else Serial.println("AVISO: sensor_get falhou");

    // ---- buffer + rede ----
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

    tocarMelodia(somBoot);

    while (Serial.available()) Serial.read();

    if (promptTimeout("Apagar cadastros? [s|n] (5s)", 5000).startsWith("s")) {
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
    Serial.println("  t = testar reconhecimento (votacao com early-exit)");
    Serial.println("  m = multiplos enrolls (com gate de nitidez)");
    Serial.println("  s = dormir agora (deep sleep por software)");
    Serial.println();
    Serial.println("PUSH BUTTON GPIO14: aperte para iniciar reconhecimento");
    Serial.println("SWITCH GPIO14: fechado = dorme | aberto = acorda");
    Serial.println();
}

void loop() {

    // (G) switch FECHADO (pino em LOW) = dormir
    if (digitalRead(pinoDeepSleep) == LOW) {
        delay(50);                                  // debounce
        if (digitalRead(pinoDeepSleep) == LOW)
            entrarEmDeepSleep();                    // nao retorna
    }

    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();

        if      (cmd.startsWith("c")) { modoContinuo = false; doEnroll(); }
        else if (cmd.startsWith("r")) { modoContinuo = true;  Serial.println(">> MODO CONTINUO"); }
        else if (cmd.startsWith("p")) { modoContinuo = false; Serial.println(">> PAUSADO (feed ativo)"); }
        else if (cmd.startsWith("d")) { recognition.dump(); }
        else if (cmd.startsWith("z")) { sharpMax = 0; Serial.println(">> pico zerado"); }
        else if (cmd.startsWith("t")) { runTentativa(); }
        else if (cmd.startsWith("m")) { modoContinuo = false; enrollMultiplo(6); }
        else if (cmd.startsWith("s")) { entrarEmDeepSleep(); }
    }

    // --- botao fisico: dispara UMA vez por aperto (borda de descida) ---
    static bool botaoUltimoEstado = HIGH;   // com INPUT_PULLUP, solto = HIGH
    bool botaoAtual = digitalRead(iniciarReconhecimento);

    if (botaoUltimoEstado == HIGH && botaoAtual == LOW) {
        runTentativa();
    }
    botaoUltimoEstado = botaoAtual;

    // SEMPRE captura + publica: o feed roda mesmo pausado (pra focar a lente)
    if (!camera.capture().isOk()) {
        delay(100);
        return;
    }

    publishFrame(camera.frame->buf, camera.frame->len);

    // proxy de nitidez: com quality fixa, imagem desfocada comprime mais
    uint32_t sharp = camera.frame->len;
    if (sharp > sharpMax) sharpMax = sharp;
    uint32_t brilho = brilhoMedioJPEG(camera.frame->buf, camera.frame->len);

    if (modoContinuo)
        runRecognition();
    else
        snprintf(g_info, sizeof(g_info),
                 "{\"face\":0,\"sharp\":%u,\"peak\":%u,\"bri\":%u,\"name\":\"(pausado)\",\"sim\":\"-\"}",
                 (unsigned) sharp, (unsigned) sharpMax, (unsigned) brilho);

    delay(10);
}

void runRecognition() {
    uint32_t sharp  = camera.frame->len;
    uint32_t brilho = brilhoMedioJPEG(camera.frame->buf, camera.frame->len);

    if (!recognition.detect().isOk()) {
        snprintf(g_info, sizeof(g_info),
                 "{\"face\":0,\"sharp\":%u,\"peak\":%u,\"bri\":%u,\"name\":\"-\",\"sim\":\"-\"}",
                 (unsigned) sharp, (unsigned) sharpMax, (unsigned) brilho);
        return;
    }

    if (!recognition.recognize().isOk()) {
        snprintf(g_info, sizeof(g_info),
                 "{\"face\":1,\"sharp\":%u,\"peak\":%u,\"bri\":%u,\"name\":\"?\",\"sim\":\"-\"}",
                 (unsigned) sharp, (unsigned) sharpMax, (unsigned) brilho);
        return;
    }

    const char* name = recognition.match.name.c_str();
    float       sim  = recognition.match.similarity;

    snprintf(g_info, sizeof(g_info),
             "{\"face\":1,\"sharp\":%u,\"peak\":%u,\"bri\":%u,\"name\":\"%s\",\"sim\":\"%.4f\"}",
             (unsigned) sharp, (unsigned) sharpMax, (unsigned) brilho, name, sim);

    // throttle do serial (nao trava o stream)
    if (millis() - lastPrint > 400) {
        lastPrint = millis();
        Serial.printf("MATCH: %-10s sim=%.4f  sharp=%u  bri=%u  (%dms)\n",
                      name, sim, (unsigned) sharp, (unsigned) brilho,
                      recognition.benchmark.millis());
    }
}

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

    const char* motivo = "";
    if (!frameOk(motivo)) {
        Serial.printf("ERRO: frame ruim para cadastro (%s). Tente de novo.\n", motivo);
        return;
    }

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
    String nome = prompt("Nome para cadastro multiplo:");

    Serial.printf(">> Multi-enroll de '%s' (meta: %d capturas boas)\n", nome.c_str(), alvo);
    int ok         = 0;
    int tentativas = 0;
    const int MAX_TENTATIVAS = alvo * 8;
    const char* motivo = "";

    while (ok < alvo) {
        if (tentativas >= MAX_TENTATIVAS) {
            Serial.printf(">> Desisti: so %d/%d salvas em %d tentativas\n",
                          ok, alvo, tentativas);
            analogWrite(pinoAzul, 0);
            return;
        }
        tentativas++;

        Serial.printf("   captura %d/%d (tentativa %d) - posicione o rosto e fique PARADO...\n",
                      ok + 1, alvo, tentativas);

        for (int k = 0; k < 20; k++) {
            if (camera.capture().isOk()) {
                publishFrame(camera.frame->buf, camera.frame->len);
            }

            // loop de 100ms atualizando o LED (em vez de delay(100) seco)
            unsigned long tEspera = millis();
            while (millis() - tEspera < 100) {
                pulsaLEDEspera();
                delay(5);
            }
        }

        if (!camera.capture().isOk()) { Serial.println("   captura falhou, repetindo"); continue; }
        publishFrame(camera.frame->buf, camera.frame->len);

        if (!frameOk(motivo)) {
            Serial.printf("   descartei (%s), repetindo\n", motivo);
            continue;
        }

        if (!recognition.detect().isOk()) { Serial.println("   sem rosto, repetindo"); continue; }

        if (recognition.enroll(nome).isOk()) {
            ok++;
            Serial.printf("   OK (%d/%d boas)\n", ok, alvo);
        } else {
            Serial.println(recognition.exception.toString());
        }
    }

    Serial.printf(">> Multi-enroll concluido: %d/%d capturas boas para '%s'\n",
                  ok, alvo, nome.c_str());
    analogWrite(pinoAzul, 0);
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

String promptTimeout(String message, uint32_t ms) {
    Serial.print(message);
    Serial.print(" ");

    uint32_t t0 = millis();
    while (!Serial.available()) {
        if (millis() - t0 > ms) {
            Serial.println("  (sem resposta, seguindo)");
            return "";
        }
        delay(10);
    }

    String answer = Serial.readStringUntil('\n');
    answer.trim();
    Serial.println(answer);
    return answer;
}
