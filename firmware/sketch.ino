/*
  Sistema de Detecção de Excesso Grave de Velocidade (ODS 3, meta 3.6)
  Universidade Presbiteriana Mackenzie · Objetos Inteligentes Conectados

  ESP32 + 2 sensores IR de barreira (no Wokwi: 2 pushbuttons)
  + LED verde, LED vermelho e buzzer + Wi-Fi + MQTT (PubSubClient)

  Tópicos MQTT (broker test.mosquitto.org:1883):
    MACK10433738/DADOS    -> ESP32 publica toda medição (JSON)
    MACK10433738/ALERTA   -> ESP32 publica só as medições CRÍTICAS
    MACK10433738/COMANDO  <- Node-RED envia "RESET" para apagar LEDs e buzzer

  Teste pelo Monitor Serial: 1 = 40 km/h · 2 = 65 km/h · 3 = 90 km/h · 4 = tempos médios
*/

#include <WiFi.h>
#include <PubSubClient.h>

// ===================== PINOS =====================
const int SENSOR_1     = 32;  // sensor IR 1 (botão 1 no Wokwi)
const int SENSOR_2     = 33;  // sensor IR 2 (botão 2 no Wokwi)
const int LED_VERDE    = 19;
const int LED_VERMELHO = 18;
const int BUZZER       = 22;

// ===================== REGRAS ====================
const float DISTANCIA_SENSORES = 0.50;                     // metros entre os sensores
const float LIMITE_VELOCIDADE  = 60.0;                     // km/h
const float LIMITE_CRITICO     = LIMITE_VELOCIDADE * 1.30; // 30% acima = 78 km/h
const unsigned long TIMEOUT_MEDICAO_US = 30000000UL;       // 30 s esperando o sensor 2

// ===================== REDE ======================
const char* WIFI_SSID      = "Wokwi-GUEST";
const char* WIFI_PASSWORD  = "";
const char* MQTT_SERVER    = "test.mosquitto.org";
const int   MQTT_PORT      = 1883;
const char* TOPICO_DADOS   = "MACK10433738/DADOS";
const char* TOPICO_ALERTA  = "MACK10433738/ALERTA";
const char* TOPICO_COMANDO = "MACK10433738/COMANDO";

WiFiClient espClient;
PubSubClient mqttClient(espClient);

// ===================== ESTADO ====================
unsigned long tempoSensor1 = 0;
unsigned long tempoSensor2 = 0;
unsigned long instanteDeteccaoUs = 0;   // momento em que a medição terminou
bool aguardandoSensor2 = false;
int estadoAnteriorSensor1 = HIGH;
int estadoAnteriorSensor2 = HIGH;
unsigned long ultimaTentativaWiFi = 0;
unsigned long ultimaTentativaMQTT = 0;
bool wifiEstavaConectado = false;

// ============ MEDIÇÃO DE TEMPOS (artigo) =========
unsigned long idMedicao = 0;
unsigned long idAguardandoEco = 0;      // id da mensagem que espera voltar do broker
unsigned long instantePublicacaoMs = 0;
unsigned long nAtuador = 0, nPublish = 0, nEco = 0;
double somaAtuadorUs = 0, somaPublishMs = 0, somaEcoMs = 0;

// ===================== ATUADORES =================
void apagarSinalizacao() {
  digitalWrite(LED_VERDE, LOW);
  digitalWrite(LED_VERMELHO, LOW);
  noTone(BUZZER);
}

// ===================== MQTT: RECEBER =============
void aoReceberMQTT(char* topico, byte* payload, unsigned int tamanho) {
  String msg;
  for (unsigned int i = 0; i < tamanho; i++) msg += (char)payload[i];

  // Comando vindo do Node-RED (central de monitoramento)
  if (strcmp(topico, TOPICO_COMANDO) == 0) {
    msg.replace("\"", "");
    msg.trim();
    msg.toUpperCase();
    if (msg == "RESET") {
      apagarSinalizacao();
      Serial.println("Comando remoto RESET recebido: sinalizacao apagada.");
    }
    return;
  }

  // Eco da própria medição: mede o tempo ESP32 -> broker -> ESP32
  if (strcmp(topico, TOPICO_DADOS) == 0 && idAguardandoEco != 0) {
    int p = msg.indexOf("\"id\":");
    if (p >= 0 && (unsigned long)msg.substring(p + 5).toInt() == idAguardandoEco) {
      unsigned long rttMs = millis() - instantePublicacaoMs;
      idAguardandoEco = 0;
      nEco++;
      somaEcoMs += rttMs;
      Serial.printf("Tempo ida e volta MQTT (ESP32 -> broker -> ESP32): %lu ms\n", rttMs);
    }
  }
}

