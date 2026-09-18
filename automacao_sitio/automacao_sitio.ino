#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <time.h>
#include <vector>
#include <FS.h>
#include <LittleFS.h>

// Inclusão da biblioteca para o sensor DHT
#include "DHT.h"

// ====================================================================
// REDES WIFI CONHECIDAS
// Este firmware é único para todas as estações: no boot, tenta cada
// rede da lista até uma conectar. A rede que conectar define a
// identidade da estação (aba usada na planilha).
// ====================================================================
struct RedeConhecida {
  const char* ssid;
  const char* senha;
  const char* identificacao;
};

RedeConhecida redesConhecidas[] = {
  {"CLARO_2GC3B8C0", "12C3B8C0",     "Arapiraca"},
  {"Sitio",          "Andrade2020",  "BeloMonte"},
};
const int NUM_REDES_CONHECIDAS = sizeof(redesConhecidas) / sizeof(redesConhecidas[0]);
const unsigned long TIMEOUT_CONEXAO_WIFI_MS = 10000;

String identificacao = ""; // definida em conectarWiFi()

const char* googleScriptURL = "https://script.google.com/macros/s/AKfycbyh71u__MFmmi2uaEhTl_fhvzuthZxjD90fjT4gchU2YOwsSfmt-akRIiKi3WakQelH/exec";

// --- PINOS ---
const int RELE_SETOR1_PIN = 25; // Irrigação Setor 1 (dentro do sítio)
const int RELE_SETOR2_PIN = 33; // Irrigação Setor 2 (atrás do salão)
const int LED_PIN = 26;
const int BOTAO_PIN = 27;
const int DHT_PIN = 4;
#define DHT_TYPE DHT22

// --- LÓGICA DO MÓDULO DE RELÉ ---
// Módulos de 2 relés com foto-acoplador costumam ser ATIVOS EM LOW (IN em LOW liga o
// relé, HIGH desliga) — o oposto do que este firmware assume por padrão (false = ativo em HIGH).
// Teste antes de ligar a carga real (bomba/válvula): ao gravar com "false", o relé deve
// ficar DESLIGADO logo no boot. Se ele ligar sozinho no boot, troque para "true" abaixo.
const bool RELE_ATIVO_EM_LOW = true;
const int RELE_LIGADO    = RELE_ATIVO_EM_LOW ? LOW  : HIGH;
const int RELE_DESLIGADO = RELE_ATIVO_EM_LOW ? HIGH : LOW;

DHT dht(DHT_PIN, DHT_TYPE);
WebServer server(80);

// --- NTP / HORÁRIO (Brasília, sem horário de verão) ---
const char* NTP_SERVER_1 = "a.st1.ntp.br";
const char* NTP_SERVER_2 = "pool.ntp.org";
const long GMT_OFFSET_SEC = -3 * 3600;
const int DAYLIGHT_OFFSET_SEC = 0;
bool horaSincronizada = false;

// ====================================================================
// TABELA DE HORÁRIOS DE IRRIGAÇÃO
// ====================================================================
struct FaixaHorario {
  int horaLiga;
  int minLiga;
  int horaDesliga;
  int minDesliga;
};

FaixaHorario horariosSetor1[] = {
  {19, 0, 19, 30},
  {20, 0, 20, 30},
  {21, 0, 21, 30},
  {22, 0, 22, 30},
  {23, 0, 23, 30},
};
const int NUM_HORARIOS_SETOR1 = sizeof(horariosSetor1) / sizeof(horariosSetor1[0]);

FaixaHorario horariosSetor2[] = {
  {0, 10, 0, 30},
  {1, 0, 1, 30},
  {2, 0, 2, 30},
  {3, 0, 3, 30},
  {4, 0, 4, 30},
};
const int NUM_HORARIOS_SETOR2 = sizeof(horariosSetor2) / sizeof(horariosSetor2[0]);

// ====================================================================
// ESTADO EM MEMÓRIA (usado pelo dashboard e pelo controle dos relés)
// ====================================================================
float ultimaUmidade = NAN;
float ultimaTemperatura = NAN;
bool estadoSetor1 = false;
bool estadoSetor2 = false;
bool estadoLed = false;

