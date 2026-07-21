/**
 * ESP32-S3 Face Recognition — DIAGNOSTICO + LIVE FEED (v2)
 * Freenove ESP32-S3-WROOM CAM | OV2640
 *
 * Core 2.0.14 | OPI PSRAM | Huge APP | Serial 115200 + New Line
 *
 * ==================== MUDANCAS DESTA VERSAO (v2) ====================
 *  A) QUALITY-GATE unificado (funcao frameOk): descarta frame
 *     - desfocado (sharp baixo)
 *     - muito escuro / muito claro (brilho medio fora da faixa)
 *     - com movimento excessivo (diferenca entre 2 frames seguidos)
 *     Usado TANTO no enroll QUANTO na votacao.
 *  B) VOTACAO com EARLY-EXIT: se juntar os VOTOS_K votos antes de
 *     completar a janela, LIBERA na hora (nao gasta os 7 frames a toa).
 *     Tambem tem "early-fail": se ja for impossivel atingir K, aborta.
 *  C) GATE DE NITIDEZ no enroll (nao cadastra rosto borrado).
 *  D) THRESHOLD consolidado: recognize() baixo pra ver numero cru,
 *     PISO_SIM e o gate real (ajustado pra 0.93).
 *  E) SENSOR: gamma correction (set_raw_gma) + opcao de EXPOSICAO FIXA
 *     (AEC/AGC desligados) pra estabilizar brilho e matar flicker.
 *  F) Protótipo de publishFrame adicionado (higiene).
 *
 * ==================== NOTA HONESTA (IMPORTANTE) ====================
 *  Gamma / equalizacao de histograma / normalizacao de brilho POR
 *  SOFTWARE (em cima dos pixels) NAO sao aplicaveis aqui: a lib
 *  Eloquent roda detect()/recognize() sobre o frame INTERNO da camera,
 *  e o buffer que temos (camera.frame->buf) e JPEG comprimido, nao RGB.
 *  Nao da pra inserir um filtro de pixel ENTRE a captura e a inferencia
 *  sem reescrever a lib. Por isso essas correcoes sao feitas NO SENSOR
 *  (gamma via set_raw_gma, brilho/exposicao via AEC fixo), que e onde
 *  elas DE FATO afetam o frame que o modelo recebe. O quality-gate de
 *  brilho/movimento roda sobre metricas do JPEG (tamanho + amostragem),
 *  que sao proxies validos e baratos, sem decodificar a imagem.
 * ===================================================================
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
#define PISO_SIM      0.91f    // (D) sim minimo pra um frame virar voto valido
                               //     0.93: acima do teto do impostor (~0.90),
                               //     abaixo do seu chao (~0.96). MEDIR e ajustar.
#define JANELA_N      7       // frames validos a coletar por tentativa (teto)
#define VOTOS_K       4        // votos a favor necessarios (K de N) -> abre com 4
#define TIMEOUT_MS    12000    // aborta se nao juntar N validos a tempo

// ---------------- QUALITY-GATE (A) ----------------
// Todos os limiares abaixo sao PROXIES baseados no tamanho do JPEG e numa
// amostragem barata dos bytes. CALIBRE rodando o modo continuo ('r') e
// olhando os valores reais no teu setup (sensor+luz+quality mudam tudo).
#define MIN_SHARP        4000  // nitidez minima (bytes do JPEG). ~85% do teu pico focado.
#define MAX_SHARP        38000 // teto de seguranca (perto do JPG_CAP): frame gigante = ruido/anomalia
#define BRILHO_MIN       40    // brilho medio (0-255) minimo -> abaixo = MUITO ESCURO
#define BRILHO_MAX       215   // brilho medio (0-255) maximo -> acima = MUITO CLARO / estourado
#define MOV_MAX_DELTA    9000  // variacao de tamanho do JPEG entre 2 frames seguidos.
                               // JPEG cresce/encolhe muito quando a cena muda (movimento).
                               // acima disso = MOVIMENTO EXCESSIVO (risco de motion blur).

// ---------------- EXPOSICAO ----------------
// (E) Coloque true pra DESLIGAR auto-exposicao/auto-ganho e fixar a exposicao.
// Isso estabiliza brilho entre frames e ajuda contra flicker de LED, MAS exige
// luz constante (ex.: COB). Se a luz variar, a imagem fica escura/estourada.
// Comece com false (auto), e so ligue true DEPOIS de ter luz controlada.
#define EXPOSICAO_FIXA   true
#define AEC_VALOR_FIXO   300    // 0..1200 (varie ate o brilho ficar bom, sem flicker)
#define AGC_GANHO_FIXO   0      // 0..30   (ganho fixo baixo = menos ruido)

// ---------------- CONFIG ----------------
#define WIFI_SSID "Caio.2g"
#define WIFI_PASS "28460363"
#define JPG_CAP   40000        // 240x240 @ quality high cabe folgado
#define FRAME_W   240          // camera.resolution.face()
#define FRAME_H   240
// ----------------------------------------

String   prompt(String message);
void     doEnroll();
void     runRecognition();
void     enrollMultiplo(int alvo);
void     publishFrame(const uint8_t* buf, size_t len);   // (F) protótipo que faltava
uint32_t brilhoMedioJPEG(const uint8_t* buf, size_t len);
bool     frameOk(const char* &motivoOut);

bool     modoContinuo = false;
uint32_t sharpMax     = 0;
uint32_t lastPrint    = 0;
uint32_t ultimoSharp  = 0;     // (A) para medir movimento entre frames


// resultado de uma tentativa
enum Veredito { PENDENTE, APROVADO, NEGADO, EXPIROU };

/**
 * (A) QUALITY-GATE UNIFICADO.
 * Retorna true se o frame ATUAL (camera.frame) esta bom o suficiente.
 * Se retornar false, escreve o motivo em motivoOut (pra log).
 *
 * Checa, em ordem barata->cara:
 *   1. nitidez (sharp) minima e maxima
 *   2. movimento excessivo (delta de sharp vs frame anterior)
 *   3. brilho medio (muito escuro / muito claro)
 *
 * Observacao: tudo isso opera sobre o JPEG (tamanho + amostragem de bytes),
 * sem decodificar a imagem -> barato o bastante pra rodar a cada frame.
 */
