/**
 * ESP32-S3 Face Recognition — Fechadura com reconhecimento facial (v3)
 * Freenove ESP32-S3-WROOM CAM | OV2640
 * Core 2.0.14 | OPI PSRAM | Huge APP | Serial 115200 + New Line
 *
 * ARQUITETURA GERAL
 *   - loop() captura frames continuamente e publica no servidor MJPEG
 *     (assim o feed no navegador mostra EXATAMENTE o frame usado na
 *     inferencia -- serve pra focar a lente e calibrar limiares).
 *   - O reconhecimento NAO decide por 1 frame. Uma "tentativa" coleta
 *     ate JANELA_N frames validos e exige VOTOS_K votos a favor.
 *     Isso derruba o falso-positivo esporadico, que e o modo de falha
 *     tipico quando similaridade de impostor chega perto da do dono.
 *   - Antes de gastar inferencia, um quality-gate descarta frame ruim.
 */

#include <eloquent_esp32cam.h>
#include <eloquent_esp32cam/face/detection.h>
#include <eloquent_esp32cam/face/recognition.h>

#include <WiFi.h>
#include <esp_http_server.h> // servidor HTTP nativo do IDF (mais leve que WebServer.h)
#include <math.h>            // sin() do pulso do LED

#include "driver/rtc_io.h" // pull-up no dominio RTC (sobrevive ao sono)
#include "esp_sleep.h"     // deep sleep


using eloq::camera;
using eloq::face::detection;
using eloq::face::recognition;

// ==================== PINOS ====================
// LED azul saiu do GPIO14 (que virou o switch de sono) e foi pro 47.
// (K) LED VERDE saiu do GPIO2 e foi pro 48, liberando o 2 para o LDR.
const int pinoVermelho = 1;
const int pinoVerde = 48; // (K) ERA 2 -- movido para liberar o ADC1
const int pinoAzul = 47;
const int pinoBuzzer = 41;
const int pinoLuz = 42; // gate do MOSFET que aciona o COB

// (J) FOTORRESISTOR: GPIO 2 = ADC1_CH1.
// TEM que ser ADC1 (GPIO 1..10 no S3): o ADC2 nao funciona com WiFi ligado.
const int pinoLDR = 2;

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
const int LDR_ALVO = 2000;   // leitura desejada (0..4095)
const int BANDA_MORTA = 150; // nao mexe se estiver perto do alvo.
                             // Sem banda morta o controle OSCILA em torno
                             // do setpoint, e luz piscando estraga a
                             // consistencia dos embeddings.
const int DUTY_MIN = 10;     // nunca apaga de vez durante a operacao
const int DUTY_MAX = 255;
const int PASSO_DUTY = 2; // ajuste INCREMENTAL, nao proporcional:
                          // mover pouco por vez tambem evita oscilacao
int dutyLuz = 30;         // ponto de partida

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
    {523, 50}, {659, 50}, {784, 50}, {1047, 200}, {0, 0}};

// Alarme/tamper: alternancia aguda e estridente (padrao de alerta).
const Nota somAlarme[] = {
    {2500, 300}, {2000, 300}, {2500, 300}, {2000, 300}, {0, 0}};

// Boot: bipe curto + bipe mais alto = "sistema pronto".
const Nota somBoot[] = {{1800, 80}, {0, 40}, {2300, 200}, {0, 0}};

// Acesso negado: descendente e grave. Seco, e inconfundivel com o de
// abertura justamente porque desce e termina no registro grave.
const Nota somAcessoNegado[] = {{600, 150}, // ataque imediato
                                {450, 150}, // transicao rapida
                                {300, 300}, // finalizacao grave e seca
                                {0, 0}};

// ==================== VOTACAO POR RAJADA ====================
#define ALVO_NOME "caio" // unico nome autorizado a abrir
#define PISO_SIM                                                               \
  0.92f                  // similaridade minima pra um frame virar VOTO.
                         // Este e o gate REAL de seguranca -- deve ficar
                         // acima do teto observado do impostor e abaixo
                         // do chao observado do dono. MEDIR e ajustar.
#define JANELA_N 7       // teto de frames validos por tentativa
#define VOTOS_K 4        // votos a favor necessarios (K de N)
#define TIMEOUT_MS 12000 // aborta se nao juntar N validos a tempo