// Últimos valores lidos da planilha para override manual ("1"/"0"/vazio=AUTO)
String overrideSetor1 = "";
String overrideSetor2 = "";
String overrideLed = "";

// ====================================================================
// FILA LOCAL (LittleFS) — registros que falharam ao enviar para a planilha
// ficam guardados na flash do ESP32 e são reenviados assim que possível,
// sobrevivendo inclusive a reinicializações.
// ====================================================================
const char* ARQUIVO_FILA = "/fila.jsonl";
const int MAX_REGISTROS_FILA = 150; // limite para não estourar a flash em quedas longas de rede
bool littleFsDisponivel = false;
int filaTamanho = 0;

// Últimos eventos (log) mantidos em memória para exibição imediata no dashboard
const int MAX_LOG_RAM = 8;
String logRecentes[MAX_LOG_RAM];
int logRecentesIndice = 0;
int logRecentesTotal = 0;

// --- INTERVALOS (loop não bloqueante baseado em millis()) ---
// Leitura do DHT fica frequente (barata, sem rede) para alimentar o dashboard em tempo real.
const unsigned long INTERVALO_LEITURA_DHT_MS = 5000;       // 5s
// Verificação do relé também fica frequente e independente do envio à planilha: as janelas de
// irrigação são de 30 min, então checar raramente atrasaria o liga/desliga e estouraria a duração.
const unsigned long INTERVALO_VERIFICA_RELE_MS = 1000;     // 1s
// Envio para a planilha Google a cada 40 min, como no projeto original.
const unsigned long INTERVALO_SYNC_PLANILHA_MS = 2400000;  // 40 min
// Tentativa de reenviar a fila local com mais frequência, para não esperar 40 min
// depois que a rede/planilha voltar a funcionar.
const unsigned long INTERVALO_TENTATIVA_FILA_MS = 120000;  // 2 min

unsigned long ultimaLeituraDht = 0;
unsigned long ultimaVerificacaoRele = 0;
unsigned long ultimoSyncPlanilha = 0;
unsigned long ultimaTentativaFila = 0;

// ====================================================================
// WIFI
// ====================================================================
bool conectarWiFi() {
  for (int i = 0; i < NUM_REDES_CONHECIDAS; i++) {
    Serial.printf("WIFI: Tentando conectar em '%s'...\n", redesConhecidas[i].ssid);
    WiFi.begin(redesConhecidas[i].ssid, redesConhecidas[i].senha);

    unsigned long inicio = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - inicio < TIMEOUT_CONEXAO_WIFI_MS) {
      delay(300);
      Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
      identificacao = redesConhecidas[i].identificacao;
      Serial.printf("\nWIFI: Conectado em '%s' -> estação identificada como '%s'\n",
                    redesConhecidas[i].ssid, identificacao.c_str());
      Serial.print("WIFI: IP local: ");
      Serial.println(WiFi.localIP());
      return true;
    }

    Serial.printf("\nWIFI: Falha ao conectar em '%s'.\n", redesConhecidas[i].ssid);
    WiFi.disconnect(true);
    delay(500);
  }
  return false;
}

// ====================================================================
// FILA LOCAL — usada quando a planilha não pode ser gravada agora
// ====================================================================
int contarLinhasFila() {
  if (!littleFsDisponivel || !LittleFS.exists(ARQUIVO_FILA)) return 0;
  File arquivo = LittleFS.open(ARQUIVO_FILA, FILE_READ);
  if (!arquivo) return 0;
  int total = 0;
  while (arquivo.available()) {
    String linha = arquivo.readStringUntil('\n');
    linha.trim();
    if (linha.length() > 0) total++;
  }
  arquivo.close();
  return total;
}

