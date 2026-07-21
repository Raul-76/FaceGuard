#include <eloquent_esp32cam.h>
#include <eloquent_esp32cam/face/detection.h>
#include <eloquent_esp32cam/face/recognition.h>

#include <WiFi.h>
#include <esp_http_server.h>   // servidor HTTP nativo do IDF (mais leve que WebServer.h)
#include <math.h>              // sin() do pulso do LED

#include "esp_sleep.h"         // deep sleep
#include "driver/rtc_io.h"     // pull-up no dominio RTC (sobrevive ao sono)

using eloq::camera;
using eloq::face::detection;
using eloq::face::recognition;

// ==================== PINOS ====================
// LED azul saiu do GPIO14 (que virou o switch de sono) e foi pro 47.
// (K) LED VERDE saiu do GPIO2 e foi pro 48, liberando o 2 para o LDR.
const int pinoVermelho = 1;
const int pinoVerde    = 48;   // (K) ERA 2 -- movido para liberar o ADC1
const int pinoAzul     = 47;
const int pinoBuzzer   = 41;
const int pinoLuz      = 42;   // gate do MOSFET que aciona o COB

// (J) FOTORRESISTOR: GPIO 2 = ADC1_CH1.
// TEM que ser ADC1 (GPIO 1..10 no S3): o ADC2 nao funciona com WiFi ligado.
const int pinoLDR      = 2;

// ==================== CONTROLE DE ILUMINACAO (J) ====================
// A realimentacao agora usa o LDR. Divisor de tensao sugerido:
//     3.3V --- [LDR] ---+--- GPIO2 (ADC1_CH1)
//                       |
//                    [10k]
//                       |
//                      GND
// Mais luz -> menor resistencia do LDR -> LEITURA MAIOR no ADC.
//
// CALIBRACAO OBRIGATORIA: rode o comando 'l' e anote a leitura crua no seu
// ambiente. Os valores abaixo sao ponto de partida, nao verdade absoluta --
// dependem do LDR, do resistor e da luz do local.
const int LDR_ALVO     = 2000;  // leitura desejada (0..4095)
const int BANDA_MORTA  = 150;   // nao mexe se estiver perto do alvo.
                                // Sem banda morta o controle OSCILA em torno
                                // do setpoint, e luz piscando estraga a
                                // consistencia dos embeddings.
const int DUTY_MIN     = 1;    // nunca apaga de vez durante a operacao
const int DUTY_MAX     = 255;
const int PASSO_DUTY   = 2;     // ajuste INCREMENTAL, nao proporcional:
                                // mover pouco por vez tambem evita oscilacao
int dutyLuz = 30;              // ponto de partida

// Botao momentaneo que dispara uma tentativa de reconhecimento.
const int iniciarReconhecimento = 21;

// Switch de trava do deep sleep. TEM que ser RTC GPIO (0..21 no S3):
// so o dominio RTC fica vivo dormindo, entao so esses pinos acordam.
const gpio_num_t pinoDeepSleep = GPIO_NUM_14;

// ==================== BUZZER ====================
// Cada som e um vetor de pares {frequencia_Hz, duracao_ms}, terminado
// por {0,0}. Frequencia 0 com duracao > 0 = pausa (silencio).
struct Nota {
  int frequencia;
  int duracao;
};

// Logica sonora do sistema: SOBE = positivo, DESCE = negativo.
// E o mesmo vocabulario de leitoras HID/Intelbras.

// Acesso permitido: arpejo ascendente ("liberado").
const Nota somPortaAberta[] = {
  {523, 50}, {659, 50}, {784, 50}, {1047, 200},
  {0, 0}
};

// Alarme/tamper: alternancia aguda e estridente (padrao de alerta).
const Nota somAlarme[] = {
  {2500, 300}, {2000, 300}, {2500, 300}, {2000, 300},
  {0, 0}
};

// Boot: bipe curto + bipe mais alto = "sistema pronto".
const Nota somBoot[] = {
  {1800, 80},
  {0, 40},
  {2300, 200},
  {0, 0}
};

// Acesso negado: descendente e grave. Seco, e inconfundivel com o de
// abertura justamente porque desce e termina no registro grave.
const Nota somAcessoNegado[] = {
  {600, 150},   // ataque imediato
  {450, 150},   // transicao rapida
  {300, 300},   // finalizacao grave e seca
  {0, 0}
};

