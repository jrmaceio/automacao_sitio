#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <vector>

// NOVO: Inclusão da biblioteca para o sensor DHT
#include "DHT.h"

// --- CONFIGURAÇÕES ---
const char* ssid = "CLARO_2GC3B8C0";
const char* password = "12C3B8C0";
const char* googleScriptURL = "https://script.google.com/macros/s/AKfycby6ED8BtXkyP03zw0nKYjEmIMdUQJquTJCv1DbZHg5gVLTlxXgewGTc7LhnXjQle4ikVg/exec";

// --- PINOS ---
const int RELE_PIN = 25;
const int LED_PIN = 26;
const int BOTAO_PIN = 27;

// NOVO: Definições para o sensor DHT11
const int DHT_PIN = 4;      // Pino onde o sensor está conectado (D4)
#define DHT_TYPE DHT11      // Define o tipo do sensor como DHT11

const unsigned long INTERVALO_LOOP = 1800000; // Intervalo entre as leituras (30 minutos)

// NOVO: Cria o objeto 'dht' para nos comunicarmos com o sensor
DHT dht(DHT_PIN, DHT_TYPE);


// --- FUNÇÕES AUXILIARES E DE INTERAÇÃO COM A PLANILHA (sem alterações) ---
String fazerRequisicao(const String& jsonPayload) {
  if (WiFi.status() != WL_CONNECTED) {
    return "ERRO: Sem conexão WiFi";
  }
  HTTPClient http;
  http.begin(googleScriptURL);
  http.addHeader("Content-Type", "application/json");
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  int httpResponseCode = http.POST(jsonPayload);
  String response = "";
  if (httpResponseCode > 0) {
    response = http.getString();
  } else {
    response = "ERRO: Falha na requisição HTTP. Código: " + String(http.errorToString(httpResponseCode).c_str());
  }
  http.end();
  return response;
}
bool escreverEmLista(const String& identificacao, int numDados, float dados[]) {
  StaticJsonDocument<256> jsonDoc;
  jsonDoc["action"] = "escreverEmLista";
  jsonDoc["identificacao"] = identificacao;
  JsonArray jsonDados = jsonDoc.createNestedArray("dados");
  for (int i = 0; i < numDados; i++) {
    jsonDados.add(dados[i]);
  }
  String jsonString;
  serializeJson(jsonDoc, jsonString);
  String response = fazerRequisicao(jsonString);
  Serial.println("Resposta do 'escreverEmLista': " + response);
  return !response.startsWith("ERRO");
}
String lerCelula(const String& identificacao, const String& celula) {
  StaticJsonDocument<200> jsonDoc;
  jsonDoc["action"] = "lerCelula";
  jsonDoc["identificacao"] = identificacao;
  jsonDoc["celula"] = celula;
  String jsonString;
  serializeJson(jsonDoc, jsonString);
  return fazerRequisicao(jsonString);
}
bool escreverEmCelula(const String& identificacao, const String& celula, const String& dado) {
    StaticJsonDocument<200> jsonDoc;
    jsonDoc["action"] = "escreverEmCelula";
    jsonDoc["identificacao"] = identificacao;
    jsonDoc["celula"] = celula;
    jsonDoc["dado"] = dado;
    String jsonString;
    serializeJson(jsonDoc, jsonString);
    String response = fazerRequisicao(jsonString);
    Serial.println("Resposta do 'escreverEmCelula': " + response);
    return !response.startsWith("ERRO");
}
void montarCabecalho(const String& boardID, const String& colunaInicial, const std::vector<String>& cabecalhos) {
    String celulaVerificacao = lerCelula(boardID, colunaInicial + "1");
    if (celulaVerificacao != cabecalhos[0]) {
        Serial.println("Cabeçalho não encontrado ou diferente. Configurando a planilha...");
        char coluna = colunaInicial[0];
        for (size_t i = 0; i < cabecalhos.size(); i++) {
            String celulaAlvo = String(coluna) + "1";
            int tentativas = 0;
            while (!escreverEmCelula(boardID, celulaAlvo, cabecalhos[i]) && tentativas < 3) {
                Serial.println("Falha ao escrever cabeçalho. Tentando novamente...");
                delay(1000);
                tentativas++;
            }
            coluna++;
        }
        Serial.println("Configuração do cabeçalho finalizada.");
    } else {
        Serial.println("Cabeçalho da planilha já está correto.");
    }
}