// Remove a linha mais antiga da fila (usado quando o limite MAX_REGISTROS_FILA é atingido)
void removerRegistroMaisAntigoDaFila() {
  if (!littleFsDisponivel) return;
  File origem = LittleFS.open(ARQUIVO_FILA, FILE_READ);
  if (!origem) return;
  String restante = "";
  bool primeira = true;
  while (origem.available()) {
    String linha = origem.readStringUntil('\n');
    if (primeira) { primeira = false; continue; }
    if (linha.length() > 0) restante += linha + "\n";
  }
  origem.close();
  File destino = LittleFS.open(ARQUIVO_FILA, FILE_WRITE);
  if (destino) {
    destino.print(restante);
    destino.close();
  }
  if (filaTamanho > 0) filaTamanho--;
}

void enfileirar(const String& jsonPayload) {
  if (!littleFsDisponivel) {
    Serial.println("FILA: LittleFS indisponível, registro perdido: " + jsonPayload);
    return;
  }
  if (filaTamanho >= MAX_REGISTROS_FILA) {
    removerRegistroMaisAntigoDaFila();
  }
  File arquivo = LittleFS.open(ARQUIVO_FILA, FILE_APPEND);
  if (arquivo) {
    arquivo.println(jsonPayload);
    arquivo.close();
    filaTamanho++;
  }
}

// Tenta reenviar todos os registros pendentes; para no primeiro que ainda falhar
// e mantém o restante na fila para a próxima tentativa.
void tentarEsvaziarFila() {
  if (!littleFsDisponivel || filaTamanho == 0) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (!LittleFS.exists(ARQUIVO_FILA)) { filaTamanho = 0; return; }

  File origem = LittleFS.open(ARQUIVO_FILA, FILE_READ);
  if (!origem) return;

  std::vector<String> linhasRestantes;
  bool falhouAlguma = false;

  while (origem.available()) {
    String linha = origem.readStringUntil('\n');
    linha.trim();
    if (linha.length() == 0) continue;

    if (!falhouAlguma) {
      String resposta = fazerRequisicao(linha);
      if (resposta.startsWith("ERRO")) {
        falhouAlguma = true;
        linhasRestantes.push_back(linha);
      }
    } else {
      linhasRestantes.push_back(linha);
    }
  }
  origem.close();

  File destino = LittleFS.open(ARQUIVO_FILA, FILE_WRITE);
  if (destino) {
    for (size_t i = 0; i < linhasRestantes.size(); i++) {
      destino.println(linhasRestantes[i]);
    }
    destino.close();
  }
  filaTamanho = linhasRestantes.size();

  if (filaTamanho == 0) {
    Serial.println("FILA: Todos os registros pendentes foram sincronizados com a planilha.");
  }
}

// ====================================================================
// COMUNICAÇÃO COM A PLANILHA GOOGLE
// ====================================================================
String fazerRequisicao(const String& jsonPayload) {
  if (WiFi.status() != WL_CONNECTED) {
    return "ERRO: Sem conexão WiFi";
  }
  HTTPClient http;
  String response = "";
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.begin(googleScriptURL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000);
  int httpResponseCode = http.POST(jsonPayload.c_str());
  if (httpResponseCode > 0) {
    response = http.getString();
    Serial.printf("[HTTP] Código: %d\n", httpResponseCode);
  } else {
    response = "ERRO: Falha na requisição. " + String(http.errorToString(httpResponseCode).c_str());
  }
  http.end();
  return response;
}

// Tenta enviar imediatamente; se falhar (sem WiFi, planilha fora do ar, etc.),
// guarda o payload na fila local em vez de descartar o dado.
bool enviarOuEnfileirar(const String& jsonPayload) {
  if (WiFi.status() == WL_CONNECTED) {
    String resposta = fazerRequisicao(jsonPayload);
    if (!resposta.startsWith("ERRO")) {
      return true;
    }
  }
  enfileirar(jsonPayload);
  return false;
}

bool escreverEmLista(const String& identificacaoEstacao, int numDados, float dados[]) {
  StaticJsonDocument<256> jsonDoc;
  jsonDoc["action"] = "escreverEmLista";
  jsonDoc["identificacao"] = identificacaoEstacao;
  JsonArray jsonDados = jsonDoc.createNestedArray("dados");
  char buffer[10];
  for (int i = 0; i < numDados; i++) {
    dtostrf(dados[i], 4, 2, buffer);
    jsonDados.add(String(buffer));
  }
  String jsonString;
  serializeJson(jsonDoc, jsonString);
  return enviarOuEnfileirar(jsonString);
}

