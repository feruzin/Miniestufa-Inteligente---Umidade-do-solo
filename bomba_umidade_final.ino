#include <WiFi.h>
#include <PubSubClient.h>
#include "time.h"   // Para pegar hora via NTP

// Config Wi-Fi
const char* ssid = "Ruzin_2.4g";       
const char* password = "eu29192313";   

// Config MQTT
const char* mqtt_server = "192.168.18.21"; // IP do notebook rodando Mosquitto
WiFiClient espClient;
PubSubClient client(espClient);

// Pino do sensor de umidade
const int sensorUmidade = 34;  

// Pinos TB6612FNG (AIN1 e AIN2)
const int motorAIN1 = 25;  
const int motorAIN2 = 27;  

// Faixa de calibração do sensor
const int seco = 1600;   // valor típico em terra seca
const int molhado = 910; // valor típico em terra bem molhada

// Variáveis de controle da bomba
bool bombaLigada = false;
unsigned long tempoInicioBomba = 0;
const unsigned long duracaoBomba = 5000; // 5 segundos

// Intervalo de envio MQTT (10 minutos)
const unsigned long intervaloEnvio = 600000; 

// Configuração NTP
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -3 * 3600;   // GMT-3 (Brasil)
const int daylightOffset_sec = 0;

// Função para reconectar ao broker
void reconnect() {
  while (!client.connected()) {
    Serial.print("Conectando ao MQTT...");
    if (client.connect("ESP32Client")) {
      Serial.println("conectado!");
    } else {
      Serial.print("falha, rc=");
      Serial.print(client.state());
      Serial.println(" tentando novamente em 5s");
      delay(5000);
    }
  }
}

// Função para pegar hora formatada
String getHoraAtual() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "00:00:00"; // fallback
  }
  char buffer[20];
  strftime(buffer, sizeof(buffer), "%d/%m/%Y %H:%M:%S", &timeinfo);
  return String(buffer);
}

// Função para publicar logs no MQTT
void publicarLog(String mensagem) {
  client.publish("miniEstufa/logs", mensagem.c_str());
  Serial.println("Mensagem enviada -> " + mensagem);
}

void setup() {
  Serial.begin(115200);
  WiFi.begin(ssid, password);

  Serial.print("Conectando ao Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print(".");
  }
  Serial.println("\nWi-Fi conectado!");

  client.setServer(mqtt_server, 1883);

  pinMode(sensorUmidade, INPUT);
  pinMode(motorAIN1, OUTPUT);
  pinMode(motorAIN2, OUTPUT);

  // Garante que a bomba inicia desligada
  digitalWrite(motorAIN1, LOW);
  digitalWrite(motorAIN2, LOW);

  // Inicializa NTP
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
}

void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  static unsigned long lastMsg = 0;
  unsigned long now = millis();

  // ---- Leituras periódicas (10 minutos) ----
  if (now - lastMsg > intervaloEnvio) {
    lastMsg = now;

    int valorUmidade = analogRead(sensorUmidade);

    // Converte para porcentagem
    int porcentagem = map(valorUmidade, seco, molhado, 0, 100);
    porcentagem = constrain(porcentagem, 0, 100);

    // Monta mensagem JSON completa
    String hora = getHoraAtual();
    String mensagem = "{Umidade do Solo";
    mensagem += "\"hora\":\"" + hora + "\",";
    mensagem += "\"tipo\":\"leituras\",";
    mensagem += "\"dados_brutos\":" + String(valorUmidade) + ",";
    mensagem += "\"percentual\":" + String(porcentagem) + "%}";

    publicarLog(mensagem);

    // Lógica de controle da bomba
    if (!bombaLigada && porcentagem <= 30) {
      digitalWrite(motorAIN1, HIGH);
      digitalWrite(motorAIN2, LOW);
      bombaLigada = true;
      tempoInicioBomba = millis();
      Serial.println("Bomba ligada por 5 segundos!");

      // Publica evento imediato
      String msgEvento = "{";
      msgEvento += "\"hora\":\"" + hora + "\",";
      msgEvento += "\"tipo\":\"evento\",";
      msgEvento += "\"evento\":\"bomba_ligada\"";
      msgEvento += "}";
      publicarLog(msgEvento);
    }
  }

  // ---- Controle de tempo da bomba ----
  if (bombaLigada && (millis() - tempoInicioBomba >= duracaoBomba)) {
    digitalWrite(motorAIN1, LOW);
    digitalWrite(motorAIN2, LOW);
    bombaLigada = false;
    Serial.println("Bomba desligada!");

    String hora = getHoraAtual();
    String msgEvento = "{";
    msgEvento += "\"hora\":\"" + hora + "\",";
    msgEvento += "\"tipo\":\"evento\",";
    msgEvento += "\"evento\":\"bomba_desligada\"";
    msgEvento += "}";
    publicarLog(msgEvento);
  }
}
