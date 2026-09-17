# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Visão geral do projeto

Firmware para automação de um sítio em Alagoas: uma placa ESP32 DOIT DEVKIT V1 com sensor DHT22 que monitora temperatura/umidade, controla dois relés de irrigação por tabela de horários, expõe um dashboard web local e sincroniza tudo com uma planilha Google via um Google Apps Script Web App (backend REST simples). Não há build system nem suíte de testes — é um conjunto de sketches Arduino, verificados com `arduino-cli compile` (core `esp32:esp32`, ver seção "Compilar" abaixo) e gravados pela Arduino IDE.

## Estrutura do repositório

**Cada sketch Arduino vive na sua própria pasta, com o `.ino` tendo o mesmo nome da pasta** — convenção obrigatória do Arduino: a IDE (e o `arduino-cli`) trata todo `.ino` que estiver numa mesma pasta como parte de um único sketch, então dois sketches diferentes nunca podem compartilhar uma pasta (já causou erro de `setup()`/`loop()` duplicado quando os arquivos estavam soltos na raiz — não volte a colocá-los soltos).

- **`automacao_sitio/automacao_sitio.ino`** — **firmware ativo, único, usado em todas as estações.** Ver arquitetura detalhada abaixo.
- `UmidTempDth22Arapiraca/`, `UmidTempDth22BeloMonte/`, `umidadetemperatura_legado/`, `googleplanilhas_Modelo/` — sketches antigos, mantidos só como referência histórica; **não usar para novas gravações**. `googleplanilhas_Modelo.ino` não compila sozinho (sem `setup()`/`loop()`), é só um template de funções HTTP.
- `ScriptPlanilha_GOOGLEDrive` — o Google Apps Script (JavaScript, handler `doPost`) que precisa ser implantado como Web App do lado da planilha; é para onde `googleScriptURL` aponta. Cria abas automaticamente se não existirem (estação nova ou a aba "Log"). **Qualquer alteração aqui exige reimplantar o Web App** (Implantar → Gerenciar implantações → editar → Nova versão) — só salvar o código não atualiza a versão publicada que o ESP32 chama.
- `PromptGrafico.md` — template de prompt para gerar um dashboard climático comparativo (BI externo). Não é código.
- O README menciona uma terceira estação, "União dos Palmares", ainda sem WiFi cadastrado no firmware.

## Compilar / verificar

Não tem Arduino IDE necessariamente disponível no ambiente do Claude Code — use `arduino-cli`:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32doit-devkit-v1 automacao_sitio
```

Rode isso (ou peça pro usuário rodar na IDE) depois de qualquer mudança em `automacao_sitio/automacao_sitio.ino` — o arquivo já teve erro de compilação real (overloads ambíguos por causa da estrutura de pastas errada) que só apareceu na primeira compilação de verdade. Não assuma que compila só de ler o código.

Bibliotecas necessárias (`arduino-cli lib install "..."` ou Gerenciador de Bibliotecas da IDE): `DHT sensor library` (Adafruit), `Adafruit Unified Sensor`, `ArduinoJson`. `WiFi.h`, `HTTPClient.h`, `WebServer.h`, `ESPmDNS.h`, `LittleFS.h`/`FS.h` já vêm no Core ESP32.

## Arquitetura de `automacao_sitio.ino`

**Identificação automática da estação por WiFi:** não há mais um `.ino` por estação. No boot, `conectarWiFi()` tenta cada rede de `redesConhecidas[]` (struct `{ssid, senha, identificacao}`) em ordem; a primeira que conectar define a variável global `identificacao`, usada como nome da aba na planilha. Para adicionar uma estação nova, só adicionar uma entrada nesse array — não criar um novo `.ino`.

**Sem deep sleep**, ao contrário dos sketches antigos: o loop é não bloqueante (baseado em `millis()`), porque o servidor web e o controle dos relés precisam responder a qualquer momento. Três cadências independentes, cada uma com seu próprio intervalo — **não junte tudo num só ciclo**, isso já foi pedido e recusado de propósito (ver histórico): as janelas de irrigação são de 30 min, então a verificação do relé (`INTERVALO_VERIFICA_RELE_MS`, 1s) tem que ficar independente do envio à planilha (`INTERVALO_SYNC_PLANILHA_MS`, 40 min) — senão o relé liga/desliga atrasado e passa da duração da janela.

- Leitura DHT22: a cada 5s (`INTERVALO_LEITURA_DHT_MS`), alimenta o dashboard.
- Verificação/aplicação dos relés: a cada 1s (`INTERVALO_VERIFICA_RELE_MS`), compara a hora (NTP) contra `horariosSetor1[]`/`horariosSetor2[]` (`dentroDeAlgumaFaixa`), aplicando override manual da planilha quando presente (`aplicarOverrideOuHorario` — só loga quando o estado muda).
- Sync com a planilha: a cada 40 min (`INTERVALO_SYNC_PLANILHA_MS`), grava a leitura do sensor e lê os overrides (G2/H2/I2/J2).
- Retry da fila local: a cada 2 min (`INTERVALO_TENTATIVA_FILA_MS`).

**Colunas da planilha (aba por estação):** A-F = timestamp/data/hora/umidade/temperatura/botão (escritas automaticamente). G2 = override Setor 1 (`1`/`0`/vazio=segue horário), H2 = override LED, I2 = comando de reset remoto (firmware limpa para "0" sozinho), J2 = override Setor 2. Não mexer na ordem dessas colunas sem atualizar tanto `montarCabecalho()` quanto os índices usados em `sincronizarComPlanilha()`.

**Log de eventos (aba "Log"):** toda mudança de estado de relé, toda sincronização NTP (uma vez no boot) e todo envio de sensor geram uma linha via `registrarLog()` (categoria/evento/resultado/detalhe). Isso passa pela mesma fila de resiliência abaixo.

**Fila local (LittleFS):** `escreverEmLista`/`escreverLogEmLista` passam por `enviarOuEnfileirar()` — se o POST falhar, o payload JSON é gravado como uma linha em `/fila.jsonl` na flash (`enfileirar()`), e `tentarEsvaziarFila()` tenta reenviar periodicamente, sobrevivendo a reboots. Limite de 150 registros (`MAX_REGISTROS_FILA`); estoura, descarta o mais antigo. **`escreverEmCelula()` (usado só para limpar o reset e escrever cabeçalho) não passa por essa fila de propósito** — se a limpeza do reset falhar, o firmware aborta o restart em vez de enfileirar, pra não cair num boot loop.

**Dashboard web local:** `WebServer` na porta 80 + `ESPmDNS` (`http://automacao-<identificacao minúsculo>.local`). Mostra leitura atual, estado dos relés/LED, fila pendente (`filaTamanho`) e os últimos eventos (ring buffer em RAM, `logRecentes[]`, não persistido).

Pinos: `RELE_SETOR1_PIN=25`, `RELE_SETOR2_PIN=33` (evite pinos input-only como GPIO34-39 se adicionar mais saídas), `LED_PIN=26`, `BOTAO_PIN=27` (INPUT_PULLUP), `DHT_PIN=4`. Tabela completa com fiação em [README.md](README.md).

## Coisas para não fazer de novo

- Não coloque dois `.ino` de sketches diferentes na mesma pasta (ver "Compilar" acima).
- Não faça a verificação do relé compartilhar cadência com o envio à planilha.
- SSID/senha do WiFi e a URL do Apps Script estão hardcoded no `.ino` como credenciais reais já commitadas — preserve-as ao editar, a menos que o usuário peça pra rotacionar.
