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

#include <ESP32Servo.h>

#include <ESPmDNS.h>

#include "dashboard.h"
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
const int LDR_ALVO = 3000;   // leitura desejada (0..4095)
const int BANDA_MORTA = 150; // nao mexe se estiver perto do alvo.
                             // Sem banda morta o controle OSCILA em torno
                             // do setpoint, e luz piscando estraga a
                             // consistencia dos embeddings.
const int DUTY_MIN = 70;      // nunca apaga de vez durante a operacao
const int DUTY_MAX = 255;
const int PASSO_DUTY = 2; // ajuste INCREMENTAL, nao proporcional:
                          // mover pouco por vez tambem evita oscilacao
int dutyLuz = 70;         // ponto de partida

// Botao momentaneo que dispara uma tentativa de reconhecimento.
const int iniciarReconhecimento = 21;

// Switch de trava do deep sleep. TEM que ser RTC GPIO (0..21 no S3):
// so o dominio RTC fica vivo dormindo, entao so esses pinos acordam.
const gpio_num_t pinoDeepSleep = GPIO_NUM_14;

// ==================== SERVO DA FECHADURA ====================
Servo fechaduraServo;
const int pinoServo = 40;          // GPIO40 livre no projeto
const int SERVO_FECHADO = 0;       // trancado
const int SERVO_ABERTO  = 90;      // destrancado
const uint32_t TEMPO_ABERTO_MS = 7000; // tempo que fica aberta antes de fechar

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

#define PISO_SIM                                                               \
  0.91f                  // similaridade minima pra um frame virar VOTO.
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
//#define WIFI_SSID "WIFI"
 #define WIFI_PASS "28460363"
//#define WIFI_PASS "net12345"

#define JPG_CAP 40000 // teto do buffer compartilhado do stream
#define FRAME_W 240   // camera.resolution.face()
#define FRAME_H 240

// Resultado de uma tentativa.
// FICA AQUI EM CIMA de proposito: o Arduino IDE injeta prototipos
// automaticos no topo do arquivo. Se o enum estiver la embaixo, o prototipo
// de runTentativa() referencia Veredito antes da declaracao
// -> erro "'Veredito' does not name a type".
enum Veredito { PENDENTE, APROVADO, NEGADO, EXPIROU };

// modo do LED azul, escrito pela task da camera, lido pela task do LED
enum EstadoLED { LED_OFF, LED_RESPIRANDO };
volatile EstadoLED estadoLED = LED_OFF;

// pedido de tentativa: setado pelo botao (loop), consumido pela task da camera
volatile bool pedidoReconhecimento = false;

volatile bool pedidoAcessoRemoto = false;

TaskHandle_t handleCamera = nullptr;
TaskHandle_t handleLED = nullptr;


// Prototipos explicitos (nao dependemos da geracao automatica do IDE).
String prompt(String message);
String promptTimeout(String message, uint32_t ms);
void entrarEmDeepSleep();
void doEnroll(String defaultName = "");
void runRecognition();
void enrollMultiplo(int alvo, String defaultName = "");
void publishFrame(const uint8_t *buf, size_t len);
bool frameOk(const char *&motivoOut);
void pulsaLEDEspera();
void sinalizaResultado(int pino, const Nota melodia[]);
Veredito runTentativa();
void tocarMelodia(const Nota melodia[]);
uint16_t lerLDR();
void ajustaLuz();
void testeLuz();
void liberarAcessoRemoto();

bool modoContinuo = false; // 'r' liga: imprime similaridade a cada frame
uint32_t sharpMax = 0;     // pico de nitidez ja visto (guia pra focar a lente)
uint32_t lastPrint = 0;    // throttle do serial no modo continuo
uint32_t ultimoSharp = 0;  // tamanho do frame anterior (detector de movimento)
volatile char httpCommand = 0; // comando web atômico ('c', 'r', 'p', 't', 'm')
char httpCommandName[64] = ""; // nome passado via web
char httpNewCommandName[64] = ""; // novo nome para edicao
String lastAccType = "-";      // "granted", "denied" ou "-"
String lastAccName = "-";      // nome ou "desconhecido"
String enrollStatus = "idle";
String enrollMsg = "-";
volatile bool cancelarCadastro = false;