// ==================== VOTACAO POR RAJADA ====================
#define ALVO_NOME     "caio"   // unico nome autorizado a abrir
#define PISO_SIM      0.92f    // similaridade minima pra um frame virar VOTO.
                               // Este e o gate REAL de seguranca -- deve ficar
                               // acima do teto observado do impostor e abaixo
                               // do chao observado do dono. MEDIR e ajustar.
#define JANELA_N      7        // teto de frames validos por tentativa
#define VOTOS_K       4        // votos a favor necessarios (K de N)
#define TIMEOUT_MS    12000    // aborta se nao juntar N validos a tempo

// ==================== QUALITY-GATE ====================
// (I) Os limiares de BRILHO foram removidos -- ver nota no cabecalho.
// Sobraram os dois que se apoiam em relacao fisica real com o tamanho do JPEG.
#define MIN_SHARP        3000  // abaixo disso = desfocado. Com quality fixa,
                               // imagem borrada comprime mais -> JPEG menor.
#define MAX_SHARP        38000 // teto: frame gigante = ruido/anomalia
#define MOV_MAX_DELTA    9000  // salto de tamanho do JPEG entre 2 frames.
                               // Cena mudando rapido -> risco de motion blur.

// ==================== EXPOSICAO ====================
// Com exposicao fixa o brilho para de variar entre frames, o que estabiliza
// os embeddings e mata flicker. EXIGE luz constante (COB); e justamente por
// isso o controle do COB via LDR faz sentido: ele mantem a cena estavel.
#define EXPOSICAO_FIXA   true
#define AEC_VALOR_FIXO   300    // 0..1200
#define AGC_GANHO_FIXO   0      // 0..30 (ganho baixo = menos ruido)

// ==================== CONFIG ====================
#define WIFI_SSID "Caio.2g"
#define WIFI_PASS "28460363"
#define JPG_CAP   40000        // teto do buffer compartilhado do stream
#define FRAME_W   240          // camera.resolution.face()
#define FRAME_H   240

// Resultado de uma tentativa.
// FICA AQUI EM CIMA de proposito: o Arduino IDE injeta prototipos
// automaticos no topo do arquivo. Se o enum estiver la embaixo, o prototipo
// de runTentativa() referencia Veredito antes da declaracao
// -> erro "'Veredito' does not name a type".
enum Veredito { PENDENTE, APROVADO, NEGADO, EXPIROU };

// Prototipos explicitos (nao dependemos da geracao automatica do IDE).
String   prompt(String message);
String   promptTimeout(String message, uint32_t ms);
void     entrarEmDeepSleep();
void     enrollMultiplo(int alvo);
void     publishFrame(const uint8_t* buf, size_t len);
bool     frameOk(const char* &motivoOut);
void     pulsaLEDEspera();
void     sinalizaResultado(int pino, const Nota melodia[]);
Veredito runTentativa();
void     tocarMelodia(const Nota melodia[]);
uint16_t lerLDR();
void     ajustaLuz();
void     testeLuz();

bool     modoContinuo = false; // 'r' liga: imprime similaridade a cada frame
uint32_t sharpMax     = 0;     // pico de nitidez ja visto (guia pra focar a lente)
uint32_t lastPrint    = 0;     // throttle do serial no modo continuo
uint32_t ultimoSharp  = 0;     // tamanho do frame anterior (detector de movimento)

/**
 * Toca a melodia de forma BLOQUEANTE (usa delay).
 * Aceitavel aqui porque os sons so tocam em transicoes de estado, nunca
 * durante a captura. O +10ms separa as notas e deixa o bipe "seco".
 */
void tocarMelodia(const Nota melodia[]) {
  int i = 0;
  while (melodia[i].frequencia != 0 || melodia[i].duracao != 0) {
    if (melodia[i].frequencia == 0) {
      noTone(pinoBuzzer);
      delay(melodia[i].duracao);      // pausa
    } else {
      tone(pinoBuzzer, melodia[i].frequencia, melodia[i].duracao);
      delay(melodia[i].duracao + 10);
    }
    i++;
  }
  noTone(pinoBuzzer);
}

