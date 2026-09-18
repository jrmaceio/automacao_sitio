#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <time.h>
#include <vector>
#include <FS.h>
#include <LittleFS.h>
#include <esp_system.h>

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
// Ajustável conforme o módulo físico: alguns energizam o relé com IN em HIGH,
// outros com IN em LOW. Teste antes de ligar a carga real (bomba/válvula): ao gravar,
// o relé deve ficar DESLIGADO logo no boot, e LIGAR quando o comando de ligar chega
// (pelo horário, override da planilha ou controle manual). Se estiver ao contrário
// (liga sozinho no boot / desliga quando devia ligar), inverta o valor abaixo.
const bool RELE_ATIVO_EM_LOW = false;
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

// Override manual temporizado, acionado pelo dashboard local (independe da planilha
// e do NTP): 0 = nenhum override ativo; caso contrário, millis() em que expira e o
// relé volta a seguir a planilha/tabela de horários automaticamente.
unsigned long manualSetor1AteMs = 0;
unsigned long manualSetor2AteMs = 0;
const int MANUAL_MINUTOS_MAXIMO = 180; // limite de segurança: no máx. 3h de override manual

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
// Verifica se o WiFi ainda está conectado a cada 30s; se ficar caído por tempo
// demais (falta de energia no roteador, etc.) e não conseguir reconectar sozinho,
// reinicia o ESP32 — mais confiável do que tentar recuperar a pilha WiFi em runtime.
const unsigned long INTERVALO_VERIFICA_WIFI_MS = 30000;       // 30s
const unsigned long LIMITE_WIFI_SEM_CONEXAO_MS = 5UL * 60000UL; // 5 min
// Overrides/comandos remotos (G2/H2/I2/J2/K2/L2) e status (M2) são lidos/escritos numa
// cadência própria, bem mais curta que o envio do sensor (40 min) — senão ligar pela
// planilha de outra cidade poderia demorar até 40 min pra fazer efeito.
const unsigned long INTERVALO_LEITURA_COMANDOS_MS = 120000;   // 2 min

unsigned long ultimaLeituraDht = 0;
unsigned long ultimaVerificacaoRele = 0;
unsigned long ultimoSyncPlanilha = 0;
unsigned long ultimaTentativaFila = 0;
unsigned long ultimaVerificacaoWifi = 0;
unsigned long ultimaLeituraComandos = 0;
unsigned long wifiCaidoDesdeMs = 0; // 0 = conectado (ou queda ainda não detectada)

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

// Chamada periodicamente no loop(). Detecta queda de WiFi em runtime (ex.: falta de
// energia no roteador), tenta reconectar sozinho e, se não conseguir dentro de
// LIMITE_WIFI_SEM_CONEXAO_MS, reinicia o ESP32 — o evento fica registrado no log
// (na fila local, já que sem WiFi não há como enviar na hora) tanto na queda quanto
// na recuperação/reinício.
void verificarConexaoWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (wifiCaidoDesdeMs != 0) {
      registrarLog("WiFi", "conexao", "RECUPERADO", "Reconectado apos queda");
      wifiCaidoDesdeMs = 0;
    }
    return;
  }

  unsigned long agoraMs = millis();
  if (wifiCaidoDesdeMs == 0) {
    wifiCaidoDesdeMs = agoraMs;
    Serial.println("WIFI: Conexão perdida. Tentando reconectar...");
    registrarLog("WiFi", "conexao", "FALHA", "Conexao perdida, tentando reconectar");
    WiFi.reconnect();
    return;
  }

  if (agoraMs - wifiCaidoDesdeMs >= LIMITE_WIFI_SEM_CONEXAO_MS) {
    Serial.println("WIFI: Sem conexão há 5+ min. Reiniciando...");
    registrarLog("WiFi", "conexao", "FALHA", "Sem conexao ha 5+ min; reiniciando o ESP32");
    delay(500);
    ESP.restart();
  } else {
    WiFi.reconnect();
  }
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