// ===================== WI-FI =====================
void gerenciarWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiEstavaConectado) {
      wifiEstavaConectado = true;
      Serial.print("\nWi-Fi conectado! IP: ");
      Serial.println(WiFi.localIP());
    }
    return;
  }
  if (wifiEstavaConectado) {
    wifiEstavaConectado = false;
    Serial.println("\nWi-Fi desconectado.");
  }
  if (millis() - ultimaTentativaWiFi >= 10000) {   // tenta de novo a cada 10 s
    ultimaTentativaWiFi = millis();
    Serial.println("Tentando reconectar ao Wi-Fi...");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD, 6);
  }
}

// ===================== MQTT: CONEXÃO =============
void gerenciarMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqttClient.connected()) {
    mqttClient.loop();
    return;
  }
  if (millis() - ultimaTentativaMQTT < 5000) return;  // tenta a cada 5 s
  ultimaTentativaMQTT = millis();

  Serial.print("Conectando ao broker MQTT...");
  String clientId = "MACK10433738-ESP32-";
  clientId += String((uint32_t)ESP.getEfuseMac(), HEX);
  if (mqttClient.connect(clientId.c_str())) {
    Serial.println(" conectado!");
    mqttClient.subscribe(TOPICO_COMANDO);
    mqttClient.subscribe(TOPICO_DADOS);   // para medir o eco
  } else {
    Serial.print(" falhou. Codigo: ");
    Serial.println(mqttClient.state());
  }
}

// ===================== MQTT: PUBLICAR ============
void publicarMQTT(float velocidade, float excesso, const char* status, unsigned long tAtuadorUs) {
  if (!mqttClient.connected()) {
    Serial.println("MQTT indisponivel. Medicao registrada apenas localmente.");
    return;
  }
  idMedicao++;
  char mensagem[220];
  snprintf(mensagem, sizeof(mensagem),
           "{\"id\":%lu,\"velocidade\":%.2f,\"limite\":%.2f,\"percentual_excesso\":%.2f,"
           "\"status\":\"%s\",\"tempo_atuador_us\":%lu}",
           idMedicao, velocidade, LIMITE_VELOCIDADE, excesso, status, tAtuadorUs);

  instantePublicacaoMs = millis();
  idAguardandoEco = idMedicao;
  unsigned long t0 = micros();
  mqttClient.publish(TOPICO_DADOS, mensagem);
  float tPublishMs = (micros() - t0) / 1000.0;
  nPublish++;
  somaPublishMs += tPublishMs;

  Serial.println("Mensagem MQTT publicada:");
  Serial.println(mensagem);
  Serial.printf("Tempo de envio (publish): %.2f ms\n", tPublishMs);

  if (strcmp(status, "CRITICO") == 0) {
    mqttClient.publish(TOPICO_ALERTA, mensagem);
    Serial.println("ALERTA MQTT enviado!");
  }
}

// ===================== CLASSIFICAÇÃO =============
void classificarVelocidade(float velocidade) {
  float excesso = 0.0;
  if (velocidade > LIMITE_VELOCIDADE) {
    excesso = (velocidade - LIMITE_VELOCIDADE) / LIMITE_VELOCIDADE * 100.0;
  }

  const char* status;
  if (velocidade <= LIMITE_VELOCIDADE) {          // NORMAL: LED verde
    status = "NORMAL";
    digitalWrite(LED_VERDE, HIGH);
    digitalWrite(LED_VERMELHO, LOW);
    noTone(BUZZER);
  } else if (velocidade <= LIMITE_CRITICO) {      // EXCESSO: LED vermelho
    status = "EXCESSO";
    digitalWrite(LED_VERDE, LOW);
    digitalWrite(LED_VERMELHO, HIGH);
    noTone(BUZZER);
  } else {                                        // CRÍTICO: LED vermelho + buzzer
    status = "CRITICO";
    digitalWrite(LED_VERDE, LOW);
    digitalWrite(LED_VERMELHO, HIGH);
    tone(BUZZER, 1200, 1000);                     // 1 s de bipe
  }

  // tempo entre o fim da detecção e o acionamento dos atuadores
  unsigned long tAtuadorUs = micros() - instanteDeteccaoUs;
  nAtuador++;
  somaAtuadorUs += tAtuadorUs;

  Serial.printf("STATUS: %s (excesso de %.1f%%)\n", status, excesso);
  Serial.printf("Tempo deteccao -> atuador: %lu us\n", tAtuadorUs);
  publicarMQTT(velocidade, excesso, status, tAtuadorUs);
}