/**
 * (J) Le o LDR com media de 5 amostras.
 * O ADC do ESP32 e ruidoso amostra a amostra; a media reduz isso sem custo
 * relevante. Retorna 0..4095 -- valor MAIOR significa MAIS luz (ver o
 * divisor de tensao documentado la em cima).
 */
uint16_t lerLDR() {
    uint32_t soma = 0;
    for (int i = 0; i < 5; i++) {
        soma += analogRead(pinoLDR);
        delayMicroseconds(200);
    }
    return soma / 5;
}

/**
 * (J) Controle de iluminacao realimentado pelo LDR.
 *
 * Ajuste INCREMENTAL com BANDA MORTA, de proposito: calcular o duty "ideal"
 * de uma vez cria um laco luz -> sensor -> duty -> luz que oscila, e luz
 * piscando gera embeddings inconsistentes -- exatamente o problema que o
 * sistema tenta combater.
 *
 * ATENCAO NA MONTAGEM: o LDR precisa enxergar a luz que ILUMINA O ROSTO, nao
 * a face do COB. Se ele receber a luz direta do LED, o laco realimenta a si
 * mesmo e o controle perde o sentido. Posicione-o voltado para onde a pessoa
 * fica, com anteparo bloqueando a visao direta do COB.
 */
void ajustaLuz() {
    uint16_t luz  = lerLDR();
    int      erro = LDR_ALVO - (int) luz;

    if (abs(erro) <= BANDA_MORTA) return;   // dentro da banda: nao mexe

    // erro > 0 -> esta escuro -> sobe o duty
    dutyLuz += (erro > 0) ? PASSO_DUTY : -PASSO_DUTY;
    dutyLuz  = constrain(dutyLuz, DUTY_MIN, DUTY_MAX);

    analogWrite(pinoLuz, dutyLuz);
}

/**
 * (J) Teste de MALHA ABERTA: forca duty conhecidos e mede a resposta do LDR.
 * Ignora o controlador de proposito -- e isso que revela se o conjunto
 * COB + MOSFET + LDR realmente responde. Use tambem pra escolher o LDR_ALVO:
 * rode o teste e escolha um valor intermediario da faixa observada.
 */
void testeLuz() {
    Serial.println("\n>> TESTE DE LUZ (malha aberta)");
    Serial.println("   duty  ->  leitura do LDR");

    const int niveis[] = {0, 32, 64, 128, 192, 255};

    for (int i = 0; i < 6; i++) {
        analogWrite(pinoLuz, niveis[i]);
        delay(500);   // o LDR tem inercia: precisa de tempo pra estabilizar
        Serial.printf("   %4d  ->  %u\n", niveis[i], (unsigned) lerLDR());
    }

    analogWrite(pinoLuz, 0);
    delay(500);
    Serial.printf("   luz ambiente (COB apagado): %u\n", (unsigned) lerLDR());
    Serial.println(">> Fim. Se a leitura nao subir com o duty, e hardware:");
    Serial.println("   MOSFET nao logic-level, GND nao comum, ou LDR mal ligado.\n");
}

/**
 * Apaga tudo, arma o despertador e dorme. NAO RETORNA: ao acordar, a
 * placa reinicia pelo setup() (a RAM do dominio digital foi perdida).
 */
void entrarEmDeepSleep() {
    Serial.println(">> Entrando em DEEP SLEEP (abra o switch para acordar)");
    Serial.flush();   // sem isso o chip dorme antes da UART terminar de enviar

    digitalWrite(pinoVermelho, LOW);
    digitalWrite(pinoVerde, LOW);
    analogWrite(pinoAzul, 0);      // mata o PWM residual do pulsaLEDEspera
    digitalWrite(pinoAzul, LOW);   // garante nivel logico LOW
    analogWrite(pinoLuz, 0);       // apaga o COB
    noTone(pinoBuzzer);

    // O pull-up do pinMode() pertence ao dominio digital, que e DESLIGADO
    // no deep sleep. Sem o pull-up do dominio RTC o pino fica flutuando e
    // a placa acorda sozinha por ruido.
    rtc_gpio_pullup_en(pinoDeepSleep);
    rtc_gpio_pulldown_dis(pinoDeepSleep);

    // ext0: acorda quando o pino for a HIGH, ou seja, quando o switch ABRIR.
    esp_sleep_enable_ext0_wakeup(pinoDeepSleep, 1);

    esp_deep_sleep_start();
}