void tarefaLED(void *pv) {
  for (;;) {
    if (estadoLED == LED_RESPIRANDO) {
      float onda = (sin(millis() / 300.0) + 1) / 2;
      analogWrite(pinoAzul, (int)(onda * 255));
    } else {
      analogWrite(pinoAzul, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(20)); // ~50 updates/s, independe da inferencia
  }
}

void tarefaCamera(void *pv) {
  for (;;) {
    // comando pendente (web ou serial funilam no mesmo httpCommand)
    if (httpCommand != 0) {
      char cmd = httpCommand;
      httpCommand = 0;
      switch (cmd) {
        case 'c': modoContinuo = false; cancelarCadastro = false;
                  updateEnrollStatus("capturing", "starting...");   // sai de "success" na hora
                  doEnroll(httpCommandName[0] ? String(httpCommandName) : ""); break;
        case 'm': modoContinuo = false; cancelarCadastro = false;
                  updateEnrollStatus("capturing", "starting...");   // idem
                  enrollMultiplo(3, httpCommandName[0] ? String(httpCommandName) : ""); break;
        case 'x': cancelarCadastro = true; break;
        case 'o': pedidoAcessoRemoto = true; break;
        case 'k': {
            File src = SPIFFS.open("/fr.bin", "rb");
            File dst = SPIFFS.open("/fr.tmp", "wb");
            bool found = false;
            while(src && dst && src.available()) {
                enrolled_face_t proto;
                src.read((uint8_t*)&proto, sizeof(proto));
                if (String(proto.name) == String(httpCommandName)) {
                    found = true;
                } else {
                    dst.write((uint8_t*)&proto, sizeof(proto));
                }
            }
            if(src) src.close();
            if(dst) dst.close();
            if (found) {
                SPIFFS.remove("/fr.bin");
                SPIFFS.rename("/fr.tmp", "/fr.bin");
                recognition.begin();
            } else {
                SPIFFS.remove("/fr.tmp");
            }
            break;
        }
        case 'e': {
            File src = SPIFFS.open("/fr.bin", "rb");
            File dst = SPIFFS.open("/fr.tmp", "wb");
            bool found = false;
            while(src && dst && src.available()) {
                enrolled_face_t proto;
                src.read((uint8_t*)&proto, sizeof(proto));
                if (String(proto.name) == String(httpCommandName)) {
                    found = true;
                    String newName = String(httpNewCommandName);
                    for (uint8_t i = 0; i < newName.length() && i < 16; i++)
                        proto.name[i] = newName[i];
                    proto.name[newName.length() < 16 ? newName.length() : 16] = '\0';
                }
                dst.write((uint8_t*)&proto, sizeof(proto));
            }
            if(src) src.close();
            if(dst) dst.close();
            if (found) {
                SPIFFS.remove("/fr.bin");
                SPIFFS.rename("/fr.tmp", "/fr.bin");
                recognition.begin();
            } else {
                SPIFFS.remove("/fr.tmp");
            }
            break;
        }
        case 'r': modoContinuo = true;  Serial.println(">> MODO CONTINUO"); break;
        case 'p': modoContinuo = false; Serial.println(">> PAUSADO"); break;
        case 't': pedidoReconhecimento = true; break;
        case 'd': recognition.dump(); break;
        case 'z': sharpMax = 0; Serial.println(">> pico zerado"); break;
        case 'l': modoContinuo = false; testeLuz(); break;
      }
    }

    if (pedidoReconhecimento) {
      pedidoReconhecimento = false;
      runTentativa();
    }

      if (pedidoAcessoRemoto) {
      pedidoAcessoRemoto = false;
      liberarAcessoRemoto();
    }

    // captura + publica sempre (feed vivo)
    if (!camera.capture().isOk()) { vTaskDelay(pdMS_TO_TICKS(30)); continue; }
    publishFrame(camera.frame->buf, camera.frame->len);

    uint32_t sharp = camera.frame->len;
    if (sharp > sharpMax) sharpMax = sharp;

    if (modoContinuo) runRecognition();
    else updateGInfo(0, sharp, sharpMax, lerLDR(), "(pausado)", "-");

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}


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

  fechaduraServo.write(SERVO_FECHADO);  // garante que dorme trancada
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
  estadoLED = LED_OFF;
  if (handleLED) vTaskSuspend(handleLED);   // <-- para o analogWrite continuo
  analogWrite(pinoAzul, 0);
  digitalWrite(pinoAzul, LOW);
  analogWrite(pinoLuz, 0);
  digitalWrite(pino, HIGH);
  tocarMelodia(melodia);

  bool aprovado = (pino == pinoVerde);
  if (aprovado) fechaduraServo.write(SERVO_ABERTO);

  uint32_t t0 = millis();
  uint32_t hold = aprovado ? TEMPO_ABERTO_MS : 2500;
  while (millis() - t0 < hold) {
    if (camera.capture().isOk()) publishFrame(camera.frame->buf, camera.frame->len);
    delay(20);
  }

  if (aprovado) fechaduraServo.write(SERVO_FECHADO);
  digitalWrite(pino, LOW);
  if (handleLED) vTaskResume(handleLED);    // <-- devolve o azul
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
  String nomeVotado = "";   // identidade travada no 1o voto valido
  uint32_t t0 = millis();
  const char *motivo = "";

  Serial.println(">> ATTEMPT started");
  estadoLED = LED_RESPIRANDO;
  analogWrite(pinoLuz, dutyLuz);

  while (validos < JANELA_N) {

    if (millis() - t0 > TIMEOUT_MS) {
      Serial.printf(">> TIMEOUT (only %d/%d valid frames)\n", validos, JANELA_N);
      sinalizaResultado(pinoVermelho, somAcessoNegado);
      lastAccType = "denied";
      lastAccName = "timeout";
      return EXPIROU;
    }

    if (!camera.capture().isOk()) {
      delay(20);
      continue;
    }
    publishFrame(camera.frame->buf, camera.frame->len);

    ajustaLuz();

    if (!frameOk(motivo)) {
      Serial.printf("   Frame discarded: %s\n", motivo);
      continue;
    }

    if (!recognition.detect().isOk())
      continue;
    if (!recognition.recognize().isOk())
      continue;

    validos++;
    const char *nome = recognition.match.name.c_str();
    float sim = recognition.match.similarity;

    // Voto: QUALQUER pessoa cadastrada serve, desde que sim >= PISO_SIM.
    // A identidade e travada no primeiro voto valido -- os K votos precisam
    // ser da MESMA pessoa, senao ruido em nomes diferentes somaria votos
    // e liberaria por engano.
    bool aFavor = false;
    if (sim >= PISO_SIM) {
      if (nomeVotado.length() == 0) {
        nomeVotado = nome;       // trava na primeira identidade valida
        aFavor = true;
      } else if (nomeVotado == nome) {
        aFavor = true;           // mesmo alvo: conta
      }
      // nome diferente do travado: nao conta (frame neutro)
    }
    if (aFavor)
      votosFavor++;

    Serial.printf("   Frame %d/%d: %s sim=%.3f -> %s (votes=%d for %s)\n",
                  validos, JANELA_N, nome, sim, aFavor ? "VOTE" : "REJECTED",
                  votosFavor, nomeVotado.length() ? nomeVotado.c_str() : "-");

    // EARLY-EXIT
    if (votosFavor >= VOTOS_K) {
      Serial.printf(">> APPROVED (early exit: %d votes in %d frames for %s)\n",
                    votosFavor, validos, nomeVotado.c_str());
      sinalizaResultado(pinoVerde, somPortaAberta);
      lastAccType = "granted";
      lastAccName = nomeVotado;
      return APROVADO;
    }

    // EARLY-FAIL
    int restantes = JANELA_N - validos;
    if (votosFavor + restantes < VOTOS_K) {
      Serial.printf(">> DENIED (early fail: %d votes, %d frames remaining)\n",
                    votosFavor, restantes);
      sinalizaResultado(pinoVermelho, somAcessoNegado);
      lastAccType = "denied";
      lastAccName = "unknown";
      return NEGADO;
    }
  }

  // Rede de seguranca (nao deveria chegar aqui com os early-exit/fail).
  if (votosFavor >= VOTOS_K) {
    Serial.printf(">> APPROVED (%d/%d votes for %s)\n",
                  votosFavor, validos, nomeVotado.c_str());
    sinalizaResultado(pinoVerde, somPortaAberta);
    lastAccType = "granted";
    lastAccName = nomeVotado;
    return APROVADO;
  }

  Serial.printf(">> DENIED (%d/%d votes, %d required)\n",
                votosFavor, validos, VOTOS_K);
  sinalizaResultado(pinoVermelho, somAcessoNegado);
  lastAccType = "denied";
  lastAccName = "unknown";
  return NEGADO;
}

void liberarAcessoRemoto() {
  Serial.println(">> REMOTE ACCESS granted via dashboard");
  sinalizaResultado(pinoVerde, somPortaAberta);
  lastAccType = "granted";
  lastAccName = "Remote";
}

// ==================== BUFFER COMPARTILHADO DO STREAM ====================
// O loop principal escreve aqui; a task do servidor HTTP le. Sao contextos
// diferentes (FreeRTOS), por isso o mutex.
static uint8_t *g_jpg = nullptr; // copia do ultimo frame (PSRAM)
static size_t g_jpgLen = 0;
static volatile uint32_t g_frameId = 0; // contador: sinaliza frame novo
static SemaphoreHandle_t g_mutex = nullptr;
static SemaphoreHandle_t g_infoMutex = nullptr;  
static char g_info[512] =
    "{\"face\":0,\"sharp\":0,\"peak\":0,\"ldr\":0,\"name\":\"-\",\"sim\":\"-\","
    "\"last_acc\":\"-\",\"last_name\":\"-\",\"enroll_status\":\"idle\",\"enroll_msg\":\"-\"}";
static httpd_handle_t g_server = nullptr;
static httpd_handle_t g_stream = nullptr;

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

void updateGInfo(int face, uint32_t sharp, uint32_t peak, uint16_t ldr, const char* name, const char* sim) {
  char tmp[512];
  snprintf(tmp, sizeof(tmp),
           "{\"face\":%d,\"sharp\":%u,\"peak\":%u,\"ldr\":%u,\"name\":\"%s\","
           "\"sim\":\"%s\",\"last_acc\":\"%s\",\"last_name\":\"%s\","
           "\"enroll_status\":\"%s\",\"enroll_msg\":\"%s\"}",
           face, (unsigned)sharp, (unsigned)peak, (unsigned)ldr, name, sim,
           lastAccType.c_str(), lastAccName.c_str(),
           enrollStatus.c_str(), enrollMsg.c_str());
  if (g_infoMutex && xSemaphoreTake(g_infoMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    memcpy(g_info, tmp, strlen(tmp) + 1);
    xSemaphoreGive(g_infoMutex);
  }
}

void updateEnrollStatus(const char* status, const char* msg) {
  enrollStatus = status;
  enrollMsg = msg;
  uint32_t sharp = (camera.frame != nullptr) ? camera.frame->len : 0;
  updateGInfo(0, sharp, sharpMax, lerLDR(), "-", "-");
}

// ==================== HTTP ====================

/** /info -> JSON com as metricas. no-store pro navegador nao cachear. */
static esp_err_t infoHandler(httpd_req_t *req) {
  char local[512];
  if (g_infoMutex && xSemaphoreTake(g_infoMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    memcpy(local, g_info, sizeof(local));
    xSemaphoreGive(g_infoMutex);
  } else {
    strcpy(local, "{}");
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, local, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t facesHandler(httpd_req_t *req) {
  String json = "[";
  File file = SPIFFS.open("/fr.bin", "rb");
  bool first = true;
  String vistos[32];      // nomes ja adicionados, evita duplicata no multi-enroll
  int totalVistos = 0;

  while(file && file.available()) {
    enrolled_face_t proto;
    file.read((uint8_t*)&proto, sizeof(proto));
    if (proto.ctrl[0] != 0x14 || proto.ctrl[1] != 0x08) break; // Parse error

    String nomeAtual = String(proto.name);

    // pula se esse nome ja foi listado (multi-enroll grava varios embeddings
    // com o MESMO nome, um por captura -- sem isso o dashboard mostraria
    // "joao, joao, joao" em vez de so "joao")
    bool jaVisto = false;
    for (int i = 0; i < totalVistos; i++) {
      if (vistos[i] == nomeAtual) { jaVisto = true; break; }
    }
    if (jaVisto) continue;

    if (totalVistos < 32) vistos[totalVistos++] = nomeAtual;

    if (!first) json += ",";
    json += "\"" + nomeAtual + "\"";
    first = false;
  }
  if(file) file.close();
  json += "]";
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, json.c_str(), json.length());
}

static esp_err_t controlHandler(httpd_req_t *req) {
  char buf[128];
  if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
    char val[16];
    if (httpd_query_key_value(buf, "cmd", val, sizeof(val)) == ESP_OK) {
      httpCommand = val[0];
    }
    char nameVal[64];
    if (httpd_query_key_value(buf, "name", nameVal, sizeof(nameVal)) == ESP_OK) {
      // decodifica URL: %XX -> byte, '+' -> espaco
      char dec[64]; int j = 0;
      for (int i = 0; nameVal[i] && j < (int)sizeof(dec) - 1; i++) {
        if (nameVal[i] == '+') {
          dec[j++] = ' ';
        } else if (nameVal[i] == '%' && nameVal[i+1] && nameVal[i+2]) {
          auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return 0;
          };
          dec[j++] = (char)(hex(nameVal[i+1]) * 16 + hex(nameVal[i+2]));
          i += 2;
        } else {
          dec[j++] = nameVal[i];
        }
      }
      dec[j] = '\0';
      strncpy(httpCommandName, dec, sizeof(httpCommandName) - 1);
      httpCommandName[sizeof(httpCommandName) - 1] = '\0';
    } else {
      httpCommandName[0] = '\0';
    }
    char newNameVal[64];
    if (httpd_query_key_value(buf, "newname", newNameVal, sizeof(newNameVal)) == ESP_OK) {
      char dec[64]; int j = 0;
      for (int i = 0; newNameVal[i] && j < (int)sizeof(dec) - 1; i++) {
        if (newNameVal[i] == '+') dec[j++] = ' ';
        else if (newNameVal[i] == '%' && newNameVal[i+1] && newNameVal[i+2]) {
          auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return 0;
          };
          dec[j++] = (char)(hex(newNameVal[i+1]) * 16 + hex(newNameVal[i+2]));
          i += 2;
        } else {
          dec[j++] = newNameVal[i];
        }
      }
      dec[j] = '\0';
      strncpy(httpNewCommandName, dec, sizeof(httpNewCommandName) - 1);
      httpNewCommandName[sizeof(httpNewCommandName) - 1] = '\0';
    } else {
      httpNewCommandName[0] = '\0';
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

static esp_err_t indexHandler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  // no-store: impede o navegador de servir uma versao velha em cache
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");

  // Manda o HTML em pedacos de 1KB (chunked). Pagina grande as vezes
  // falha quando enviada de uma vez so; em pedacos e robusto.
  const size_t CHUNK = 1024;
  size_t total = strlen(INDEX_HTML);
  size_t sent = 0;
  while (sent < total) {
    size_t n = (total - sent > CHUNK) ? CHUNK : (total - sent);
    if (httpd_resp_send_chunk(req, INDEX_HTML + sent, n) != ESP_OK) {
      return ESP_FAIL;
    }
    sent += n;
  }
  httpd_resp_send_chunk(req, NULL, 0); // finaliza a resposta
  return ESP_OK;
}

void startServer() {
  // Porta 80: pagina, /info, /control
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 80;
  cfg.ctrl_port = 32768;
  cfg.max_uri_handlers = 8;
  cfg.stack_size = 8192;
  //cfg.stack_size = 6144;
  //cfg.stack_size = 4096;
  cfg.lru_purge_enable = true;
  cfg.recv_wait_timeout = 10;   // segundos: nao derruba conexao lenta no meio
  cfg.send_wait_timeout = 10;   // idem no envio da pagina grande
  cfg.core_id = 0;  
  cfg.max_open_sockets = 5;   // menos chance de recusar conexao nova sob carga

  if (httpd_start(&g_server, &cfg) != ESP_OK) {
    Serial.println("ERRO: httpd porta 80 falhou");
    return;
  }

  httpd_uri_t u1 = {"/", HTTP_GET, indexHandler, nullptr};
  httpd_uri_t u3 = {"/info", HTTP_GET, infoHandler, nullptr};
  httpd_uri_t u4 = {"/control", HTTP_GET, controlHandler, nullptr};
  httpd_uri_t u5 = {"/faces", HTTP_GET, facesHandler, nullptr};
  httpd_register_uri_handler(g_server, &u1);
  httpd_register_uri_handler(g_server, &u3);
  httpd_register_uri_handler(g_server, &u4);
  httpd_register_uri_handler(g_server, &u5);

  // Porta 81: SO o stream MJPEG
  httpd_config_t cfg2 = HTTPD_DEFAULT_CONFIG();
  cfg2.server_port = 81;
  cfg2.ctrl_port = 32769;
  cfg2.max_uri_handlers = 2;
  cfg2.stack_size = 8192;
  //cfg2.stack_size = 6144;
  //cfg2.stack_size = 4096;
  cfg2.lru_purge_enable = true;
  cfg2.core_id = 0;              // <-- NOVA
  cfg2.recv_wait_timeout = 10;   // <-- NOVA
  cfg2.send_wait_timeout = 10;   // <-- NOVA

  if (httpd_start(&g_stream, &cfg2) != ESP_OK) {
    Serial.println("ERRO: httpd porta 81 falhou");
    return;
  }

  httpd_uri_t u2 = {"/stream", HTTP_GET, streamHandler, nullptr};
  httpd_register_uri_handler(g_stream, &u2);
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
  camera.resolution.face();  // resolucao qvga 320x240
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

  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);
  fechaduraServo.setPeriodHertz(50);             // servo padrao = 50Hz
  fechaduraServo.attach(pinoServo, 500, 2400);   // faixa de pulso em us
  fechaduraServo.write(SERVO_FECHADO);      // nasce trancada e assim fica no boot

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
  g_infoMutex = xSemaphoreCreateMutex();
  g_jpg = (uint8_t *)ps_malloc(JPG_CAP); // PSRAM: 40KB nao cabe na RAM interna
  if (!g_jpg) {
    Serial.println("ERRO: ps_malloc falhou. PSRAM habilitada? (OPI PSRAM)");
    while (true)
      delay(1000); // trava proposital: sem buffer nao roda
  }

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);   // <-- LINHA NOVA
  WiFi.persistent(true);
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

  // mDNS: dá um nome amigável à placa. Acesse http://fechadura.local
  if (MDNS.begin("FaceGuard")) {
    MDNS.addService("http", "tcp", 80);   // registra o servidor web
    Serial.println(">>> Nome de rede: http://FaceGuard.local");
  } else {
    Serial.println(">>> AVISO: mDNS falhou (use o IP abaixo)");
  }

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
  Serial.println("  x = cancelar cadastro");
  Serial.println();
  Serial.println("BOTAO  GPIO21: aperte para iniciar reconhecimento");
  Serial.println("SWITCH GPIO14: fechado = dorme | aberto = acorda");
  Serial.println("LDR    GPIO2  | COB/MOSFET GPIO42 | LED verde GPIO48");
  Serial.println();

  xTaskCreatePinnedToCore(tarefaLED,    "led",     2048, NULL, 3, &handleLED,    1);
xTaskCreatePinnedToCore(tarefaCamera, "camera", 16384, NULL, 1, &handleCamera, 1);
}

// ==================== LOOP ====================
void loop() {

  static uint32_t lastWifiCheck = 0;
  if (millis() - lastWifiCheck > 5000) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println(">> WiFi caiu, reconectando...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
  }
  // switch -> deep sleep (com debounce)
  if (digitalRead(pinoDeepSleep) == LOW) {
    delay(50);
    if (digitalRead(pinoDeepSleep) == LOW) entrarEmDeepSleep();
  }

  // serial -> vira comando no mesmo canal do web
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.startsWith("s")) entrarEmDeepSleep();          // sono e imediato
    else if (cmd.length() > 0) { httpCommand = cmd[0]; httpCommandName[0] = '\0'; }
  }

  // botao -> pedido (borda de descida)
  static bool botaoUltimoEstado = HIGH;
  bool botaoAtual = digitalRead(iniciarReconhecimento);
  if (botaoUltimoEstado == HIGH && botaoAtual == LOW) pedidoReconhecimento = true;
  botaoUltimoEstado = botaoAtual;

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
    updateGInfo(0, sharp, sharpMax, ldr, "-", "-");
    return;
  }

  // Rosto detectado, mas sem match (face:1, name:"?").
  if (!recognition.recognize().isOk()) {
    updateGInfo(1, sharp, sharpMax, ldr, "?", "-");
    return;
  }

  const char *name = recognition.match.name.c_str();
  float sim = recognition.match.similarity;
  char simStr[16];
  snprintf(simStr, sizeof(simStr), "%.4f", sim);

  updateGInfo(1, sharp, sharpMax, ldr, name, simStr);

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

  Serial.println("Position your face. Registering in 3 seconds...");
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
    Serial.printf("ERROR: Poor-quality frame for enrollment (%s). Please try again.\n",
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
void enrollMultiplo(int alvo, String defaultName) {
  String nome = defaultName;
  if (nome == "") {
    nome = prompt("Nome para cadastro multiplo:");
  }

  Serial.printf(">> Multi-enrollment for '%s' (target: %d)\n", nome.c_str(), alvo);
  int ok = 0;
  int tentativas = 0;
  const int MAX_TENTATIVAS = alvo * 3;
  const char *motivo = "";
  char buf[96];   // buffer fixo reutilizado -> nao fragmenta o heap

  estadoLED = LED_RESPIRANDO;
  analogWrite(pinoLuz, dutyLuz);

  while (ok < alvo) {
    Serial.printf("   [HEAP livre: %u | maior bloco: %u]\n",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    //vTaskDelay(10 / portTICK_PERIOD_MS);
    
    yield();
    
    if (httpCommand == 'x') {
        cancelarCadastro = true;
        httpCommand = 0; // Limpa a variável para não processar o 'x' duas vezes
    }

    //server.handleClient();

    if (cancelarCadastro) {
      updateEnrollStatus("cancelled", "Enrollment cancelled.");
      Serial.println(">> cancelled");
      estadoLED = LED_OFF; analogWrite(pinoAzul, 0); analogWrite(pinoLuz, 0);
      return;
    }
    if (tentativas >= MAX_TENTATIVAS) {
      snprintf(buf, sizeof(buf), "Failed: only %d/%d after %d attempts", ok, alvo, tentativas);
      updateEnrollStatus("failed", buf);
      Serial.printf(">> %s\n", buf);
      estadoLED = LED_OFF; analogWrite(pinoAzul, 0); analogWrite(pinoLuz, 0);
      return;
    }
    tentativas++;

    snprintf(buf, sizeof(buf), "Capture %d/%d (attempt %d) - hold STILL...", ok + 1, alvo, tentativas);
    updateEnrollStatus("capturing", buf);
    Serial.printf("   %s\n", buf);

    for (int k = 0; k < 12; k++) {
      if (camera.capture().isOk()) publishFrame(camera.frame->buf, camera.frame->len);
      ajustaLuz();
      delay(100);
    }

    if (!camera.capture().isOk()) {
      updateEnrollStatus("capturing", "Capture failed, retrying");
      continue;
    }
    publishFrame(camera.frame->buf, camera.frame->len);

    if (!frameOk(motivo)) {
      snprintf(buf, sizeof(buf), "Discarded (%s), retrying", motivo);
      updateEnrollStatus("capturing", buf);
      continue;
    }

    if (!recognition.detect().isOk()) {
      updateEnrollStatus("capturing", "No face detected, retrying");
      continue;
    }

    if (recognition.enroll(nome).isOk()) {
      ok++;
      snprintf(buf, sizeof(buf), "OK (%d/%d)", ok, alvo);
      updateEnrollStatus("capturing", buf);
      Serial.printf("   %s\n", buf);
    } else {
      updateEnrollStatus("capturing", "enroll error, retrying");
      Serial.println(recognition.exception.toString());
    }
  }

  snprintf(buf, sizeof(buf), "Enrollment done: %d/%d for '%s'", ok, alvo, nome.c_str());
  updateEnrollStatus("success", buf);
  Serial.printf(">> %s\n", buf);
  estadoLED = LED_OFF; analogWrite(pinoAzul, 0); analogWrite(pinoLuz, 0);
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
