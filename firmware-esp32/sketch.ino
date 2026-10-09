/*
 * Semáforo Inteligente - Firmware do ESP32
 * Disciplina: Sistemas Embarcados (Automação e Conectividade) - IFPE Garanhuns
 *
 * O ESP32 recebe pelo MQTT a configuração enviada pela plataforma web
 * (tempos de carro/pedestre, horário de pico e status) e controla os LEDs,
 * reduzindo o tempo do vermelho quando o sensor HC-SR04 detecta presença.
 *
 * Itens da R5 identificados nos comentários:
 *  A. configuração dos pinos
 *  B. inicialização dos sensores
 *  C. variáveis utilizadas
 *  D. comandos responsáveis pela leitura
 *  E. estruturas condicionais utilizadas
 *  F. acionamento de atuadores
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>

// =====================================================
// A. CONFIGURAÇÃO DOS PINOS
// =====================================================
const int PIN_TRIG = 5;    // HC-SR04: TRIG (saída do ESP32, dispara o pulso)
const int PIN_ECHO = 18;   // HC-SR04: ECHO (entrada do ESP32, recebe o eco)
const int LED_R = 25;      // LED vermelho (atuador)
const int LED_Y = 26;      // LED amarelo  (atuador)
const int LED_G = 27;      // LED verde    (atuador)

// =====================================================
// C. VARIÁVEIS UTILIZADAS
// =====================================================
// --- Rede e MQTT ---
const char* WIFI_SSID = "Wokwi-GUEST";          // rede Wi-Fi simulada do Wokwi
const char* WIFI_PASS = "";
const char* MQTT_HOST = "broker.hivemq.com";    // mesmo broker usado pela plataforma web
const int   MQTT_PORT = 1883;
const char* TOPICO    = "Ifpe/Semaforo/Semaforo1";  // tópico publicado pela plataforma

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// --- Estado do semáforo ---
enum Estado { VERDE, AMARELO, VERMELHO };
Estado estado = VERDE;                // fase atual
unsigned long inicioEstado = 0;       // instante (ms) em que a fase atual começou

// --- Tempos (ms). Valores iniciais; são substituídos pela config recebida via MQTT ---
unsigned long tVerdePadrao    = 25000;   // verde dos carros fora do pico
unsigned long tVermelhoPadrao = 25000;   // vermelho (verde do pedestre) fora do pico
unsigned long tVerdePico      = 35000;   // verde dos carros no horário de pico
unsigned long tVermelhoPico   = 15000;   // vermelho no horário de pico
const unsigned long tAmarelo  = 3000;    // amarelo fixo

// --- Horário de pico (minutos desde 00:00) e status ---
int picoInicio = 7 * 60;
int picoFim    = 9 * 60;
bool manutencao = false;              // true quando o status recebido é "manutencao"

// --- Sensor ---
bool presenca = false;                // true quando há veículo/pedestre próximo
const int DIST_PRESENCA_CM = 50;      // distância (cm) abaixo da qual há presença

// --- Controle de tempo (sem delay, para não travar o semáforo) ---
unsigned long ultimaLeitura = 0;      // última leitura do sensor
unsigned long ultimaTentativa = 0;    // última tentativa de reconexão MQTT
unsigned long ultimoPisca = 0;        // controle do amarelo piscante (manutenção)

// Converte "HH:MM" em minutos desde a meia-noite
int paraMinutos(const char* hhmm) {
  int h = 0, m = 0;
  sscanf(hhmm, "%d:%d", &h, &m);
  return h * 60 + m;
}

// =====================================================
// MQTT: recebe a configuração (JSON) publicada pela plataforma web
// Exemplo de mensagem:
// {"tempoPadrao":{"carro":25,"pedestre":25},
//  "tempoHorarioPico":{"carro":35,"pedestre":15},
//  "horarioPico":{"inicio":"07:00","fim":"09:00"},
//  "statusOperacional":"ativo"}
// =====================================================
void callback(char* topic, byte* payload, unsigned int len) {
  JsonDocument doc;
  DeserializationError erro = deserializeJson(doc, payload, len);

  // E. condicional: ignora mensagens que não são JSON válido
  if (erro) {
    Serial.print("JSON invalido: ");
    Serial.println(erro.c_str());
    return;
  }

  // Lê os campos; o valor após "|" é usado se o campo não existir
  tVerdePadrao    = (doc["tempoPadrao"]["carro"]        | 25) * 1000UL;
  tVermelhoPadrao = (doc["tempoPadrao"]["pedestre"]     | 25) * 1000UL;
  tVerdePico      = (doc["tempoHorarioPico"]["carro"]   | 35) * 1000UL;
  tVermelhoPico   = (doc["tempoHorarioPico"]["pedestre"]| 15) * 1000UL;
  picoInicio = paraMinutos(doc["horarioPico"]["inicio"] | "07:00");
  picoFim    = paraMinutos(doc["horarioPico"]["fim"]    | "09:00");
  manutencao = (strcmp(doc["statusOperacional"] | "ativo", "manutencao") == 0);

  Serial.println("Nova configuracao recebida via MQTT.");
  Serial.printf("Padrao: verde=%lus vermelho=%lus | Pico: verde=%lus vermelho=%lus | Manutencao=%d\n",
                tVerdePadrao / 1000, tVermelhoPadrao / 1000,
                tVerdePico / 1000, tVermelhoPico / 1000, manutencao);

  // E. condicional: ao sair da manutenção, reinicia o ciclo normal
  if (!manutencao) {
    estado = VERDE;
    inicioEstado = millis();
    aplicaLeds();
  }
}

// Conecta ao Wi-Fi (chamada a cada loop; reconecta se cair)
void conectaWifi() {
  if (WiFi.status() == WL_CONNECTED) return;   // E. condicional: já conectado
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

// Conecta ao broker MQTT e assina o tópico (nova tentativa a cada 3 s)
void conectaMqtt() {
  if (WiFi.status() != WL_CONNECTED || mqtt.connected()) return;
  if (millis() - ultimaTentativa < 3000) return;
  ultimaTentativa = millis();
  String id = "esp32-semaforo-" + String(random(0xffff), HEX);
  if (mqtt.connect(id.c_str())) {
    mqtt.subscribe(TOPICO);
    Serial.println("MQTT conectado e inscrito no topico.");
  }
}

// =====================================================
// D. COMANDOS RESPONSÁVEIS PELA LEITURA
// Lê o HC-SR04 e devolve a distância em centímetros
// =====================================================
long lerDistanciaCm() {
  digitalWrite(PIN_TRIG, LOW);          // garante nível baixo antes do pulso
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);         // pulso de 10 us dispara a medição
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  long us = pulseIn(PIN_ECHO, HIGH, 30000);  // tempo do eco (timeout 30 ms)
  if (us == 0) return 999;              // E. condicional: sem eco = nada por perto
  return us / 58;                       // converte microssegundos em centímetros
}

// =====================================================
// E. ESTRUTURAS CONDICIONAIS
// Informa se o horário atual (via NTP) está dentro do horário de pico
// =====================================================
bool emHorarioPico() {
  struct tm t;
  if (!getLocalTime(&t, 0)) return false;       // sem hora ainda: usa tempos padrão
  int agora = t.tm_hour * 60 + t.tm_min;
  if (picoInicio <= picoFim) return agora >= picoInicio && agora <= picoFim;
  return agora >= picoInicio || agora <= picoFim;   // pico que atravessa a meia-noite
}

// Define quanto tempo a fase atual deve durar
unsigned long duracaoAtual() {
  bool pico = emHorarioPico();
  switch (estado) {                     // E. condicional: duração por fase
    case VERDE:   return pico ? tVerdePico : tVerdePadrao;
    case AMARELO: return tAmarelo;
    default: {
      unsigned long base = pico ? tVermelhoPico : tVermelhoPadrao;
      // Com presença detectada, reduz o tempo de espera no vermelho pela metade
      return presenca ? base / 2 : base;
    }
  }
}

// =====================================================
// F. ACIONAMENTO DE ATUADORES (LEDs)
// =====================================================
// Liga apenas o LED correspondente à fase atual
void aplicaLeds() {
  digitalWrite(LED_R, estado == VERMELHO);
  digitalWrite(LED_Y, estado == AMARELO);
  digitalWrite(LED_G, estado == VERDE);
}

// Modo manutenção: amarelo piscando, demais LEDs apagados
void modoManutencao() {
  digitalWrite(LED_R, LOW);
  digitalWrite(LED_G, LOW);
  if (millis() - ultimoPisca >= 500) {
    ultimoPisca = millis();
    digitalWrite(LED_Y, !digitalRead(LED_Y));
  }
}

void setup() {
  Serial.begin(115200);

  // A. Modo de cada pino
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(LED_R, OUTPUT);
  pinMode(LED_Y, OUTPUT);
  pinMode(LED_G, OUTPUT);

  // B. INICIALIZAÇÃO DOS SENSORES: TRIG em nível baixo e tempo
  // para o HC-SR04 estabilizar antes da primeira leitura
  digitalWrite(PIN_TRIG, LOW);
  delay(50);

  // Wi-Fi, hora (NTP, fuso de Brasília UTC-3) e MQTT
  conectaWifi();
  configTime(-3 * 3600, 0, "pool.ntp.org");
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(512);              // a mensagem da plataforma passa de 256 bytes
  mqtt.setCallback(callback);

  inicioEstado = millis();
  aplicaLeds();                         // F. começa com o LED verde aceso
}

void loop() {
  conectaWifi();
  conectaMqtt();
  mqtt.loop();                          // processa mensagens MQTT recebidas

  // D. Leitura do sensor a cada 200 ms
  if (millis() - ultimaLeitura >= 200) {
    ultimaLeitura = millis();
    long d = lerDistanciaCm();
    bool novo = (d < DIST_PRESENCA_CM); // E. condicional: presença se estiver perto

    if (novo != presenca) {             // E. condicional: só avisa quando muda
      presenca = novo;
      Serial.printf("Distancia: %ld cm | presenca: %s\n", d, presenca ? "SIM" : "NAO");
    }
  }

  // E. condicional: em manutenção, só pisca o amarelo
  if (manutencao) {
    modoManutencao();
    return;
  }

  // E. Troca de fase quando o tempo acaba
  if (millis() - inicioEstado >= duracaoAtual()) {
    estado = (Estado)((estado + 1) % 3);   // VERDE -> AMARELO -> VERMELHO -> VERDE
    inicioEstado = millis();
    aplicaLeds();                          // F. atualiza os LEDs
    Serial.printf("Estado: %s\n", estado == VERDE ? "VERDE" : estado == AMARELO ? "AMARELO" : "VERMELHO");
  }
}