/**
 * Encerra a tentativa: apaga azul + COB e sinaliza o veredito.
 * A dupla analogWrite(0) + digitalWrite(LOW) e necessaria: so parar o PWM
 * pode deixar brilho residual no azul, que se mistura com o vermelho/verde
 * e da cor errada.
 * Fica aqui (e nao em cada return do runTentativa) porque esta funcao e
 * chamada em TODAS as cinco saidas da tentativa.
 */
void sinalizaResultado(int pino, const Nota melodia[]) {
    analogWrite(pinoAzul, 0);
    digitalWrite(pinoAzul, LOW);
    analogWrite(pinoLuz, 0);      // apaga a iluminacao ao encerrar
    digitalWrite(pino, HIGH);
    tocarMelodia(melodia);
    delay(2500);                  // mantem a cor visivel (simula porta aberta)
    digitalWrite(pino, LOW);
}

/**
 * Respiracao do LED azul enquanto processa. Nao bloqueia: le millis() e
 * escreve o duty na hora, entao precisa ser chamada repetidamente dentro
 * do laco de quem estiver trabalhando.
 */
void pulsaLEDEspera() {
    float onda      = (sin(millis() / 300.0) + 1) / 2;   // normaliza -1..1 para 0..1
    int   brilhoLED = onda * 255;
    analogWrite(pinoAzul, brilhoLED);
}

/**
 * QUALITY-GATE: decide se o frame atual merece inferencia.
 * (I) So restaram nitidez e movimento -- os dois criterios que tem relacao
 * fisica comprovada com o tamanho do JPEG. O criterio de brilho foi removido
 * por nao medir o que dizia medir.
 * Escreve o motivo da rejeicao em motivoOut (passado por referencia).
 */
bool frameOk(const char* &motivoOut) {
    uint32_t sharp = camera.frame->len;   // o tamanho do JPEG E o proxy de nitidez

    // 1) NITIDEZ
    if (sharp < MIN_SHARP) { motivoOut = "desfocado (sharp baixo)";     return false; }
    if (sharp > MAX_SHARP) { motivoOut = "anomalo (sharp alto demais)"; return false; }

    // 2) MOVIMENTO: JPEG muda muito de tamanho quando a cena muda rapido.
    //    Na 1a chamada ultimoSharp = 0 e o teste e pulado.
    if (ultimoSharp != 0) {
        uint32_t delta = (sharp > ultimoSharp) ? (sharp - ultimoSharp)
                                               : (ultimoSharp - sharp);
        if (delta > MOV_MAX_DELTA) {
            ultimoSharp = sharp;   // atualiza antes de sair, senao a proxima
                                   // comparacao usaria referencia velha
            motivoOut = "movimento excessivo";
            return false;
        }
    }
    ultimoSharp = sharp;

    motivoOut = "ok";
    return true;
}

/**
 * UMA tentativa de acesso: coleta frames validos e conta votos.
 *
 * "valido"  = passou no quality-gate, tinha rosto e o modelo respondeu
 * "voto"    = valido E bateu o nome-alvo E sim >= PISO_SIM
 *
 * Sai cedo em dois casos:
 *   EARLY-EXIT: juntou K votos -> libera na hora, nao gasta os 7 frames
 *   EARLY-FAIL: mesmo acertando todos os frames restantes nao da K -> aborta
 */