// ==================== QUALITY-GATE ====================
// (I) Os limiares de BRILHO foram removidos -- ver nota no cabecalho.
// Sobraram os dois que se apoiam em relacao fisica real com o tamanho do JPEG.
#define MIN_SHARP                                                              \
  3000                  // abaixo disso = desfocado. Com quality fixa,
                        // imagem borrada comprime mais -> JPEG menor.
#define MAX_SHARP 38000 // teto: frame gigante = ruido/anomalia
#define MOV_MAX_DELTA                                                          \
  9000 // salto de tamanho do JPEG entre 2 frames.
       // Cena mudando rapido -> risco de motion blur.

// ==================== EXPOSICAO ====================
// Com exposicao fixa o brilho para de variar entre frames, o que estabiliza
// os embeddings e mata flicker. EXIGE luz constante (COB); e justamente por
// isso o controle do COB via LDR faz sentido: ele mantem a cena estavel.
#define EXPOSICAO_FIXA true
#define AEC_VALOR_FIXO 300 // 0..1200
#define AGC_GANHO_FIXO 0   // 0..30 (ganho baixo = menos ruido)

// ==================== CONFIG ====================
#define WIFI_SSID "Caio.2g"
#define WIFI_PASS "28460363"
#define JPG_CAP 40000 // teto do buffer compartilhado do stream
#define FRAME_W 240   // camera.resolution.face()
#define FRAME_H 240

// Resultado de uma tentativa.
// FICA AQUI EM CIMA de proposito: o Arduino IDE injeta prototipos
// automaticos no topo do arquivo. Se o enum estiver la embaixo, o prototipo
// de runTentativa() referencia Veredito antes da declaracao
// -> erro "'Veredito' does not name a type".
enum Veredito { PENDENTE, APROVADO, NEGADO, EXPIROU };

// Prototipos explicitos (nao dependemos da geracao automatica do IDE).
String prompt(String message);
String promptTimeout(String message, uint32_t ms);
void entrarEmDeepSleep();
void doEnroll();
void runRecognition();
void enrollMultiplo(int alvo);
void publishFrame(const uint8_t *buf, size_t len);
bool frameOk(const char *&motivoOut);
void pulsaLEDEspera();
void sinalizaResultado(int pino, const Nota melodia[]);
Veredito runTentativa();
void tocarMelodia(const Nota melodia[]);
uint16_t lerLDR();
void ajustaLuz();
void testeLuz();

bool modoContinuo = false; // 'r' liga: imprime similaridade a cada frame
uint32_t sharpMax = 0;     // pico de nitidez ja visto (guia pra focar a lente)
uint32_t lastPrint = 0;    // throttle do serial no modo continuo
uint32_t ultimoSharp = 0;  // tamanho do frame anterior (detector de movimento)
volatile char httpCommand = 0; // comando web atômico ('c', 'r', 'p', 't')
String lastAccType = "-";  // "granted", "denied" ou "-"
String lastAccName = "-";  // nome ou "desconhecido"

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
      delay(melodia[i].duracao); // pausa
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
  uint16_t luz = lerLDR();
  int erro = LDR_ALVO - (int)luz;

  if (abs(erro) <= BANDA_MORTA)
    return; // dentro da banda: nao mexe

  // erro > 0 -> esta escuro -> sobe o duty
  dutyLuz += (erro > 0) ? PASSO_DUTY : -PASSO_DUTY;
  dutyLuz = constrain(dutyLuz, DUTY_MIN, DUTY_MAX);

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
    delay(500); // o LDR tem inercia: precisa de tempo pra estabilizar
    Serial.printf("   %4d  ->  %u\n", niveis[i], (unsigned)lerLDR());
  }

  analogWrite(pinoLuz, 0);
  delay(500);
  Serial.printf("   luz ambiente (COB apagado): %u\n", (unsigned)lerLDR());
  Serial.println(">> Fim. Se a leitura nao subir com o duty, e hardware:");
  Serial.println(
      "   MOSFET nao logic-level, GND nao comum, ou LDR mal ligado.\n");
}

/**
 * Apaga tudo, arma o despertador e dorme. NAO RETORNA: ao acordar, a
 * placa reinicia pelo setup() (a RAM do dominio digital foi perdida).
 */