// Grava uma linha de log (categoria/evento/resultado/detalhe) na aba "Log" da planilha.
bool escreverLogEmLista(const std::vector<String>& valores) {
  StaticJsonDocument<384> jsonDoc;
  jsonDoc["action"] = "escreverEmLista";
  jsonDoc["identificacao"] = "Log";
  JsonArray jsonDados = jsonDoc.createNestedArray("dados");
  for (size_t i = 0; i < valores.size(); i++) {
    jsonDados.add(valores[i]);
  }
  String jsonString;
  serializeJson(jsonDoc, jsonString);
  return enviarOuEnfileirar(jsonString);
}

String lerCelula(const String& identificacaoEstacao, const String& celula) {
  StaticJsonDocument<200> jsonDoc;
  jsonDoc["action"] = "lerCelula";
  jsonDoc["identificacao"] = identificacaoEstacao;
  jsonDoc["celula"] = celula;
  String jsonString;
  serializeJson(jsonDoc, jsonString);
  return fazerRequisicao(jsonString);
}

bool escreverEmCelula(const String& identificacaoEstacao, const String& celula, const String& dado) {
  StaticJsonDocument<200> jsonDoc;
  jsonDoc["action"] = "escreverEmCelula";
  jsonDoc["identificacao"] = identificacaoEstacao;
  jsonDoc["celula"] = celula;
  jsonDoc["dado"] = dado;
  String jsonString;
  serializeJson(jsonDoc, jsonString);
  String response = fazerRequisicao(jsonString);
  return !response.startsWith("ERRO");
}

void montarCabecalho(const String& boardID, const String& colunaInicial, const std::vector<String>& cabecalhos) {
  String celulaVerificacao = lerCelula(boardID, colunaInicial + "1");
  if (celulaVerificacao != cabecalhos[0]) {
    Serial.println("Cabeçalho não encontrado. Configurando a planilha...");
    char coluna = colunaInicial[0];
    for (size_t i = 0; i < cabecalhos.size(); i++) {
      String celulaAlvo = String(coluna) + "1";
      escreverEmCelula(boardID, celulaAlvo, cabecalhos[i]);
      coluna++;
    }
  } else {
    Serial.println("Cabeçalho da planilha já está correto.");
  }
}

void adicionarLogRam(const String& linha) {
  logRecentes[logRecentesIndice] = linha;
  logRecentesIndice = (logRecentesIndice + 1) % MAX_LOG_RAM;
  if (logRecentesTotal < MAX_LOG_RAM) logRecentesTotal++;
}

// Registra um evento (relé ligado/desligado, sincronização NTP, gravação de sensor, etc.)
// na aba "Log" da planilha (com fallback para a fila local em caso de falha) e mantém
// uma cópia em memória para exibição imediata no dashboard.
void registrarLog(const String& categoria, const String& evento, const String& resultado, const String& detalhe) {
  struct tm horaAtual;
  char horaTexto[9] = "--:--:--";
  if (obterHoraAtual(horaAtual)) {
    snprintf(horaTexto, sizeof(horaTexto), "%02d:%02d:%02d", horaAtual.tm_hour, horaAtual.tm_min, horaAtual.tm_sec);
  }

  std::vector<String> valores = {identificacao, categoria, evento, resultado, detalhe};
  escreverLogEmLista(valores);

  String linhaRam = "[" + String(horaTexto) + "] " + categoria + "/" + evento + " -> " + resultado;
  if (detalhe.length() > 0) linhaRam += " (" + detalhe + ")";
  adicionarLogRam(linhaRam);
  Serial.println("LOG: " + linhaRam);
}