// ===================== CÁLCULO ===================
void calcularVelocidade() {
  float intervaloS = (tempoSensor2 - tempoSensor1) / 1000000.0;
  if (intervaloS <= 0) return;

  float velocidadeKmh = (DISTANCIA_SENSORES / intervaloS) * 3.6;  // v = d / dt, m/s -> km/h

  Serial.println("\n============================");
  Serial.printf("Tempo entre sensores: %.4f s\n", intervaloS);
  Serial.printf("Velocidade calculada: %.2f km/h\n", velocidadeKmh);
  Serial.printf("Limite: %.2f km/h | Critico: %.2f km/h\n", LIMITE_VELOCIDADE, LIMITE_CRITICO);
  classificarVelocidade(velocidadeKmh);
  Serial.println("============================");
}

// ============ MODO DE TESTE (SERIAL) =============
void simularVelocidade(float velocidadeAlvo) {
  aguardandoSensor2 = false;
  unsigned long intervaloUs = (DISTANCIA_SENSORES / (velocidadeAlvo / 3.6)) * 1000000.0;
  tempoSensor1 = micros();
  tempoSensor2 = tempoSensor1 + intervaloUs;
  instanteDeteccaoUs = micros();
  Serial.printf("\n***** TESTE: %.0f km/h *****", velocidadeAlvo);
  calcularVelocidade();
}

void mostrarTemposMedios() {
  Serial.println("\n======= TEMPOS MEDIOS =======");
  if (nAtuador) Serial.printf("Deteccao -> atuador:  %.1f us (%lu medicoes)\n", somaAtuadorUs / nAtuador, nAtuador);
  if (nPublish) Serial.printf("Envio MQTT (publish): %.2f ms (%lu medicoes)\n", somaPublishMs / nPublish, nPublish);
  if (nEco)     Serial.printf("Ida e volta MQTT:     %.1f ms (%lu medicoes)\n", somaEcoMs / nEco, nEco);
  if (!nAtuador) Serial.println("Nenhuma medicao ainda.");
  Serial.println("=============================");
}

void mostrarMenu() {
  Serial.println("\n==================================");
  Serial.println("1 = Simular 40 km/h (NORMAL)");
  Serial.println("2 = Simular 65 km/h (EXCESSO)");
  Serial.println("3 = Simular 90 km/h (CRITICO)");
  Serial.println("4 = Mostrar tempos medios");
  Serial.println("==================================\n");
}

// ===================== SETUP =====================
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(SENSOR_1, INPUT_PULLUP);
  pinMode(SENSOR_2, INPUT_PULLUP);
  pinMode(LED_VERDE, OUTPUT);
  pinMode(LED_VERMELHO, OUTPUT);
  pinMode(BUZZER, OUTPUT);
  apagarSinalizacao();

  estadoAnteriorSensor1 = digitalRead(SENSOR_1);
  estadoAnteriorSensor2 = digitalRead(SENSOR_2);

  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(aoReceberMQTT);

  Serial.println("\nIniciando conexao Wi-Fi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, 6);

  Serial.println("Sistema de deteccao de velocidade iniciado.");
  Serial.println("Aguardando passagem pelo Sensor 1...");
  mostrarMenu();
}

// ===================== LOOP ======================
void loop() {
  gerenciarWiFi();
  gerenciarMQTT();

  if (Serial.available()) {
    char comando = Serial.read();
    if (comando == '1') simularVelocidade(40.0);
    else if (comando == '2') simularVelocidade(65.0);
    else if (comando == '3') simularVelocidade(90.0);
    else if (comando == '4') mostrarTemposMedios();
  }

  int estadoSensor1 = digitalRead(SENSOR_1);
  int estadoSensor2 = digitalRead(SENSOR_2);

  // Sensor 1: feixe interrompido (HIGH -> LOW) inicia a medição
  if (estadoAnteriorSensor1 == HIGH && estadoSensor1 == LOW && !aguardandoSensor2) {
    tempoSensor1 = micros();
    aguardandoSensor2 = true;
    apagarSinalizacao();
    Serial.println("\nSensor 1 acionado. Cronometro iniciado. Aguardando Sensor 2...");
  }

  // Sensor 2: encerra a medição e calcula a velocidade
  if (estadoAnteriorSensor2 == HIGH && estadoSensor2 == LOW && aguardandoSensor2) {
    tempoSensor2 = micros();
    instanteDeteccaoUs = tempoSensor2;
    aguardandoSensor2 = false;
    Serial.println("Sensor 2 acionado.");
    calcularVelocidade();
    Serial.println("Sistema pronto para nova medicao. Aguardando Sensor 1...");
  }

  // Timeout: sensor 2 não foi acionado
  if (aguardandoSensor2 && (micros() - tempoSensor1 > TIMEOUT_MEDICAO_US)) {
    aguardandoSensor2 = false;
    Serial.println("\nMedicao cancelada: Sensor 2 nao acionado. Aguardando Sensor 1...");
  }

  estadoAnteriorSensor1 = estadoSensor1;
  estadoAnteriorSensor2 = estadoSensor2;
  delay(10);
}