Veredito runTentativa() {
    int      validos    = 0;
    int      votosFavor = 0;
    uint32_t t0         = millis();
    const char* motivo  = "";

    Serial.println(">> TENTATIVA iniciada");

    analogWrite(pinoLuz, dutyLuz);   // acende no duty aprendido

    while (validos < JANELA_N) {

        pulsaLEDEspera();   // precisa ser chamada no laco pra "respirar"

        // Timeout: rosto nao apareceu ou sumiu no meio.
        if (millis() - t0 > TIMEOUT_MS) {
            Serial.printf(">> EXPIROU (so %d/%d validos)\n", validos, JANELA_N);
            sinalizaResultado(pinoVermelho, somAcessoNegado);
            return EXPIROU;
        }

        // Frame fresco. A lib gerencia o buffer sozinha (nao ha fb_return).
        if (!camera.capture().isOk()) { delay(20); continue; }
        publishFrame(camera.frame->buf, camera.frame->len);  // mantem o feed vivo

        // Controle de luz ANTES do gate: o LDR independe do frame, e se o
        // gate rejeitasse primeiro o controle nunca agiria quando a cena
        // estivesse ruim -- justamente quando ele e necessario.
        ajustaLuz();

        // Descarta frame ruim ANTES de gastar inferencia.
        if (!frameOk(motivo)) {
            Serial.printf("   frame descartado: %s\n", motivo);
            continue;   // nao conta como valido
        }

        // Nenhum dos dois conta como valido se falhar: sem rosto na cena
        // ou modelo sem resposta nao sao "voto contra", sao "nada".
        if (!recognition.detect().isOk())    continue;
        if (!recognition.recognize().isOk()) continue;

        validos++;
        const char* nome = recognition.match.name.c_str();
        float       sim  = recognition.match.similarity;

        // O voto exige AS DUAS coisas: nome certo e similaridade acima do piso.
        bool aFavor = (strcmp(nome, ALVO_NOME) == 0) && (sim >= PISO_SIM);
        if (aFavor) votosFavor++;

        Serial.printf("   frame %d/%d: %s sim=%.3f -> %s  (favor=%d)\n",
                      validos, JANELA_N, nome, sim,
                      aFavor ? "VOTO" : "descartado", votosFavor);

        // EARLY-EXIT
        if (votosFavor >= VOTOS_K) {
            Serial.printf(">> APROVADO (early-exit: %d votos em %d frames)\n",
                          votosFavor, validos);
            sinalizaResultado(pinoVerde, somPortaAberta);
            return APROVADO;
        }

        // EARLY-FAIL: aritmetica simples, ja nao da mais pra atingir K.
        int restantes = JANELA_N - validos;
        if (votosFavor + restantes < VOTOS_K) {
            Serial.printf(">> NEGADO (early-fail: %d votos, faltam %d frames)\n",
                          votosFavor, restantes);
            sinalizaResultado(pinoVermelho, somAcessoNegado);
            return NEGADO;
        }
    }

    // Rede de seguranca: com os early-exit/fail acima o fluxo nao deveria
    // chegar aqui, mas se chegar, o juiz decide pela contagem final.
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

// ==================== BUFFER COMPARTILHADO DO STREAM ====================
// O loop principal escreve aqui; a task do servidor HTTP le. Sao contextos
// diferentes (FreeRTOS), por isso o mutex.
static uint8_t*          g_jpg     = nullptr;   // copia do ultimo frame (PSRAM)
static size_t            g_jpgLen  = 0;
static volatile uint32_t g_frameId = 0;         // contador: sinaliza frame novo
static SemaphoreHandle_t g_mutex   = nullptr;
static char              g_info[220] = "{\"face\":0,\"sharp\":0,\"peak\":0,\"ldr\":0,\"name\":\"-\",\"sim\":\"-\"}";
static httpd_handle_t    g_server  = nullptr;

/**
 * Copia o frame pro buffer compartilhado.
 * Usa timeout curto no mutex: se o servidor estiver ocupado, PERDE o frame
 * em vez de travar o loop de captura. Preferivel perder feed a travar a
 * fechadura.
 */
void publishFrame(const uint8_t* buf, size_t len) {
    if (!g_jpg || !buf || len == 0 || len > JPG_CAP) return;
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
    memcpy(g_jpg, buf, len);
    g_jpgLen = len;
    g_frameId++;
    xSemaphoreGive(g_mutex);
}

// ==================== HTTP ====================

/** /info -> JSON com as metricas. no-store pro navegador nao cachear. */
static esp_err_t infoHandler(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, g_info, HTTPD_RESP_USE_STRLEN);
}

/**
 * /stream -> MJPEG (multipart/x-mixed-replace): sequencia infinita de JPEGs
 * separados por um boundary. O <img> do navegador entende nativamente.
 * Copia pra um buffer local antes de enviar, pra soltar o mutex rapido e
 * nao segurar o loop de captura durante a transmissao (que e lenta).
 */