// G2 = override Setor 1 | H2 = override LED | I2 = reset | J2 = override Setor 2
// (mesmas colunas G/H/I já usadas nas planilhas existentes; J2 é nova)
void sincronizarComPlanilha() {
  bool statusBotao = (digitalRead(BOTAO_PIN) == LOW);

  if (!isnan(ultimaUmidade) && !isnan(ultimaTemperatura)) {
    float dadosParaEnviar[3] = {ultimaUmidade, ultimaTemperatura, (float)statusBotao};
    bool enviouAgora = escreverEmLista(identificacao, 3, dadosParaEnviar);
    registrarLog("Sensor", "leitura_temp_umidade", enviouAgora ? "SUCESSO" : "FALHA_ENFILEIRADO", "");
  } else {
    registrarLog("Sensor", "leitura_temp_umidade", "FALHA", "Sensor DHT22 sem leitura valida");
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("PLANILHA: Sem WiFi, dados enfileirados localmente; pulando leitura de overrides.");
    return;
  }

  overrideSetor1 = lerCelula(identificacao, "G2");
  overrideLed = lerCelula(identificacao, "H2");
  overrideSetor2 = lerCelula(identificacao, "J2");

  if (overrideLed == "1") {
    digitalWrite(LED_PIN, HIGH);
    estadoLed = true;
  } else if (overrideLed == "0") {
    digitalWrite(LED_PIN, LOW);
    estadoLed = false;
  }

  String valorReset = lerCelula(identificacao, "I2");
  valorReset.trim();
  if (valorReset == "1") {
    Serial.println("PLANILHA: Comando de reset recebido.");
    if (escreverEmCelula(identificacao, "I2", "0")) {
      Serial.println("PLANILHA: Célula de reset limpa. Reiniciando em 2s...");
      delay(2000);
      ESP.restart();
    } else {
      Serial.println("PLANILHA: Falha ao limpar célula de reset. Reset abortado.");
    }
  }
}

// ====================================================================
// HORÁRIO (NTP) E CONTROLE DOS RELÉS
// ====================================================================
bool obterHoraAtual(struct tm &agora) {
  return getLocalTime(&agora, 1000);
}

bool dentroDeAlgumaFaixa(const struct tm &agora, FaixaHorario faixas[], int numFaixas) {
  int minutosAgora = agora.tm_hour * 60 + agora.tm_min;
  for (int i = 0; i < numFaixas; i++) {
    int minutosLiga = faixas[i].horaLiga * 60 + faixas[i].minLiga;
    int minutosDesliga = faixas[i].horaDesliga * 60 + faixas[i].minDesliga;
    if (minutosAgora >= minutosLiga && minutosAgora < minutosDesliga) {
      return true;
    }
  }
  return false;
}

// valorOverride "1"/"0" força o relé; qualquer outro valor (vazio, etc.) segue a tabela de horários.
// Só registra log quando o estado realmente muda (evita spammar a planilha a cada segundo).
void aplicarOverrideOuHorario(const String& nomeRele, const String& valorOverride, bool ligarPorHorario, int pino, bool &estadoAtual) {
  bool ligar = (valorOverride == "1") ? true
             : (valorOverride == "0") ? false
             : ligarPorHorario;
  if (ligar != estadoAtual) {
    registrarLog("Rele", nomeRele, ligar ? "LIGADO" : "DESLIGADO", "");
  }
  digitalWrite(pino, ligar ? RELE_LIGADO : RELE_DESLIGADO);
  estadoAtual = ligar;
}

void verificarEAplicarRele() {
  struct tm horaAtual;
  if (!obterHoraAtual(horaAtual)) {
    return; // hora ainda não sincronizada; mantém último estado
  }
  bool ligarSetor1 = dentroDeAlgumaFaixa(horaAtual, horariosSetor1, NUM_HORARIOS_SETOR1);
  bool ligarSetor2 = dentroDeAlgumaFaixa(horaAtual, horariosSetor2, NUM_HORARIOS_SETOR2);
  aplicarOverrideOuHorario("Setor1", overrideSetor1, ligarSetor1, RELE_SETOR1_PIN, estadoSetor1);
  aplicarOverrideOuHorario("Setor2", overrideSetor2, ligarSetor2, RELE_SETOR2_PIN, estadoSetor2);
}

