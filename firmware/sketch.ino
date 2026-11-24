// Sistema de Detecção de Excesso Grave de Velocidade - ODS 3 (meta 3.6)
// Objetos Inteligentes Conectados - Mackenzie
// No Wokwi os dois sensores infravermelhos são simulados por dois botões.

#include <WiFi.h>
#include <PubSubClient.h>

// Rede e broker
const char* ssid = "Wokwi-GUEST";
const char* senha = "";
const char* broker = "test.mosquitto.org";
const int porta = 1883;

// Tópicos MQTT
const char* topicoDados   = "MACK10433738/DADOS";
const char* topicoAlerta  = "MACK10433738/ALERTA";
const char* topicoComando = "MACK10433738/COMANDO";

// Pinos
const int SENSOR1 = 32;
const int SENSOR2 = 33;
const int LED_VERDE = 19;
const int LED_VERMELHO = 18;
const int BUZZER = 22;

// Regras do projeto
const float DISTANCIA = 0.50;      // metros entre os sensores
const float LIMITE = 60.0;         // km/h
const float CRITICO = LIMITE * 1.3; // 30% acima do limite = 78 km/h

WiFiClient espClient;
PubSubClient client(espClient);

unsigned long t1 = 0;              // instante do sensor 1 (microssegundos)
bool esperandoSensor2 = false;
unsigned long ultimaTentativa = 0;
unsigned long envioMQTT = 0;       // para medir o tempo de ida e volta no broker

// Recebe mensagens do broker
void callback(char* topico, byte* payload, unsigned int tamanho) {
  String msg = "";
  for (int i = 0; i < tamanho; i++) msg += (char)payload[i];

  // Comando enviado pelo Node-RED
  if (String(topico) == topicoComando && msg == "RESET") {
    digitalWrite(LED_VERDE, LOW);
    digitalWrite(LED_VERMELHO, LOW);
    noTone(BUZZER);
    Serial.println("RESET recebido do Node-RED");
  }

  // A propria medicao voltando do broker: mede o tempo de ida e volta
  if (String(topico) == topicoDados && envioMQTT > 0) {
    Serial.print("Tempo ida e volta no broker: ");
    Serial.print(millis() - envioMQTT);
    Serial.println(" ms");
    envioMQTT = 0;
  }
}

// Conecta no broker sem travar o programa (tenta a cada 5 s)
void conectarMQTT() {
  if (WiFi.status() != WL_CONNECTED || millis() - ultimaTentativa < 5000) return;
  ultimaTentativa = millis();
  Serial.print("Conectando ao broker... ");
  if (client.connect("MACK10433738-esp32")) {
    Serial.println("conectado!");
    client.subscribe(topicoComando);
    client.subscribe(topicoDados);
  } else {
    Serial.println("falhou, tentando de novo");
  }
}

// Calcula a velocidade, aciona os atuadores e publica no broker
void medirVelocidade(unsigned long intervaloUs) {
  unsigned long inicio = micros();
  float velocidade = DISTANCIA / (intervaloUs / 1000000.0) * 3.6;   // km/h
  float excesso = 0;
  if (velocidade > LIMITE) excesso = (velocidade - LIMITE) / LIMITE * 100;

  String status;
  if (velocidade <= LIMITE) {
    status = "NORMAL";
    digitalWrite(LED_VERDE, HIGH);
    digitalWrite(LED_VERMELHO, LOW);
  } else if (velocidade <= CRITICO) {
    status = "EXCESSO";
    digitalWrite(LED_VERDE, LOW);
    digitalWrite(LED_VERMELHO, HIGH);
  } else {
    status = "CRITICO";
    digitalWrite(LED_VERDE, LOW);
    digitalWrite(LED_VERMELHO, HIGH);
    tone(BUZZER, 1200, 1000);
  }
  unsigned long tempoAtuador = micros() - inicio;

  String json = "{\"velocidade\":" + String(velocidade, 2) +
                ",\"limite\":" + String(LIMITE, 2) +
                ",\"percentual_excesso\":" + String(excesso, 2) +
                ",\"status\":\"" + status + "\"}";

  Serial.println("Velocidade: " + String(velocidade, 2) + " km/h -> " + status);
  Serial.println("Tempo deteccao -> atuador: " + String(tempoAtuador) + " us");

  if (client.connected()) {
    envioMQTT = millis();
    client.publish(topicoDados, json.c_str());
    if (status == "CRITICO") client.publish(topicoAlerta, json.c_str());
    Serial.println("Publicado: " + json);
  } else {
    Serial.println("Sem conexao com o broker, medicao so local");
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(SENSOR1, INPUT_PULLUP);
  pinMode(SENSOR2, INPUT_PULLUP);
  pinMode(LED_VERDE, OUTPUT);
  pinMode(LED_VERMELHO, OUTPUT);
  pinMode(BUZZER, OUTPUT);

  WiFi.begin(ssid, senha, 6);
  client.setServer(broker, porta);
  client.setCallback(callback);

  Serial.println("Sistema iniciado. Teste pelo Serial: 1 = 40 km/h, 2 = 65 km/h, 3 = 90 km/h");
}

void loop() {
  if (!client.connected()) conectarMQTT();
  client.loop();

  // Sensor 1: o veiculo interrompe o primeiro feixe (botao 1)
  if (digitalRead(SENSOR1) == LOW && !esperandoSensor2) {
    t1 = micros();
    esperandoSensor2 = true;
    Serial.println("Sensor 1 acionado, aguardando sensor 2...");
  }

  // Sensor 2: o veiculo interrompe o segundo feixe (botao 2)
  if (digitalRead(SENSOR2) == LOW && esperandoSensor2) {
    esperandoSensor2 = false;
    medirVelocidade(micros() - t1);
  }

  // Se o sensor 2 nao for acionado em 30 s, cancela a medicao
  if (esperandoSensor2 && micros() - t1 > 30000000) {
    esperandoSensor2 = false;
    Serial.println("Medicao cancelada");
  }

  // Modo de teste: com os botoes nao da para chegar a 60 km/h (0,03 s entre os sensores)
  if (Serial.available()) {
    char c = Serial.read();
    if (c == '1') medirVelocidade(45000);   // 0,045 s -> 40 km/h
    if (c == '2') medirVelocidade(27692);   // 0,0277 s -> 65 km/h
    if (c == '3') medirVelocidade(20000);   // 0,020 s -> 90 km/h
  }

  delay(10);
}