static esp_err_t streamHandler(httpd_req_t* req) {
    uint8_t* local = (uint8_t*) ps_malloc(JPG_CAP);
    if (!local) return ESP_FAIL;

    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=frame");
    uint32_t lastId = 0;
    char part[96];

    while (true) {
        size_t len = 0;
        if (xSemaphoreTake(g_mutex, portMAX_DELAY) == pdTRUE) {
            // So copia se houver frame NOVO (evita reenviar o mesmo).
            if (g_frameId != lastId && g_jpgLen) {
                memcpy(local, g_jpg, g_jpgLen);
                len    = g_jpgLen;
                lastId = g_frameId;
            }
            xSemaphoreGive(g_mutex);
        }
        if (!len) { delay(10); continue; }   // nada novo: espera

        // Qualquer falha de envio = cliente fechou a aba -> sai do laco.
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

// Pagina de debug: imagem + miras de centralizacao + area segura tracejada
// + painel de metricas atualizado a cada 250ms via /info.
// PROGMEM mantem a string na flash, sem gastar RAM.
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
      'LDR       : ' + d.ldr + '   (0-4095)'                     + '\n' +
      'match     : ' + d.name + '   sim=' + d.sim;
  } catch(e) {}
}, 250);
</script>
)HTML";

static esp_err_t indexHandler(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

/** Sobe o httpd com stack maior que o padrao (o stream precisa). */
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

// ==================== SETUP ====================
void setup() {
    delay(2000);                  // da tempo do monitor serial conectar
    Serial.begin(115200);
    Serial.println("\n=== DIAGNOSTICO + LIVE FEED (v4) ===");

    // Distingue boot por energia de retorno do deep sleep. Util pra
    // confirmar que o ciclo de sono realmente aconteceu.
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0)
        Serial.println(">> Acordei do DEEP SLEEP (switch aberto)");
    else
        Serial.println(">> Boot normal (energia/reset)");

    pinMode(pinoVermelho, OUTPUT);
    pinMode(pinoVerde, OUTPUT);
    pinMode(pinoAzul, OUTPUT);
    pinMode(pinoBuzzer, OUTPUT);
    pinMode(pinoLuz, OUTPUT);
    pinMode(iniciarReconhecimento, INPUT_PULLUP);   // solto = HIGH

    // (J) ADC do LDR. 12 bits -> 0..4095. Atenuacao de 11dB abre a faixa de
    // leitura para ~0..3.3V; sem isso o ADC satura em ~1V e o divisor de
    // tensao ficaria inutilizavel na pratica.
    analogReadResolution(12);
    analogSetPinAttenuation(pinoLDR, ADC_11db);

    // Devolve o pino do controle RTC pro controle digital normal. Sem isso
    // ele pode continuar "congelado" no estado em que entrou no sono.
    rtc_gpio_deinit(pinoDeepSleep);
    pinMode(pinoDeepSleep, INPUT_PULLUP);

    // --- Configuracao da camera (ANTES do begin) ---
    camera.pinout.freenove_s3();
    camera.brownout.disable();     // evita reset por queda de tensao no pico
    camera.resolution.face();      // 240x240, resolucao esperada pelo modelo
    camera.quality.high();         // quality FIXA: e o que torna o tamanho do
                                   // JPEG utilizavel como proxy de nitidez
    camera.xclk.slow();            // 10MHz: OV2640 estavel, sem chuvisco

    detection.accurate();          // modelo de deteccao mais preciso (e mais lento)
    detection.confidence(0.7);
    recognition.confidence(0.85);  // filtro interno da lib; o gate real de
                                   // seguranca e o PISO_SIM da votacao

    // Laco ate conseguir: sem camera ou sem modelo nao ha o que fazer.
    while (!camera.begin().isOk())
        Serial.println(camera.exception.toString());
    while (!recognition.begin().isOk())
        Serial.println(recognition.exception.toString());

    Serial.println("Camera OK / Recognizer OK");

    // --- Ajuste do SENSOR (obrigatoriamente DEPOIS do begin) ---
    // Este e o UNICO ponto onde da pra "normalizar a imagem": a lib roda a
    // inferencia sobre o frame interno, e o buffer que temos e JPEG
    // comprimido -- nao da pra filtrar pixel entre captura e inferencia.
    sensor_t* s = esp_camera_sensor_get();
    if (s) {
        s->set_hmirror(s, 1);                                  // espelha horizontal
        s->set_gainceiling(s, (gainceiling_t) GAINCEILING_2X); // teto de ganho baixo = menos ruido
        s->set_brightness(s, 1);      // -2..2
        s->set_contrast(s, 1);        // -2..2, ajuda o detalhe fino
        s->set_saturation(s, 0);
        s->set_whitebal(s, 1);        // white balance
        s->set_awb_gain(s, 1);
        s->set_lenc(s, 1);            // corrige vinheta da lente (bordas escuras)
        s->set_vflip(s, 0);
        s->set_dcw(s, 1);

        s->set_raw_gma(s, 1);         // curva de gama do ISP: detalhe em sombras/altas
        s->set_bpc(s, 1);             // correcao de pixels ruins
        s->set_wpc(s, 1);

        if (EXPOSICAO_FIXA) {
            // Desliga os automaticos e trava os valores. Frames com brilho
            // constante geram embeddings mais consistentes.
            s->set_gain_ctrl(s, 0);              // AGC off
            s->set_exposure_ctrl(s, 0);          // AEC off
            s->set_aec2(s, 0);                   // AEC DSP off
            s->set_agc_gain(s, AGC_GANHO_FIXO);
            s->set_aec_value(s, AEC_VALOR_FIXO);
            Serial.println("Sensor: EXPOSICAO FIXA (auto desligado)");
        } else {
            s->set_gain_ctrl(s, 1);
            s->set_exposure_ctrl(s, 1);
            s->set_ae_level(s, 1);               // -2..2, sobe se estiver escuro
            Serial.println("Sensor: exposicao AUTO");
        }
        Serial.println("Sensor tunado");
    }
    else Serial.println("AVISO: sensor_get falhou");

    // --- Buffer do stream + rede ---
    g_mutex = xSemaphoreCreateMutex();
    g_jpg   = (uint8_t*) ps_malloc(JPG_CAP);   // PSRAM: 40KB nao cabe na RAM interna
    if (!g_jpg) {
        Serial.println("ERRO: ps_malloc falhou. PSRAM habilitada? (OPI PSRAM)");
        while (true) delay(1000);              // trava proposital: sem buffer nao roda
    }

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);      // desliga o power save do WiFi: latencia estavel no stream
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("WiFi");
    while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print("."); }
    Serial.println();

    startServer();
    Serial.print(">>> ABRA NO NAVEGADOR:  http://");
    Serial.println(WiFi.localIP());
    Serial.println();

    Serial.printf("LDR (luz ambiente no boot): %u\n", (unsigned) lerLDR());

    tocarMelodia(somBoot);

    while (Serial.available()) Serial.read();   // limpa lixo do buffer serial

    // Pergunta COM TIMEOUT. Sem resposta em 8s, segue sem apagar nada.
    // Critico: com o prompt() bloqueante o setup travava aqui esperando o
    // monitor serial, e o loop() -- que le o switch e o botao -- nunca rodava.
    // Padrao seguro: so apaga com um "s" deliberado.
    if (promptTimeout("Apagar cadastros? [s|n] (8s)", 8000).startsWith("s")) {
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
    Serial.println("  l = testar luz (varredura de duty x leitura do LDR)");
    Serial.println("  s = dormir agora (deep sleep por software)");
    Serial.println();
    Serial.println("BOTAO  GPIO21: aperte para iniciar reconhecimento");
    Serial.println("SWITCH GPIO14: fechado = dorme | aberto = acorda");
    Serial.println("LDR    GPIO2  | COB/MOSFET GPIO42 | LED verde GPIO48");
    Serial.println();
}