// ====================================================================
// SERVIDOR WEB / DASHBOARD
// ====================================================================
String montarPaginaDashboard() {
  struct tm horaAtual;
  bool temHora = obterHoraAtual(horaAtual);
  char horaTexto[9] = "--:--:--";
  if (temHora) {
    snprintf(horaTexto, sizeof(horaTexto), "%02d:%02d:%02d", horaAtual.tm_hour, horaAtual.tm_min, horaAtual.tm_sec);
  }

  String temperaturaTexto = isnan(ultimaTemperatura) ? String("--") : String(ultimaTemperatura, 1);
  String umidadeTexto = isnan(ultimaUmidade) ? String("--") : String(ultimaUmidade, 1);

  String html = "<!DOCTYPE html><html lang='pt-BR'><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='15'>";
  html += "<title>Automação " + identificacao + "</title>";
  html += "<style>";
  html += "body{font-family:Arial,sans-serif;background:#f4f6f5;margin:0;padding:24px;color:#222;}";
  html += "h1{font-size:1.4rem;margin-bottom:4px;}";
  html += ".sub{color:#666;margin-bottom:20px;}";
  html += ".cards{display:flex;flex-wrap:wrap;gap:16px;}";
  html += ".card{background:#fff;border-radius:10px;padding:16px 20px;box-shadow:0 1px 4px rgba(0,0,0,0.12);min-width:150px;}";
  html += ".card .valor{font-size:1.8rem;font-weight:bold;}";
  html += ".card .rotulo{color:#666;font-size:0.85rem;}";
  html += ".on{color:#1a7a1a;} .off{color:#a33;}";
  html += ".fila{color:" + String(filaTamanho > 0 ? "#a33" : "#1a7a1a") + ";}";
  html += "ul.eventos{list-style:none;padding:0;margin:8px 0 0 0;}";
  html += "ul.eventos li{background:#fff;border-radius:6px;padding:8px 12px;margin-bottom:6px;font-size:0.85rem;box-shadow:0 1px 3px rgba(0,0,0,0.08);}";
  html += "</style></head><body>";
  html += "<h1>Estação " + identificacao + "</h1>";
  html += "<div class='sub'>IP local: " + WiFi.localIP().toString() + " &middot; Hora: " + String(horaTexto) + "</div>";
  html += "<div class='cards'>";
  html += "<div class='card'><div class='valor'>" + temperaturaTexto + " &deg;C</div><div class='rotulo'>Temperatura</div></div>";
  html += "<div class='card'><div class='valor'>" + umidadeTexto + " %</div><div class='rotulo'>Umidade</div></div>";
  html += "<div class='card'><div class='valor " + String(estadoSetor1 ? "on" : "off") + "'>" + String(estadoSetor1 ? "LIGADO" : "DESLIGADO") + "</div><div class='rotulo'>Irrigação Setor 1</div></div>";
  html += "<div class='card'><div class='valor " + String(estadoSetor2 ? "on" : "off") + "'>" + String(estadoSetor2 ? "LIGADO" : "DESLIGADO") + "</div><div class='rotulo'>Irrigação Setor 2</div></div>";
  html += "<div class='card'><div class='valor " + String(estadoLed ? "on" : "off") + "'>" + String(estadoLed ? "LIGADO" : "DESLIGADO") + "</div><div class='rotulo'>LED</div></div>";
  html += "<div class='card'><div class='valor fila'>" + String(filaTamanho) + "</div><div class='rotulo'>Pendentes p/ sincronizar</div></div>";
  html += "</div>";
  html += "<h2 style='margin-top:28px;font-size:1.05rem;'>Eventos recentes</h2>";
  html += "<ul class='eventos'>";
  if (logRecentesTotal == 0) {
    html += "<li>Nenhum evento registrado ainda.</li>";
  } else {
    for (int i = 0; i < logRecentesTotal; i++) {
      int indice = (logRecentesIndice - 1 - i + MAX_LOG_RAM) % MAX_LOG_RAM;
      html += "<li>" + logRecentes[indice] + "</li>";
    }
  }
  html += "</ul>";
  html += "<p style='margin-top:24px;color:#999;font-size:0.8rem;'>Atualiza automaticamente a cada 15s. Hor&aacute;rio sincronizado via NTP";
  html += horaSincronizada ? "." : " (falhou, tentando novamente).";
  html += "</p>";
  html += "</body></html>";
  return html;
}