void entrarEmDeepSleep() {
  Serial.println(">> Entrando em DEEP SLEEP (abra o switch para acordar)");
  Serial.flush(); // sem isso o chip dorme antes da UART terminar de enviar

  digitalWrite(pinoVermelho, LOW);
  digitalWrite(pinoVerde, LOW);
  analogWrite(pinoAzul, 0);    // mata o PWM residual do pulsaLEDEspera
  digitalWrite(pinoAzul, LOW); // garante nivel logico LOW
  analogWrite(pinoLuz, 0);     // apaga o COB
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
  analogWrite(pinoLuz, 0); // apaga a iluminacao ao encerrar
  digitalWrite(pino, HIGH);
  tocarMelodia(melodia);
  delay(2500); // mantem a cor visivel (simula porta aberta)
  digitalWrite(pino, LOW);
}

/**
 * Respiracao do LED azul enquanto processa. Nao bloqueia: le millis() e
 * escreve o duty na hora, entao precisa ser chamada repetidamente dentro
 * do laco de quem estiver trabalhando.
 */
void pulsaLEDEspera() {
  float onda = (sin(millis() / 300.0) + 1) / 2; // normaliza -1..1 para 0..1
  int brilhoLED = onda * 255;
  analogWrite(pinoAzul, brilhoLED);
}

/**
 * QUALITY-GATE: decide se o frame atual merece inferencia.
 * (I) So restaram nitidez e movimento -- os dois criterios que tem relacao
 * fisica comprovada com o tamanho do JPEG. O criterio de brilho foi removido
 * por nao medir o que dizia medir.
 * Escreve o motivo da rejeicao em motivoOut (passado por referencia).
 */