// ==================== LOOP ====================
void loop() {

    // Switch fechado (LOW) = dormir. O segundo digitalRead apos 50ms e o
    // debounce: filtra o repique mecanico do contato.
    if (digitalRead(pinoDeepSleep) == LOW) {
        delay(50);
        if (digitalRead(pinoDeepSleep) == LOW)
            entrarEmDeepSleep();   // nao retorna
    }

    // Comandos do monitor serial.
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();
[
        if (cmd.startsWith("d")) { recognition.dump(); }
        else if (cmd.startsWith("z")) { sharpMax = 0; Serial.println(">> pico zerado"); }
        else if (cmd.startsWith("t")) { runTentativa(); }
        else if (cmd.startsWith("m")) { modoContinuo = false; enrollMultiplo(6); }
        else if (cmd.startsWith("l")) { modoContinuo = false; testeLuz(); }
        else if (cmd.startsWith("s")) { entrarEmDeepSleep(); }
    }

    // Botao: dispara UMA vez por aperto, por deteccao de BORDA DE DESCIDA.
    // Sem isso, segurar o botao dispararia tentativas em sequencia.
    // 'static' faz a variavel sobreviver entre chamadas do loop().
    static bool botaoUltimoEstado = HIGH;
    bool botaoAtual = digitalRead(iniciarReconhecimento);

    if (botaoUltimoEstado == HIGH && botaoAtual == LOW) {
        runTentativa();
    }
    botaoUltimoEstado = botaoAtual;

    // Captura + publica SEMPRE, mesmo pausado: o feed precisa ficar vivo
    // pra voce conseguir focar a lente olhando o navegador.
    if (!camera.capture().isOk()) {
        delay(100);
        return;
    }

    publishFrame(camera.frame->buf, camera.frame->len);

    // Pico de nitidez: gire a lente devagar buscando MAXIMIZAR este valor.
    uint32_t sharp = camera.frame->len;
    if (sharp > sharpMax) sharpMax = sharp;

    if (modoContinuo)
        runRecognition();
    else
        snprintf(g_info, sizeof(g_info),
                 "{\"face\":0,\"sharp\":%u,\"peak\":%u,\"ldr\":%u,\"name\":\"(pausado)\",\"sim\":\"-\"}",
                 (unsigned) sharp, (unsigned) sharpMax, (unsigned) lerLDR());

    delay(10);
}