// --- SETUP ---
void setup() {
  Serial.begin(115200);
  delay(1000); 
  Serial.println("\n\n--- INICIANDO SETUP ---");
  
  pinMode(RELE_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BOTAO_PIN, INPUT_PULLUP); 

  digitalWrite(RELE_PIN, LOW); 
  digitalWrite(LED_PIN, LOW);  

  // NOVO: Inicia o sensor DHT
  dht.begin();
  Serial.println("SETUP: Sensor DHT11 iniciado.");

  Serial.println("SETUP: Conectando ao WiFi...");
  WiFi.begin(ssid, password);

  int tentativasWifi = 0;
  while (WiFi.status() != WL_CONNECTED && tentativasWifi < 20) {
    delay(500);
    Serial.print(".");
    tentativasWifi++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nSETUP: WiFi Conectado!");
    Serial.println("SETUP: Antes de montarCabecalho...");
    
    montarCabecalho("Arapiraca", "A", {"Data completa", "Data", "Hora", "Umidade", "Temperatura", "Botao", "Rele_Planilha", "Led_Planilha"});
    
    Serial.println("SETUP: Depois de montarCabecalho.");
    Serial.println("--- SETUP CONCLUído, ENTRANDO NO LOOP ---");
  } else {
    Serial.println("\nFalha ao conectar no WiFi. Reiniciando...");
    delay(10000);
    ESP.restart();
  }
}

// --- LOOP ---
void loop() {
  Serial.println("\nLOOP: Início da execução.");
  bool statusBotao = (digitalRead(BOTAO_PIN) == LOW); 
  
  // NOVO: Leitura da umidade e temperatura do sensor DHT11
  float umidade = dht.readHumidity();
  float temperatura = dht.readTemperature(); // Lê em Celsius

  // NOVO: Verificação de erro na leitura do sensor (muito importante!)
  // Se a leitura falhar, 'isnan' retornará true.
  if (isnan(umidade) || isnan(temperatura)) {
    Serial.println("LOOP: Falha ao ler dados do sensor DHT11!");
    // Pula o envio de dados para a planilha se a leitura falhou
  } else {
    Serial.print("LOOP: Leitura do Sensor -> ");
    Serial.print(umidade);
    Serial.print("% Umidade, ");
    Serial.print(temperatura);
    Serial.println("°C");
    
    // Envia os dados REAIS para a planilha somente se a leitura for bem-sucedida
    float dadosParaEnviar[] = {umidade, temperatura, (float)statusBotao};
    Serial.println("LOOP: Antes de escreverEmLista...");
    escreverEmLista("Arapiraca", 3, dadosParaEnviar);
    Serial.println("LOOP: Depois de escreverEmLista.");
  }
  
  // O sensor DHT11 precisa de um intervalo de ~2s entre leituras para estabilizar.
  // Como o resto do código já tem delays e pausas, pode não ser estritamente
  // necessário, mas é uma boa prática para garantir leituras estáveis.
  delay(2000); 

  Serial.println("LOOP: Antes de ler a celula do Rele (G2)...");
  String valorRelePlanilha = lerCelula("Arapiraca", "G2");
  Serial.println("LOOP: Depois de ler a celula do Rele. Valor: " + valorRelePlanilha);

  if (valorRelePlanilha == "1") {
    digitalWrite(RELE_PIN, HIGH);
  } else if (valorRelePlanilha == "0") {
    digitalWrite(RELE_PIN, LOW);
  }
  
  delay(10);

  Serial.println("LOOP: Antes de ler a celula do LED (H2)...");
  String valorLedPlanilha = lerCelula("Arapiraca", "H2");
  Serial.println("LOOP: Depois de ler a celula do LED. Valor: " + valorLedPlanilha);
  
  if (valorLedPlanilha == "1") {
    digitalWrite(LED_PIN, HIGH); 
  } else if (valorLedPlanilha == "0") {
    digitalWrite(LED_PIN, LOW);
  }

  Serial.println("LOOP: Fim da execução, aguardando proximo ciclo.");
  Serial.println("--------------------------------");
  delay(INTERVALO_LOOP);
}
