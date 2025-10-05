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
const int seco = 2200;   // valor típico em terra seca
const int molhado = 910; // valor típico em terra bem molhada

// Variáveis de controle da bomba
bool bombaLigada = false;
unsigned long tempoInicioBomba = 0;
const unsigned long duracaoBomba = 5000; // 5 segundos

// Intervalo de envio MQTT (1 minuto = 60000 ms)
const unsigned long intervaloEnvio = 1200000; 

// Configuração NTP
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -3 * 3600;   // GMT-3 (Brasil)
const int daylightOffset_sec = 0;

// ---- Filtro de Média Móvel ----
const int NUM_AMOSTRAS = 10;  
int leituras[NUM_AMOSTRAS];  
int indice = 0;
long soma = 0;

// ---- Histerese ----
bool bloqueioHisterese = false;
const int LIMITE_LIGAR = 30;    // Liga bomba se <= 30%
const int LIMITE_RESET = 40;    // Só libera novo ciclo quando passar de 40%

// ---- Funções MQTT ----
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
    return "00/00/0000 00:00:00"; // fallback
  }
  char buffer[20];
  strftime(buffer, sizeof(buffer), "%d/%m/%Y %H:%M:%S", &timeinfo);
  return String(buffer);
}

// Publica leituras
void publicarLeitura(String mensagem) {
  client.publish("miniEstufa/leituras", mensagem.c_str());
  Serial.println("Leitura enviada -> " + mensagem);
}

// Publica eventos
void publicarEvento(String mensagem) {
  client.publish("miniEstufa/eventos", mensagem.c_str());
  Serial.println("Evento enviado -> " + mensagem);
}

// Função para obter leitura filtrada (média móvel)
int leituraFiltrada() {
  soma -= leituras[indice];                     // remove valor antigo
  leituras[indice] = analogRead(sensorUmidade); // nova leitura
  soma += leituras[indice];                     // adiciona valor novo
  indice = (indice + 1) % NUM_AMOSTRAS;         // índice circular

  return soma / NUM_AMOSTRAS;
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

  // Inicializa buffer do filtro
  for (int i = 0; i < NUM_AMOSTRAS; i++) {
    leituras[i] = analogRead(sensorUmidade);
    soma += leituras[i];
  }
}

void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  static unsigned long lastMsg = 0;
  unsigned long now = millis();

  // ---- Leitura filtrada em tempo real ----
  int valorUmidade = leituraFiltrada();  
  int porcentagem = map(valorUmidade, seco, molhado, 0, 100);
  porcentagem = constrain(porcentagem, 0, 100);

  // ---- Controle da bomba em tempo real com histerese ----
  if (!bombaLigada && !bloqueioHisterese && porcentagem <= LIMITE_LIGAR) {
    // Liga a bomba
    digitalWrite(motorAIN1, HIGH);
    digitalWrite(motorAIN2, LOW);
    bombaLigada = true;
    tempoInicioBomba = millis();
    bloqueioHisterese = true;  // trava até subir acima de 40%

    String hora = getHoraAtual();
    String msgEvento = "{";
    msgEvento += "\"hora\":\"" + hora + "\",";
    msgEvento += "\"tipo\":\"evento\",";
    msgEvento += "\"evento\":\"bomba_ligada\"";
    msgEvento += "}";
    publicarEvento(msgEvento);

    Serial.println(">>> Bomba ligada!");
  }

  // Mantém a bomba ligada até completar os 5s
  if (bombaLigada) {
    if (millis() - tempoInicioBomba >= duracaoBomba) {
      // Desliga a bomba
      digitalWrite(motorAIN1, LOW);
      digitalWrite(motorAIN2, LOW);
      bombaLigada = false;

      String hora = getHoraAtual();
      String msgEvento = "{";
      msgEvento += "\"hora\":\"" + hora + "\",";
      msgEvento += "\"tipo\":\"evento\",";
      msgEvento += "\"evento\":\"bomba_desligada\"";
      msgEvento += "}";
      publicarEvento(msgEvento);

      Serial.println(">>> Bomba desligada!");
    }
  }

  // Libera novo ciclo só quando a umidade subir acima de 40%
  if (!bombaLigada && bloqueioHisterese && porcentagem > LIMITE_RESET) {
    bloqueioHisterese = false;
    Serial.println(">>> Histerese liberada, pronto para novo ciclo.");
  }

  // ---- Publica leitura a cada 1 minuto ----
  if (now - lastMsg > intervaloEnvio) {
    lastMsg = now;

    String hora = getHoraAtual();
    String mensagem = "{";
    mensagem += "\"hora\":\"" + hora + "\",";
    mensagem += "\"tipo\":\"leituras\",";
    mensagem += "\"dados_brutos\":" + String(valorUmidade) + ",";
    mensagem += "\"percentual\":" + String(porcentagem);
    mensagem += "}";
    publicarLeitura(mensagem);
  }

  delay(1000); // nova amostra a cada 1s
}