/**
 * Multi-enroll ('m'): grava varios embeddings do MESMO nome.
 * Cobre variacoes de angulo e luz, o que sobe a similaridade do dono e abre
 * distancia em relacao ao impostor -- exatamente o que da margem pra ajustar
 * o PISO_SIM.
 * So conta captura que passa no quality-gate; MAX_TENTATIVAS evita laco
 * infinito se a condicao estiver ruim demais.
 */
void enrollMultiplo(int alvo) {
    String nome = prompt("Nome para cadastro multiplo:");

    Serial.printf(">> Multi-enroll de '%s' (meta: %d capturas boas)\n", nome.c_str(), alvo);
    int ok         = 0;
    int tentativas = 0;
    const int MAX_TENTATIVAS = alvo * 8;   // teto folgado: o gate rejeita bastante
    const char* motivo = "";

    analogWrite(pinoLuz, dutyLuz);   // acende no duty aprendido

    while (ok < alvo) {
        if (tentativas >= MAX_TENTATIVAS) {
            Serial.printf(">> Desisti: so %d/%d salvas em %d tentativas\n",
                          ok, alvo, tentativas);
            analogWrite(pinoAzul, 0);
            analogWrite(pinoLuz, 0);
            return;
        }
        tentativas++;

        Serial.printf("   captura %d/%d (tentativa %d) - posicione o rosto e fique PARADO...\n",
                      ok + 1, alvo, tentativas);

        // ~2s de espera entre capturas, mantendo feed, LED e controle vivos.
        for (int k = 0; k < 20; k++) {
            if (camera.capture().isOk()) {
                publishFrame(camera.frame->buf, camera.frame->len);
            }
            ajustaLuz();
            // Em vez de delay(100) seco, laco de 100ms pulsando o LED.
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
    analogWrite(pinoLuz, 0);
}

/**
 * Prompt BLOQUEANTE: insiste ate receber resposta nao-vazia.
 * O bloqueio aqui e desejado -- so e chamado nos cadastros, onde voce
 * acabou de digitar o comando e portanto esta no monitor serial.
 */
String prompt(String message) {
    String answer;
    do {
        Serial.print(message);
        Serial.print(" ");
        while (!Serial.available())
            delay(1);
        answer = Serial.readStringUntil('\n');
        answer.trim();
    } while (answer.length() == 0);   // enter vazio -> repete a pergunta
    Serial.println(answer);
    return answer;
}

/**
 * Prompt com TIMEOUT: retorna string vazia se ninguem responder.
 * Usado no boot. Resposta vazia e um resultado valido aqui (significa
 * "nao faca nada"), por isso nao ha o laco de insistencia do prompt().
 */
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