bool frameOk(const char *&motivoOut) {
  uint32_t sharp = camera.frame->len; // o tamanho do JPEG E o proxy de nitidez

  // 1) NITIDEZ
  if (sharp < MIN_SHARP) {
    motivoOut = "desfocado (sharp baixo)";
    return false;
  }
  if (sharp > MAX_SHARP) {
    motivoOut = "anomalo (sharp alto demais)";
    return false;
  }

  // 2) MOVIMENTO: JPEG muda muito de tamanho quando a cena muda rapido.
  //    Na 1a chamada ultimoSharp = 0 e o teste e pulado.
  if (ultimoSharp != 0) {
    uint32_t delta =
        (sharp > ultimoSharp) ? (sharp - ultimoSharp) : (ultimoSharp - sharp);
    if (delta > MOV_MAX_DELTA) {
      ultimoSharp = sharp; // atualiza antes de sair, senao a proxima
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
  int validos = 0;
  int votosFavor = 0;
  uint32_t t0 = millis();
  const char *motivo = "";

  Serial.println(">> TENTATIVA iniciada");

  analogWrite(pinoLuz, dutyLuz); // acende no duty aprendido

  while (validos < JANELA_N) {

    pulsaLEDEspera(); // precisa ser chamada no laco pra "respirar"

    // Timeout: rosto nao apareceu ou sumiu no meio.
    if (millis() - t0 > TIMEOUT_MS) {
      Serial.printf(">> EXPIROU (so %d/%d validos)\n", validos, JANELA_N);
      sinalizaResultado(pinoVermelho, somAcessoNegado);
      lastAccType = "denied";
      lastAccName = "timeout";
      return EXPIROU;
    }

    // Frame fresco. A lib gerencia o buffer sozinha (nao ha fb_return).
    if (!camera.capture().isOk()) {
      delay(20);
      continue;
    }
    publishFrame(camera.frame->buf, camera.frame->len); // mantem o feed vivo

    // Controle de luz ANTES do gate: o LDR independe do frame, e se o
    // gate rejeitasse primeiro o controle nunca agiria quando a cena
    // estivesse ruim -- justamente quando ele e necessario.
    ajustaLuz();

    // Descarta frame ruim ANTES de gastar inferencia.
    if (!frameOk(motivo)) {
      Serial.printf("   frame descartado: %s\n", motivo);
      continue; // nao conta como valido
    }

    // Nenhum dos dois conta como valido se falhar: sem rosto na cena
    // ou modelo sem resposta nao sao "voto contra", sao "nada".
    if (!recognition.detect().isOk())
      continue;
    if (!recognition.recognize().isOk())
      continue;

    validos++;
    const char *nome = recognition.match.name.c_str();
    float sim = recognition.match.similarity;

    // O voto exige AS DUAS coisas: nome certo e similaridade acima do piso.
    bool aFavor = (strcmp(nome, ALVO_NOME) == 0) && (sim >= PISO_SIM);
    if (aFavor)
      votosFavor++;

    Serial.printf("   frame %d/%d: %s sim=%.3f -> %s  (favor=%d)\n", validos,
                  JANELA_N, nome, sim, aFavor ? "VOTO" : "descartado",
                  votosFavor);

    // EARLY-EXIT
    if (votosFavor >= VOTOS_K) {
      Serial.printf(">> APROVADO (early-exit: %d votos em %d frames)\n",
                    votosFavor, validos);
      sinalizaResultado(pinoVerde, somPortaAberta);
      lastAccType = "granted";
      lastAccName = ALVO_NOME;
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
    lastAccType = "granted";
    lastAccName = ALVO_NOME;
    return APROVADO;
  }

  Serial.printf(">> NEGADO (%d/%d votos, precisava %d)\n", votosFavor, validos,
                VOTOS_K);
  sinalizaResultado(pinoVermelho, somAcessoNegado);
  lastAccType = "denied";
  lastAccName = "desconhecido";
  return NEGADO;
}

// ==================== BUFFER COMPARTILHADO DO STREAM ====================
// O loop principal escreve aqui; a task do servidor HTTP le. Sao contextos
// diferentes (FreeRTOS), por isso o mutex.
static uint8_t *g_jpg = nullptr; // copia do ultimo frame (PSRAM)
static size_t g_jpgLen = 0;
static volatile uint32_t g_frameId = 0; // contador: sinaliza frame novo
static SemaphoreHandle_t g_mutex = nullptr;
static char g_info[350] =
    "{\"face\":0,\"sharp\":0,\"peak\":0,\"ldr\":0,\"name\":\"-\",\"sim\":\"-\","
    "\"last_acc\":\"-\",\"last_name\":\"-\"}";
static httpd_handle_t g_server = nullptr;

/**
 * Copia o frame pro buffer compartilhado.
 * Usa timeout curto no mutex: se o servidor estiver ocupado, PERDE o frame
 * em vez de travar o loop de captura. Preferivel perder feed a travar a
 * fechadura.
 */
void publishFrame(const uint8_t *buf, size_t len) {
  if (!g_jpg || !buf || len == 0 || len > JPG_CAP)
    return;
  if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(20)) != pdTRUE)
    return;
  memcpy(g_jpg, buf, len);
  g_jpgLen = len;
  g_frameId++;
  xSemaphoreGive(g_mutex);
}

// ==================== HTTP ====================

/** /info -> JSON com as metricas. no-store pro navegador nao cachear. */
static esp_err_t infoHandler(httpd_req_t *req) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, g_info, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t controlHandler(httpd_req_t *req) {
  char buf[32];
  if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
    char val[16];
    if (httpd_query_key_value(buf, "cmd", val, sizeof(val)) == ESP_OK) {
      httpCommand = val[0];
    }
  }
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, "OK", 2);
}

/**
 * /stream -> MJPEG (multipart/x-mixed-replace): sequencia infinita de JPEGs
 * separados por um boundary. O <img> do navegador entende nativamente.
 * Copia pra um buffer local antes de enviar, pra soltar o mutex rapido e
 * nao segurar o loop de captura durante a transmissao (que e lenta).
 */
