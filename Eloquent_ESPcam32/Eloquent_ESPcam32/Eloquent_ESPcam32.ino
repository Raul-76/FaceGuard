/**
 * ESP32-S3 Face Recognition — DIAGNÓSTICO
 * Freenove ESP32-S3-WROOM CAM
 *
 * Objetivo: medir a distribuição de similaridade
 * (mesma pessoa vs. impostores) sem o prompt no meio.
 *
 * Core 2.0.14 | OPI PSRAM | Huge APP | Serial 115200 + New Line
 */

#include <eloquent_esp32cam.h>
#include <eloquent_esp32cam/face/detection.h>
#include <eloquent_esp32cam/face/recognition.h>

using eloq::camera;
using eloq::face::detection;
using eloq::face::recognition;

String prompt(String message);
void doEnroll();
void runLoop();

bool modoContinuo = false;



void setup() {
    delay(4000);
    Serial.begin(115200);
    Serial.println("=== DIAGNOSTICO ===");

    camera.pinout.freenove_s3();
    camera.brownout.disable();
    camera.resolution.face();
    camera.quality.high();

    detection.accurate();
    detection.confidence(0.7);

    // threshold BAIXO de proposito: queremos VER os numeros,
    // nao filtrar. Filtragem vem depois, com base nos dados.
    recognition.confidence(0.10);

    while (!camera.begin().isOk())
        Serial.println(camera.exception.toString());

    while (!recognition.begin().isOk())
        Serial.println(recognition.exception.toString());

    Serial.println("Camera OK / Recognizer OK");

    if (prompt("Apagar cadastros? [s|n]").startsWith("s")) {
        recognition.deleteAll();
        Serial.println("Apagado.");
    }

    Serial.println();
    Serial.println("COMANDOS (digite a qualquer momento):");
    Serial.println("  c = cadastrar rosto");
    Serial.println("  r = MODO CONTINUO (imprime similaridade a cada frame)");
    Serial.println("  p = pausar modo continuo");
    Serial.println("  d = listar cadastrados");
    Serial.println();
}


void loop() {
    // le comando SEM bloquear
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();

        if (cmd.startsWith("c")) { modoContinuo = false; doEnroll(); }
        else if (cmd.startsWith("r")) { modoContinuo = true;  Serial.println(">> MODO CONTINUO"); }
        else if (cmd.startsWith("p")) { modoContinuo = false; Serial.println(">> PAUSADO"); }
        else if (cmd.startsWith("d")) { recognition.dump(); }
    }

    if (modoContinuo)
        runLoop();
    else
        delay(50);
}


/**
 * Captura + detecta + reconhece NO MESMO INSTANTE.
 * Sem prompt no meio -> frame sempre fresco.
 */
void runLoop() {
    if (!camera.capture().isOk())
        return;

    if (!recognition.detect().isOk()) {
        Serial.println("[sem rosto]");
        delay(300);
        return;
    }

    if (!recognition.recognize().isOk()) {
        Serial.print("[nao reconhecido] ");
        Serial.println(recognition.exception.toString());
        delay(300);
        return;
    }

    Serial.print("MATCH: ");
    Serial.print(recognition.match.name.c_str());
    Serial.print("  sim=");
    Serial.print(recognition.match.similarity, 4);   // 4 casas!
    Serial.print("  (");
    Serial.print(recognition.benchmark.millis());
    Serial.println("ms)");

    delay(400);
}


/**
 * Cadastro: captura o frame NA HORA, depois pergunta o nome.
 * (ordem invertida em relacao ao exemplo original)
 */
void doEnroll() {
    String name = prompt("Nome:");

    Serial.println("Posicione o rosto. Cadastrando em 3s...");
    delay(3000);

    if (!camera.capture().isOk()) {
        Serial.println("ERRO: captura falhou");
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