void tratarRequisicaoRaiz() {
  server.send(200, "text/html; charset=UTF-8", montarPaginaDashboard());
}

// ====================================================================
// SETUP
// ====================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- INICIANDO SETUP ---");

  pinMode(RELE_SETOR1_PIN, OUTPUT);
  pinMode(RELE_SETOR2_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BOTAO_PIN, INPUT_PULLUP);
  digitalWrite(RELE_SETOR1_PIN, RELE_DESLIGADO);
  digitalWrite(RELE_SETOR2_PIN, RELE_DESLIGADO);
  digitalWrite(LED_PIN, LOW);

  dht.begin();

  littleFsDisponivel = LittleFS.begin(true);
  if (littleFsDisponivel) {
    filaTamanho = contarLinhasFila();
    Serial.printf("FILA: LittleFS pronto. %d registro(s) pendente(s) de antes do boot.\n", filaTamanho);
  } else {
    Serial.println("FILA: Falha ao montar LittleFS — fila local ficará desabilitada nesta sessão.");
  }

  if (!conectarWiFi()) {
    Serial.println("WIFI: Não foi possível conectar em nenhuma rede conhecida. Reiniciando em 10s...");
    delay(10000);
    ESP.restart();
  }

  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2);
  struct tm horaTeste;
  horaSincronizada = getLocalTime(&horaTeste, 5000);
  if (horaSincronizada) {
    Serial.printf("NTP: Hora sincronizada -> %02d:%02d:%02d\n", horaTeste.tm_hour, horaTeste.tm_min, horaTeste.tm_sec);
  } else {
    Serial.println("NTP: Falha ao sincronizar hora (segue tentando em segundo plano).");
  }
  registrarLog("NTP", "sincronizacao_hora", horaSincronizada ? "SUCESSO" : "FALHA", "");

  montarCabecalho(identificacao, "A", {"Data completa", "Data", "Hora", "Umidade", "Temperatura", "Botao", "Rele_Setor1_Planilha", "Led_Planilha", "Reset", "Rele_Setor2_Planilha"});
  montarCabecalho("Log", "A", {"Data completa", "Data", "Hora", "Estacao", "Categoria", "Evento", "Resultado", "Detalhe"});

  String nomeMDNS = "automacao-" + identificacao;
  nomeMDNS.toLowerCase();
  if (MDNS.begin(nomeMDNS.c_str())) {
    Serial.printf("mDNS: Dashboard também acessível em http://%s.local\n", nomeMDNS.c_str());
  }

  server.on("/", tratarRequisicaoRaiz);
  server.begin();
  Serial.println("WEB: Servidor iniciado na porta 80.");

  digitalWrite(LED_PIN, HIGH);
  estadoLed = true;
  Serial.println("--- SETUP CONCLUÍDO ---");
}

// ====================================================================
// LOOP (não bloqueante)
// ====================================================================
void loop() {
  server.handleClient();

  unsigned long agoraMs = millis();

  if (agoraMs - ultimaLeituraDht >= INTERVALO_LEITURA_DHT_MS) {
    ultimaLeituraDht = agoraMs;
    float umidade = dht.readHumidity();
    float temperatura = dht.readTemperature();
    if (!isnan(umidade) && !isnan(temperatura)) {
      ultimaUmidade = umidade;
      ultimaTemperatura = temperatura;
    } else {
      Serial.println("LOOP: Falha ao ler o sensor DHT22.");
    }
  }

  if (agoraMs - ultimaVerificacaoRele >= INTERVALO_VERIFICA_RELE_MS) {
    ultimaVerificacaoRele = agoraMs;
    verificarEAplicarRele();
  }

  if (ultimoSyncPlanilha == 0 || agoraMs - ultimoSyncPlanilha >= INTERVALO_SYNC_PLANILHA_MS) {
    ultimoSyncPlanilha = agoraMs;
    sincronizarComPlanilha();
  }

  if (agoraMs - ultimaTentativaFila >= INTERVALO_TENTATIVA_FILA_MS) {
    ultimaTentativaFila = agoraMs;
    tentarEsvaziarFila();
  }
}