static esp_err_t streamHandler(httpd_req_t *req) {
  uint8_t *local = (uint8_t *)ps_malloc(JPG_CAP);
  if (!local)
    return ESP_FAIL;

  httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=frame");
  uint32_t lastId = 0;
  char part[96];

  while (true) {
    size_t len = 0;
    if (xSemaphoreTake(g_mutex, portMAX_DELAY) == pdTRUE) {
      // So copia se houver frame NOVO (evita reenviar o mesmo).
      if (g_frameId != lastId && g_jpgLen) {
        memcpy(local, g_jpg, g_jpgLen);
        len = g_jpgLen;
        lastId = g_frameId;
      }
      xSemaphoreGive(g_mutex);
    }
    if (!len) {
      delay(10);
      continue;
    } // nada novo: espera

    // Qualquer falha de envio = cliente fechou a aba -> sai do laco.
    if (httpd_resp_send_chunk(req, "\r\n--frame\r\n", 11) != ESP_OK)
      break;
    int n = snprintf(part, sizeof(part),
                     "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                     (unsigned)len);
    if (httpd_resp_send_chunk(req, part, n) != ESP_OK)
      break;
    if (httpd_resp_send_chunk(req, (const char *)local, len) != ESP_OK)
      break;
  }
  free(local);
  return ESP_OK;
}

// Pagina de debug: imagem + miras de centralizacao + area segura tracejada
// + painel de metricas atualizado a cada 250ms via /info.
// PROGMEM mantem a string na flash, sem gastar RAM.
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
  min-height: 100vh;
  overflow-x: hidden;
  line-height: 1.5;
}

/* Background grid pattern */
body::before {
  content: '';
  position: fixed;
  inset: 0;
  background-image:
    linear-gradient(rgba(59,130,246,0.03) 1px, transparent 1px),
    linear-gradient(90deg, rgba(59,130,246,0.03) 1px, transparent 1px);
  background-size: 40px 40px;
  pointer-events: none;
  z-index: 0;
}

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
  max-width: 1400px;
  margin: 0 auto;
  padding: 24px 24px 40px;
}