// Verifica a ÚLTIMA célula do cabeçalho (não só a primeira): assim, se novas colunas
// forem adicionadas a `cabecalhos` num sketch novo, uma planilha já em uso (que já tem
// A1 preenchido de antes) ainda recebe as colunas novas em vez de ficar sem cabeçalho nelas.
void montarCabecalho(const String& boardID, const String& colunaInicial, const std::vector<String>& cabecalhos) {
  char colunaFinal = colunaInicial[0] + (cabecalhos.size() - 1);
  String celulaVerificacao = lerCelula(boardID, String(colunaFinal) + "1");
  if (celulaVerificacao != cabecalhos[cabecalhos.size() - 1]) {
    Serial.println("Cabeçalho incompleto/desatualizado. Configurando a planilha...");
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

  // Antes do WiFi conectar a identificação da estação ainda não é conhecida;
  // usa um rótulo fixo em vez de deixar a coluna vazia na aba "Log".
  String estacaoLog = identificacao.length() > 0 ? identificacao : "Desconhecida";
  std::vector<String> valores = {estacaoLog, categoria, evento, resultado, detalhe};
  escreverLogEmLista(valores);

  String linhaRam = "[" + String(horaTexto) + "] " + categoria + "/" + evento + " -> " + resultado;
  if (detalhe.length() > 0) linhaRam += " (" + detalhe + ")";
  adicionarLogRam(linhaRam);
  Serial.println("LOG: " + linhaRam);
}

// Envia a leitura do sensor + botão para a planilha. Overrides/comandos remotos e o
// status atual são tratados à parte, em lerComandosDaPlanilha() (cadência própria,
// bem mais frequente — ver comentário lá) para não depender do ciclo de 40 min daqui.
void sincronizarComPlanilha() {
  bool statusBotao = (digitalRead(BOTAO_PIN) == LOW);

  if (!isnan(ultimaUmidade) && !isnan(ultimaTemperatura)) {
    float dadosParaEnviar[3] = {ultimaUmidade, ultimaTemperatura, (float)statusBotao};
    bool enviouAgora = escreverEmLista(identificacao, 3, dadosParaEnviar);
    registrarLog("Sensor", "leitura_temp_umidade", enviouAgora ? "SUCESSO" : "FALHA_ENFILEIRADO", "");
  } else {
    registrarLog("Sensor", "leitura_temp_umidade", "FALHA", "Sensor DHT22 sem leitura valida");
  }
}

// Lê a célula de comando remoto (K2/L2 = "ligar Setor N por X minutos"), aplica e limpa
// a célula de volta pra "" — só depois de limpar com sucesso é que o override é ativado,
// pra não reiniciar o cronômetro a cada ciclo caso a limpeza falhe e o valor continue lá.
void lerComandoManualRemoto(const String& nomeRele, const String& celula, unsigned long &manualAteMs) {
  String comando = lerCelula(identificacao, celula);
  comando.trim();
  if (comando.length() == 0) return;

  int minutos = comando.toInt();
  if (minutos <= 0) {
    escreverEmCelula(identificacao, celula, ""); // valor inválido/lixo: limpa e ignora
    return;
  }

  if (escreverEmCelula(identificacao, celula, "")) {
    iniciarOverrideManual(nomeRele, manualAteMs, minutos);
    registrarLog("Rele", nomeRele, "MANUAL_LIGADO", String(minutos) + " min (via planilha, remoto)");
  } else {
    Serial.println("PLANILHA: Falha ao limpar comando remoto de " + nomeRele + "; tenta de novo no proximo ciclo.");
  }
}

// "HORARIO" = seguindo a tabela | "PLANILHA" = override permanente G2/J2 | "MANUAL" = override
// temporizado ativo (dashboard local ou comando remoto K2/L2 — ambos usam o mesmo mecanismo).
String origemAplicada(bool manualAtivo, const String& overrideValor) {
  if (manualAtivo) return "MANUAL";
  if (overrideValor == "1" || overrideValor == "0") return "PLANILHA";
  return "HORARIO";
}

// Escreve um resumo do estado atual (relés, LED, overrides ativos, fila pendente) numa
// única célula (M2), pra dar pra entender tudo que está valendo agora só de olhar a
// planilha — sem precisar rolar o log. Complementa o log, não substitui: o log mostra o
// histórico de mudanças, o M2 mostra a "foto" do momento.
void atualizarStatusNaPlanilha() {
  struct tm horaAtual;
  char horaTexto[9] = "--:--:--";
  if (obterHoraAtual(horaAtual)) {
    snprintf(horaTexto, sizeof(horaTexto), "%02d:%02d:%02d", horaAtual.tm_hour, horaAtual.tm_min, horaAtual.tm_sec);
  }

  unsigned long agoraMs = millis();
  long restante1Min = 0;
  if (manualSetor1AteMs != 0) {
    long diffMs = (long)(manualSetor1AteMs - agoraMs);
    restante1Min = diffMs > 0 ? diffMs / 60000 : 0;
  }
  long restante2Min = 0;
  if (manualSetor2AteMs != 0) {
    long diffMs = (long)(manualSetor2AteMs - agoraMs);
    restante2Min = diffMs > 0 ? diffMs / 60000 : 0;
  }

  String status = "Setor1=" + String(estadoSetor1 ? "LIGADO" : "DESLIGADO");
  status += ";Origem1=" + origemAplicada(manualSetor1AteMs != 0, overrideSetor1);
  status += ";ManualRestanteMin1=" + String(restante1Min);
  status += ";Setor2=" + String(estadoSetor2 ? "LIGADO" : "DESLIGADO");
  status += ";Origem2=" + origemAplicada(manualSetor2AteMs != 0, overrideSetor2);
  status += ";ManualRestanteMin2=" + String(restante2Min);
  status += ";LED=" + String(estadoLed ? "LIGADO" : "DESLIGADO");
  status += ";FilaPendente=" + String(filaTamanho);
  status += ";Atualizado=" + String(horaTexto);

  escreverEmCelula(identificacao, "M2", status);
}

// G2 = override Setor 1 | H2 = override LED | I2 = reset | J2 = override Setor 2 |
// K2 = ligar Setor 1 remoto (minutos) | L2 = ligar Setor 2 remoto (minutos) | M2 = status atual.
// Roda a cada INTERVALO_LEITURA_COMANDOS_MS (2 min, independente do envio do sensor a cada
// 40 min) para que ligar remotamente pela planilha — de outra cidade — tenha efeito em minutos.
void lerComandosDaPlanilha() {
  if (WiFi.status() != WL_CONNECTED) return;

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

  lerComandoManualRemoto("Setor1", "K2", manualSetor1AteMs);
  lerComandoManualRemoto("Setor2", "L2", manualSetor2AteMs);

  verificarEAplicarRele(); // aplica overrides/comandos na hora, sem esperar o próximo ciclo de 1s
  atualizarStatusNaPlanilha();
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

// Liga (minutos > 0) ou cancela (minutos <= 0) o override manual temporizado de um setor.
void iniciarOverrideManual(const String& nomeRele, unsigned long &manualAteMs, int minutos) {
  if (minutos <= 0) {
    if (manualAteMs != 0) {
      manualAteMs = 0;
      registrarLog("Rele", nomeRele, "MANUAL_CANCELADO", "Retornando ao modo automatico");
    }
    return;
  }
  if (minutos > MANUAL_MINUTOS_MAXIMO) minutos = MANUAL_MINUTOS_MAXIMO;
  manualAteMs = millis() + (unsigned long)minutos * 60000UL;
  registrarLog("Rele", nomeRele, "MANUAL_LIGADO", String(minutos) + " min");
}

// Expira overrides manuais vencidos (chamado a cada ciclo de verificação do relé).
void expirarOverrideManualSeVencido(const String& nomeRele, unsigned long &manualAteMs) {
  if (manualAteMs != 0 && (long)(millis() - manualAteMs) >= 0) {
    manualAteMs = 0;
    registrarLog("Rele", nomeRele, "MANUAL_EXPIRADO", "Retornando ao modo automatico");
  }
}

void verificarEAplicarRele() {
  expirarOverrideManualSeVencido("Setor1", manualSetor1AteMs);
  expirarOverrideManualSeVencido("Setor2", manualSetor2AteMs);

  struct tm horaAtual;
  bool horaOk = obterHoraAtual(horaAtual);
  bool ligarSetor1PorHorario = horaOk && dentroDeAlgumaFaixa(horaAtual, horariosSetor1, NUM_HORARIOS_SETOR1);
  bool ligarSetor2PorHorario = horaOk && dentroDeAlgumaFaixa(horaAtual, horariosSetor2, NUM_HORARIOS_SETOR2);

  // O override manual do dashboard tem prioridade sobre a planilha/horário e funciona
  // mesmo sem hora sincronizada (não depende de NTP); fora dele, segue a lógica normal.
  bool manual1Ativo = manualSetor1AteMs != 0;
  bool manual2Ativo = manualSetor2AteMs != 0;
  String valorEfetivoSetor1 = manual1Ativo ? "1" : overrideSetor1;
  String valorEfetivoSetor2 = manual2Ativo ? "1" : overrideSetor2;

  // Aplica sempre, mesmo sem NTP: ligarXPorHorario já é false quando horaOk é false,
  // e overrides manual/planilha não dependem de NTP. Um guard "manual1Ativo || horaOk"
  // existiu aqui e causava bug: cancelar o manual antes do primeiro sync NTP não
  // desligava o relé (nenhum digitalWrite era emitido, pino ficava travado em LIGADO).
  aplicarOverrideOuHorario("Setor1", valorEfetivoSetor1, ligarSetor1PorHorario, RELE_SETOR1_PIN, estadoSetor1);
  aplicarOverrideOuHorario("Setor2", valorEfetivoSetor2, ligarSetor2PorHorario, RELE_SETOR2_PIN, estadoSetor2);
}

// ====================================================================
// SERVIDOR WEB / DASHBOARD
// ====================================================================
// Monta o card de controle manual de um setor (formulário para ligar por N minutos
// e botão para cancelar um override manual em andamento).
String montarCardManual(int setor, unsigned long manualAteMs, bool estadoAtual) {
  String s = String(setor);
  String html = "<div class='card'>";
  html += "<div class='rotulo'>Setor " + s + " &middot; <span class='" + String(estadoAtual ? "on" : "off") + "'>" + String(estadoAtual ? "LIGADO" : "DESLIGADO") + "</span></div>";
  if (manualAteMs != 0) {
    long segundosRestantes = (long)(manualAteMs - millis()) / 1000;
    if (segundosRestantes < 0) segundosRestantes = 0;
    html += "<div class='aviso'>Manual ativo &mdash; falta" + String(segundosRestantes == 1 ? "" : "m") + " " + String(segundosRestantes / 60) + " min " + String(segundosRestantes % 60) + "s</div>";
    html += "<form method='GET' action='/manual'>";
    html += "<input type='hidden' name='setor' value='" + s + "'>";
    html += "<input type='hidden' name='minutos' value='0'>";
    html += "<button type='submit' class='cancelar'>Cancelar manual</button>";
    html += "</form>";
  } else {
    html += "<form method='GET' action='/manual'>";
    html += "<input type='hidden' name='setor' value='" + s + "'>";
    html += "<select name='minutos'>";
    html += "<option value='15'>15 min</option>";
    html += "<option value='30'>30 min</option>";
    html += "<option value='60'>1 h</option>";
    html += "<option value='120'>2 h</option>";
    html += "</select>";
    html += "<button type='submit'>Ligar manual</button>";
    html += "</form>";
  }
  html += "</div>";
  return html;
}

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
  html += ".manual{display:flex;flex-wrap:wrap;gap:16px;margin-top:8px;}";
  html += ".manual .card form{display:inline-flex;gap:6px;align-items:center;margin-top:8px;}";
  html += ".manual .card{min-width:220px;}";
  html += ".manual select,.manual button{font-size:0.85rem;padding:4px 8px;}";
  html += ".manual button{border:none;border-radius:5px;background:#1a7a1a;color:#fff;cursor:pointer;}";
  html += ".manual button.cancelar{background:#a33;}";
  html += ".manual .aviso{color:#a33;font-size:0.8rem;margin-top:4px;}";
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
  html += "<h2 style='margin-top:28px;font-size:1.05rem;'>Controle manual</h2>";
  html += "<div class='manual'>";
  html += montarCardManual(1, manualSetor1AteMs, estadoSetor1);
  html += montarCardManual(2, manualSetor2AteMs, estadoSetor2);
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

// GET /manual?setor=1|2&minutos=N — liga o setor manualmente por N minutos
// (minutos=0 cancela o override e devolve o controle ao horário/planilha).
void tratarRequisicaoManual() {
  if (!server.hasArg("setor") || !server.hasArg("minutos")) {
    server.send(400, "text/plain; charset=UTF-8", "Parametros invalidos. Use /manual?setor=1&minutos=15");
    return;
  }
  int setor = server.arg("setor").toInt();
  int minutos = server.arg("minutos").toInt();
  if (minutos < 0) minutos = 0;

  if (setor == 1) {
    iniciarOverrideManual("Setor1", manualSetor1AteMs, minutos);
  } else if (setor == 2) {
    iniciarOverrideManual("Setor2", manualSetor2AteMs, minutos);
  } else {
    server.send(400, "text/plain; charset=UTF-8", "Setor invalido. Use 1 ou 2.");
    return;
  }

  verificarEAplicarRele(); // aplica na hora, sem esperar o próximo ciclo de 1s
  server.sendHeader("Location", "/");
  server.send(303);
}

// ====================================================================
// MOTIVO DO REINÍCIO (detecta recuperação de falta de energia, brownout, etc.)
// ====================================================================
String motivoReset(esp_reset_reason_t motivo, bool &pareceRecuperacaoDeFalta) {
  pareceRecuperacaoDeFalta = (motivo == ESP_RST_POWERON || motivo == ESP_RST_BROWNOUT);
  switch (motivo) {
    case ESP_RST_POWERON:   return "Energizado (liga da fonte) - provavel recuperacao apos falta de energia";
    case ESP_RST_BROWNOUT:  return "Queda de tensao (brownout) - provavel falta de energia momentanea";
    case ESP_RST_EXT:       return "Reset externo (botao/pino EN)";
    case ESP_RST_SW:        return "Reset por software (ESP.restart() do proprio firmware)";
    case ESP_RST_PANIC:     return "Reset por panico/excecao no firmware";
    case ESP_RST_INT_WDT:   return "Reset por watchdog interno";
    case ESP_RST_TASK_WDT:  return "Reset por watchdog de tarefa (loop travado)";
    case ESP_RST_WDT:       return "Reset por outro watchdog";
    case ESP_RST_DEEPSLEEP: return "Retorno de deep sleep";
    case ESP_RST_SDIO:      return "Reset via SDIO";
    default:                return "Motivo desconhecido";
  }
}

// ====================================================================
// SETUP
// ====================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- INICIANDO SETUP ---");

  esp_reset_reason_t razaoReset = esp_reset_reason();

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
    // Sem identificação e sem WiFi ainda: fica só na fila local, enviado após um boot que conecte.
    registrarLog("WiFi", "conexao", "FALHA", "Nenhuma rede conhecida disponivel; reiniciando");
    delay(10000);
    ESP.restart();
  }

  bool pareceRecuperacaoDeFalta = false;
  String detalheReset = motivoReset(razaoReset, pareceRecuperacaoDeFalta);
  Serial.println("BOOT: " + detalheReset);
  registrarLog("Sistema", "boot", pareceRecuperacaoDeFalta ? "RECUPERADO" : "REINICIADO", detalheReset);

  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2);
  struct tm horaTeste;
  horaSincronizada = getLocalTime(&horaTeste, 5000);
  if (horaSincronizada) {
    Serial.printf("NTP: Hora sincronizada -> %02d:%02d:%02d\n", horaTeste.tm_hour, horaTeste.tm_min, horaTeste.tm_sec);
  } else {
    Serial.println("NTP: Falha ao sincronizar hora (segue tentando em segundo plano).");
  }
  registrarLog("NTP", "sincronizacao_hora", horaSincronizada ? "SUCESSO" : "FALHA", "");

  montarCabecalho(identificacao, "A", {"Data completa", "Data", "Hora", "Umidade", "Temperatura", "Botao", "Rele_Setor1_Planilha", "Led_Planilha", "Reset", "Rele_Setor2_Planilha", "Ligar_Setor1_Min", "Ligar_Setor2_Min", "Status_Atual"});
  montarCabecalho("Log", "A", {"Data completa", "Data", "Hora", "Estacao", "Categoria", "Evento", "Resultado", "Detalhe"});

  String nomeMDNS = "automacao-" + identificacao;
  nomeMDNS.toLowerCase();
  if (MDNS.begin(nomeMDNS.c_str())) {
    Serial.printf("mDNS: Dashboard também acessível em http://%s.local\n", nomeMDNS.c_str());
  }

  server.on("/", tratarRequisicaoRaiz);
  server.on("/manual", tratarRequisicaoManual);
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

  if (agoraMs - ultimaVerificacaoWifi >= INTERVALO_VERIFICA_WIFI_MS) {
    ultimaVerificacaoWifi = agoraMs;
    verificarConexaoWifi();
  }

  if (ultimaLeituraComandos == 0 || agoraMs - ultimaLeituraComandos >= INTERVALO_LEITURA_COMANDOS_MS) {
    ultimaLeituraComandos = agoraMs;
    lerComandosDaPlanilha();
  }
}