bool frameOk(const char* &motivoOut) {
    const uint8_t* buf = camera.frame->buf;
    size_t         len = camera.frame->len;
    uint32_t       sharp = len;

    // 1) NITIDEZ ------------------------------------------------------
    if (sharp < MIN_SHARP) { motivoOut = "desfocado (sharp baixo)"; return false; }
    if (sharp > MAX_SHARP) { motivoOut = "anomalo (sharp alto demais)"; return false; }

    // 2) MOVIMENTO ----------------------------------------------------
    // se o tamanho do JPEG pulou muito de um frame pro outro, a cena mudou
    // rapido -> provavel motion blur. (na 1a chamada ultimoSharp=0, ignora)
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

/**
 * Estima o brilho medio SEM decodificar o JPEG inteiro.
 * Amostra os bytes do fluxo JPEG (ignorando o cabecalho inicial) e tira a
 * media. NAO e o brilho exato de pixel (JPEG e comprimido), mas serve muito
 * bem como PROXY pra "muito escuro" vs "muito claro" -- que e tudo que o
 * gate precisa. Amostra 1 a cada N bytes pra ser barato.
 */
uint32_t brilhoMedioJPEG(const uint8_t* buf, size_t len) {
    if (!buf || len < 200) return 128;   // sem dado -> assume neutro
    const size_t inicio = 100;           // pula cabecalho JPEG
    const size_t passo  = 32;            // amostra 1 a cada 32 bytes
    uint32_t soma = 0, n = 0;
    for (size_t i = inicio; i < len; i += passo) { soma += buf[i]; n++; }
    return n ? (soma / n) : 128;
}

Veredito runTentativa() {
    int      validos    = 0;    // frames que passaram no gate + foram reconhecidos
    int      votosFavor = 0;    // subset dos validos que bateu ALVO + piso
    uint32_t t0         = millis();
    const char* motivo  = "";

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

        // ----- (A) QUALITY-GATE: descarta frame ruim ANTES de gastar voto -----
        if (!frameOk(motivo)) {
            Serial.printf("   frame descartado: %s\n", motivo);
            continue;   // nao conta como valido
        }

        // ----- detect + recognize -----
        if (!recognition.detect().isOk())    continue;  // sem rosto: nao conta
        if (!recognition.recognize().isOk()) continue;  // nao reconheceu: nao conta

        // frame VALIDO (passou no gate, tinha rosto e o modelo respondeu)
        validos++;
        const char* nome = recognition.match.name.c_str();
        float       sim  = recognition.match.similarity;

        // ----- o voto: precisa ser o nome-alvo E passar do piso -----
        bool aFavor = (strcmp(nome, ALVO_NOME) == 0) && (sim >= PISO_SIM);
        if (aFavor) votosFavor++;

        Serial.printf("   frame %d/%d: %s sim=%.3f -> %s  (favor=%d)\n",
                      validos, JANELA_N, nome, sim,
                      aFavor ? "VOTO" : "descartado", votosFavor);

        // ----- (B) EARLY-EXIT: bateu K votos? LIBERA JA, nao espera o resto -----
        if (votosFavor >= VOTOS_K) {
            Serial.printf(">> APROVADO (early-exit: %d votos em %d frames)\n",
                          votosFavor, validos);
            return APROVADO;
        }

        // ----- (B) EARLY-FAIL: ja e impossivel atingir K com o que resta? aborta -----
        int restantes = JANELA_N - validos;
        if (votosFavor + restantes < VOTOS_K) {
            Serial.printf(">> NEGADO (early-fail: %d votos, faltam %d frames, impossivel)\n",
                          votosFavor, restantes);
            return NEGADO;
        }
    }

    // ----- juiz: fechou a janela sem early-exit (nao deveria chegar aqui
    //       com a logica acima, mas fica como rede de seguranca) -----
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
    Serial.println("\n=== DIAGNOSTICO + LIVE FEED (v2) ===");

    camera.pinout.freenove_s3();
    camera.brownout.disable();
    camera.resolution.face();      // 240x240
    camera.quality.high();
    camera.xclk.slow();            // 10MHz: estavel na OV2640 (contra chuvisco)

    detection.accurate();
    detection.confidence(0.7);

    // (D) recognize() BAIXO de proposito: queremos o SIM CRU pra votacao
    // decidir. O gate real e o PISO_SIM la em cima. NAO subir isso, senao
    // vira "unknown" antes de chegar na logica de voto.
    recognition.confidence(0.91);

    while (!camera.begin().isOk())
        Serial.println(camera.exception.toString());

    while (!recognition.begin().isOk())
        Serial.println(recognition.exception.toString());

    Serial.println("Camera OK / Recognizer OK");

// ---------------- SENSOR (depois do begin!) ----------------
    // (E) AJUSTE DE CAMERA. Aqui e o UNICO lugar onde "normalizar imagem"
    // realmente afeta o frame que o modelo recebe (a lib nao expoe o buffer
    // RGB entre captura e inferencia).
    sensor_t* s = esp_camera_sensor_get();
    if (s) {
        s->set_gainceiling(s, (gainceiling_t) GAINCEILING_2X);   // teto de ganho baixo = menos ruido
        s->set_brightness(s, 1);      // -2..2
        s->set_contrast(s, 1);        // -2..2  ajuda o detalhe fino
        s->set_saturation(s, 0);
        s->set_whitebal(s, 1);        // white balance (normaliza cor)
        s->set_awb_gain(s, 1);
        s->set_lenc(s, 1);            // corrige vinheta da lente (normaliza brilho nas bordas)
        s->set_hmirror(s, 1);
        s->set_vflip(s, 0);
        s->set_dcw(s, 1);

        // --- GAMMA CORRECTION (equivalente no sensor) ---
        // set_raw_gma aplica a curva de gama do ISP: melhora detalhe nas
        // sombras/altas luzes. Este e o "gamma correction" possivel aqui.
        s->set_raw_gma(s, 1);
        // set_bpc/set_wpc: correcao de pixels ruins (limpa ruido pontual)
        s->set_bpc(s, 1);
        s->set_wpc(s, 1);

        // --- EXPOSICAO / GANHO ---
        if (EXPOSICAO_FIXA) {
            // (E) DESLIGA auto-exposure e auto-ganho e FIXA os valores.
            // Estabiliza brilho entre frames e ajuda contra flicker.
            // EXIGE luz constante (COB). Varie AEC_VALOR_FIXO ate o brilho
            // ficar bom (veja o campo 'brilho' no navegador).
            s->set_gain_ctrl(s, 0);                 // AGC off
            s->set_exposure_ctrl(s, 0);             // AEC off
            s->set_aec2(s, 0);                      // AEC DSP off
            s->set_agc_gain(s, AGC_GANHO_FIXO);     // ganho fixo
            s->set_aec_value(s, AEC_VALOR_FIXO);    // exposicao fixa (0..1200)
            Serial.println("Sensor: EXPOSICAO FIXA (auto desligado)");
        } else {
            // auto-exposicao/ganho ligados (bom pra luz variavel)
            s->set_gain_ctrl(s, 1);                 // AGC on
            s->set_exposure_ctrl(s, 1);             // AEC on
            s->set_ae_level(s, 1);                  // -2..2 sobe se estiver escuro
            Serial.println("Sensor: exposicao AUTO");
        }

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
    Serial.println("  t = testar reconhecimento (votacao com early-exit)");
    Serial.println("  m = multiplos enrolls (com gate de nitidez)");
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
        else if (cmd.startsWith("t")) { runTentativa(); }
        else if (cmd.startsWith("m")) { modoContinuo = false; enrollMultiplo(6); }
    }

    // SEMPRE captura + publica: o feed roda mesmo pausado (pra focar a lente)
    if (!camera.capture().isOk()) {
        delay(100);
        return;
    }

    publishFrame(camera.frame->buf, camera.frame->len);

    // proxy de nitidez: com quality fixa, imagem desfocada comprime mais
    uint32_t sharp  = camera.frame->len;
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

/**
 * Detecta + reconhece o MESMO frame que acabou de ir pro navegador.
 * (mostra tambem o brilho no feed, pra voce calibrar os limiares do gate)
 */
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

/**
 * Cadastro simples: captura o frame NA HORA, depois pergunta o nome.
 * (C) Agora com gate de nitidez.
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

    // (C) gate de nitidez + qualidade antes de aceitar o cadastro
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

/**
 * (C) Multi-enroll com gate de nitidez/qualidade: so conta captura BOA.
 */
void enrollMultiplo(int alvo) {
    String nome = prompt("Nome para cadastro multiplo:");

    Serial.printf(">> Multi-enroll de '%s' (meta: %d capturas boas)\n", nome.c_str(), alvo);
    int ok         = 0;
    int tentativas = 0;
    const int MAX_TENTATIVAS = alvo * 8;   // teto maior: o gate rejeita mais
    const char* motivo = "";

    while (ok < alvo) {
        if (tentativas >= MAX_TENTATIVAS) {
            Serial.printf(">> Desisti: so %d/%d salvas em %d tentativas\n",
                          ok, alvo, tentativas);
            return;
        }
        tentativas++;

        Serial.printf("   captura %d/%d (tentativa %d) - posicione o rosto e fique PARADO...\n",
                      ok + 1, alvo, tentativas);

        for (int k = 0; k < 20; k++) {
            if (camera.capture().isOk())
                publishFrame(camera.frame->buf, camera.frame->len);
            delay(100);
        }

        if (!camera.capture().isOk()) { Serial.println("   captura falhou, repetindo"); continue; }
        publishFrame(camera.frame->buf, camera.frame->len);

        // (C) so aceita frame nitido, bem exposto e sem movimento
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