.grid {
  display: grid;
  grid-template-columns: 1fr 400px;
  gap: 20px;
  align-items: start;
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
  min-height: 360px;
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
  object-fit: cover;
  display: block;
  min-height: 360px;
  max-height: 480px;
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
  height: 340px;
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
          <div class="input-group">
            <span class="input-prefix">http://</span>
            <input type="text" id="espIpInput" placeholder="192.168.1.xxx" value="" autocomplete="off" spellcheck="false" />
          </div>
        </div>
        <button class="btn-connect" id="btnConnect" onclick="connectToESP()">
          <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M5 12.55a11 11 0 0 1 14.08 0"/><path d="M1.42 9a16 16 0 0 1 21.16 0"/><path d="M8.53 16.11a6 6 0 0 1 6.95 0"/><circle cx="12" cy="20" r="1"/></svg>
          Conectar
        </button>
        <button class="btn-disconnect" id="btnDisconnect" onclick="disconnectFromESP()" style="display:none;">
          <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/></svg>
          Desconectar
        </button>
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
            <p class="placeholder-text">Configure o IP do ESP32 e clique em <strong>Conectar</strong></p>
            <p class="placeholder-sub">para iniciar o stream de vídeo ao vivo</p>
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
          <button class="ctrl-btn enroll-btn" id="btnEnroll" onclick="startEnroll()" title="Cadastrar rosto">
            <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M16 21v-2a4 4 0 0 0-4-4H5a4 4 0 0 0-4 4v2"/><circle cx="8.5" cy="7" r="4"/><line x1="20" y1="8" x2="20" y2="14"/><line x1="23" y1="11" x2="17" y2="11"/></svg>
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
    // Auto-conecta se o dashboard estiver sendo servido pelo próprio ESP32 (ou servidor local)
    if (window.location.protocol.startsWith('http') && window.location.host === savedIp) {
      setTimeout(connectToESP, 100);
    }
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
  // Se o usuário digitou uma porta no IP (ex: localhost:3000), usa ela. 
  // Senão, usa a rota padrão /stream
  const streamUrl = `http://${ip}/stream`;

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
  const btnConnect = document.getElementById('btnConnect');
  const btnDisconnect = document.getElementById('btnDisconnect');

  if (connected) {
    dot.className = 'status-dot connected';
    label.textContent = `Conectado — ${state.esp32Ip}`;
    liveBadge.className = 'live-badge live';
    liveBadge.innerHTML = '<span class="live-dot"></span> AO VIVO';
    btnConnect.style.display = 'none';
    btnDisconnect.style.display = 'flex';
  } else {
    dot.className = 'status-dot';
    label.textContent = 'Desconectado';
    liveBadge.className = 'live-badge';
    liveBadge.innerHTML = '<span class="live-dot"></span> OFFLINE';
    btnConnect.style.display = 'flex';
    btnDisconnect.style.display = 'none';
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
async function sendControl(cmd) {
  if (!state.connected) {
    showToast('Conecte ao ESP32 primeiro!', 'warning');
    return false;
  }
  try {
    const url = `http://${state.esp32Ip}/control?cmd=${cmd}`;
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

async function startEnroll() {
  const ok = await sendControl('c');
  if (ok) {
    showToast('📸 Cadastramento iniciado! Olhe para a câmera.', 'warning');
    addLogEntry('info', 'Cadastramento iniciado', 'Aguardando rosto');
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

static esp_err_t indexHandler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

/** Sobe o httpd com stack maior que o padrao (o stream precisa). */
void startServer() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 80;
  cfg.ctrl_port = 32768;
  cfg.max_uri_handlers = 8;
  cfg.stack_size = 8192;

  if (httpd_start(&g_server, &cfg) != ESP_OK) {
    Serial.println("ERRO: httpd falhou");
    return;
  }

  httpd_uri_t u1 = {"/", HTTP_GET, indexHandler, nullptr};
  httpd_uri_t u2 = {"/stream", HTTP_GET, streamHandler, nullptr};
  httpd_uri_t u3 = {"/info", HTTP_GET, infoHandler, nullptr};
  httpd_uri_t u4 = {"/control", HTTP_GET, controlHandler, nullptr};
  httpd_register_uri_handler(g_server, &u1);
  httpd_register_uri_handler(g_server, &u2);
  httpd_register_uri_handler(g_server, &u3);
  httpd_register_uri_handler(g_server, &u4);
}

// ==================== SETUP ====================
void setup() {
  delay(2000); // da tempo do monitor serial conectar
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
  pinMode(iniciarReconhecimento, INPUT_PULLUP); // solto = HIGH

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
  camera.brownout.disable(); // evita reset por queda de tensao no pico
  camera.resolution.face();  // 240x240, resolucao esperada pelo modelo
  camera.quality.high();     // quality FIXA: e o que torna o tamanho do
                             // JPEG utilizavel como proxy de nitidez
  camera.xclk.slow();        // 10MHz: OV2640 estavel, sem chuvisco

  detection.accurate(); // modelo de deteccao mais preciso (e mais lento)
  detection.confidence(0.7);
  recognition.confidence(0.85); // filtro interno da lib; o gate real de
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
  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_hmirror(s, 1); // espelha horizontal
    s->set_gainceiling(
        s, (gainceiling_t)GAINCEILING_2X); // teto de ganho baixo = menos ruido
    s->set_brightness(s, 1);               // -2..2
    s->set_contrast(s, 1);                 // -2..2, ajuda o detalhe fino
    s->set_saturation(s, 0);
    s->set_whitebal(s, 1); // white balance
    s->set_awb_gain(s, 1);
    s->set_lenc(s, 1); // corrige vinheta da lente (bordas escuras)
    s->set_vflip(s, 0);
    s->set_dcw(s, 1);

    s->set_raw_gma(s, 1); // curva de gama do ISP: detalhe em sombras/altas
    s->set_bpc(s, 1);     // correcao de pixels ruins
    s->set_wpc(s, 1);

    if (EXPOSICAO_FIXA) {
      // Desliga os automaticos e trava os valores. Frames com brilho
      // constante geram embeddings mais consistentes.
      s->set_gain_ctrl(s, 0);     // AGC off
      s->set_exposure_ctrl(s, 0); // AEC off
      s->set_aec2(s, 0);          // AEC DSP off
      s->set_agc_gain(s, AGC_GANHO_FIXO);
      s->set_aec_value(s, AEC_VALOR_FIXO);
      Serial.println("Sensor: EXPOSICAO FIXA (auto desligado)");
    } else {
      s->set_gain_ctrl(s, 1);
      s->set_exposure_ctrl(s, 1);
      s->set_ae_level(s, 1); // -2..2, sobe se estiver escuro
      Serial.println("Sensor: exposicao AUTO");
    }
    Serial.println("Sensor tunado");
  } else
    Serial.println("AVISO: sensor_get falhou");

  // --- Buffer do stream + rede ---
  g_mutex = xSemaphoreCreateMutex();
  g_jpg = (uint8_t *)ps_malloc(JPG_CAP); // PSRAM: 40KB nao cabe na RAM interna
  if (!g_jpg) {
    Serial.println("ERRO: ps_malloc falhou. PSRAM habilitada? (OPI PSRAM)");
    while (true)
      delay(1000); // trava proposital: sem buffer nao roda
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(
      false); // desliga o power save do WiFi: latencia estavel no stream
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  startServer();
  Serial.print(">>> ABRA NO NAVEGADOR:  http://");
  Serial.println(WiFi.localIP());
  Serial.println();

  Serial.printf("LDR (luz ambiente no boot): %u\n", (unsigned)lerLDR());

  tocarMelodia(somBoot);

  while (Serial.available())
    Serial.read(); // limpa lixo do buffer serial

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
      entrarEmDeepSleep(); // nao retorna
  }

  // Comandos da web.
  if (httpCommand != 0) {
    char cmd = httpCommand;
    httpCommand = 0;
    if (cmd == 'c') {
      modoContinuo = false;
      doEnroll(ALVO_NOME);
    } else if (cmd == 'r') {
      modoContinuo = true;
      Serial.println(">> MODO CONTINUO (Web)");
    } else if (cmd == 'p') {
      modoContinuo = false;
      Serial.println(">> PAUSADO (Web)");
    } else if (cmd == 't') {
      runTentativa();
    }
  }

  // Comandos do monitor serial.
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();

    if (cmd.startsWith("c")) {
      modoContinuo = false;
      doEnroll();
    } else if (cmd.startsWith("r")) {
      modoContinuo = true;
      Serial.println(">> MODO CONTINUO");
    } else if (cmd.startsWith("p")) {
      modoContinuo = false;
      Serial.println(">> PAUSADO (feed ativo)");
    } else if (cmd.startsWith("d")) {
      recognition.dump();
    } else if (cmd.startsWith("z")) {
      sharpMax = 0;
      Serial.println(">> pico zerado");
    } else if (cmd.startsWith("t")) {
      runTentativa();
    } else if (cmd.startsWith("m")) {
      modoContinuo = false;
      enrollMultiplo(6);
    } else if (cmd.startsWith("l")) {
      modoContinuo = false;
      testeLuz();
    } else if (cmd.startsWith("s")) {
      entrarEmDeepSleep();
    }
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
  if (sharp > sharpMax)
    sharpMax = sharp;

  if (modoContinuo)
    runRecognition();
  else
    snprintf(
        g_info, sizeof(g_info),
        "{\"face\":0,\"sharp\":%u,\"peak\":%u,\"ldr\":%u,\"name\":\"(pausado)"
        "\",\"sim\":\"-\",\"last_acc\":\"%s\",\"last_name\":\"%s\"}",
        (unsigned)sharp, (unsigned)sharpMax, (unsigned)lerLDR(),
        lastAccType.c_str(), lastAccName.c_str());

  delay(10);
}

/**
 * Modo continuo ('r'): roda inferencia no MESMO frame que foi pro navegador
 * e publica o resultado em /info. Serve pra MEDIR similaridades reais --
 * do dono e de impostores -- e dai calibrar o PISO_SIM da votacao.
 * Nao aciona nada: e diagnostico, nao decisao.
 */
void runRecognition() {
  uint32_t sharp = camera.frame->len;
  uint16_t ldr = lerLDR();

  // Sem rosto na cena.
  if (!recognition.detect().isOk()) {
    snprintf(g_info, sizeof(g_info),
             "{\"face\":0,\"sharp\":%u,\"peak\":%u,\"ldr\":%u,\"name\":\"-\","
             "\"sim\":\"-\",\"last_acc\":\"%s\",\"last_name\":\"%s\"}",
             (unsigned)sharp, (unsigned)sharpMax, (unsigned)ldr,
             lastAccType.c_str(), lastAccName.c_str());
    return;
  }

  // Rosto detectado, mas sem match (face:1, name:"?").
  if (!recognition.recognize().isOk()) {
    snprintf(g_info, sizeof(g_info),
             "{\"face\":1,\"sharp\":%u,\"peak\":%u,\"ldr\":%u,\"name\":\"?\","
             "\"sim\":\"-\",\"last_acc\":\"%s\",\"last_name\":\"%s\"}",
             (unsigned)sharp, (unsigned)sharpMax, (unsigned)ldr,
             lastAccType.c_str(), lastAccName.c_str());
    return;
  }

  const char *name = recognition.match.name.c_str();
  float sim = recognition.match.similarity;

  snprintf(g_info, sizeof(g_info),
           "{\"face\":1,\"sharp\":%u,\"peak\":%u,\"ldr\":%u,\"name\":\"%s\","
           "\"sim\":\"%.4f\",\"last_acc\":\"%s\",\"last_name\":\"%s\"}",
           (unsigned)sharp, (unsigned)sharpMax, (unsigned)ldr, name, sim,
           lastAccType.c_str(), lastAccName.c_str());

  // Throttle: imprimir a cada frame inundaria o serial e atrasaria o stream.
  if (millis() - lastPrint > 400) {
    lastPrint = millis();
    Serial.printf("MATCH: %-10s sim=%.4f  sharp=%u  ldr=%u  (%dms)\n", name,
                  sim, (unsigned)sharp, (unsigned)ldr,
                  recognition.benchmark.millis());
  }
}

/**
 * Cadastro simples ('c'): 3s pra voce se posicionar, depois UMA captura.
 * Um unico embedding -- suficiente pra testar, fraco pra uso real.
 * A luz acende aqui tambem: cadastrar sob luz ambiente e depois comparar sob
 * luz do COB derruba a similaridade -- exatamente o problema que o sistema
 * tenta evitar.
 */
void doEnroll(String defaultName) {
  String name = defaultName;
  if (name == "") {
    name = prompt("Nome:");
  }

  analogWrite(pinoLuz, dutyLuz); // acende no duty aprendido

  Serial.println("Posicione o rosto. Cadastrando em 3s...");
  for (int i = 0; i < 30;
       i++) { // 30 x 100ms: tempo de sobra pro controle convergir
    if (camera.capture().isOk()) {
      publishFrame(camera.frame->buf, camera.frame->len);
      ajustaLuz();
    }
    delay(100);
  }

  if (!camera.capture().isOk()) {
    Serial.println("ERRO: captura falhou");
    analogWrite(pinoLuz, 0);
    return;
  }
  publishFrame(camera.frame->buf, camera.frame->len);

  // Gate no cadastro e o ponto mais critico do sistema: um embedding
  // gerado de rosto borrado contamina TODAS as comparacoes futuras.
  const char *motivo = "";
  if (!frameOk(motivo)) {
    Serial.printf("ERRO: frame ruim para cadastro (%s). Tente de novo.\n",
                  motivo);
    analogWrite(pinoLuz, 0);
    return;
  }

  if (!recognition.detect().isOk()) {
    Serial.println("ERRO: nenhum rosto detectado");
    analogWrite(pinoLuz, 0);
    return;
  }

  if (recognition.enroll(name).isOk()) {
    Serial.print("OK, cadastrado: ");
    Serial.println(name);
  } else {
    Serial.println(recognition.exception.toString());
  }

  analogWrite(pinoLuz, 0);
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

  Serial.printf(">> Multi-enroll de '%s' (meta: %d capturas boas)\n",
                nome.c_str(), alvo);
  int ok = 0;
  int tentativas = 0;
  const int MAX_TENTATIVAS = alvo * 8; // teto folgado: o gate rejeita bastante
  const char *motivo = "";

  analogWrite(pinoLuz, dutyLuz); // acende no duty aprendido

  while (ok < alvo) {
    if (tentativas >= MAX_TENTATIVAS) {
      Serial.printf(">> Desisti: so %d/%d salvas em %d tentativas\n", ok, alvo,
                    tentativas);
      analogWrite(pinoAzul, 0);
      analogWrite(pinoLuz, 0);
      return;
    }
    tentativas++;

    Serial.printf("   captura %d/%d (tentativa %d) - posicione o rosto e fique "
                  "PARADO...\n",
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

    if (!camera.capture().isOk()) {
      Serial.println("   captura falhou, repetindo");
      continue;
    }
    publishFrame(camera.frame->buf, camera.frame->len);

    if (!frameOk(motivo)) {
      Serial.printf("   descartei (%s), repetindo\n", motivo);
      continue;
    }

    if (!recognition.detect().isOk()) {
      Serial.println("   sem rosto, repetindo");
      continue;
    }

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
  } while (answer.length() == 0); // enter vazio -> repete a pergunta
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
