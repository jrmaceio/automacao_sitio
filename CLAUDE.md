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
- Verificação/aplicação dos relés: a cada 1s (`INTERVALO_VERIFICA_RELE_MS`), compara a hora (NTP) contra `horariosSetor1[]`/`horariosSetor2[]` (`dentroDeAlgumaFaixa`), aplicando override manual da planilha ou o override manual temporizado (dashboard local ou planilha) quando presente (`aplicarOverrideOuHorario` — só loga quando o estado muda).
- Sync com a planilha: a cada 40 min (`INTERVALO_SYNC_PLANILHA_MS`), grava a leitura do sensor (`sincronizarComPlanilha`).
- Comandos/overrides da planilha: a cada 2 min (`INTERVALO_LEITURA_COMANDOS_MS`), lê G2/H2/I2/J2/K2/L2 e escreve o status em M2 (`lerComandosDaPlanilha`) — **cadência separada e bem mais curta que o envio do sensor de propósito**, pra ligar remotamente pela planilha (de outra cidade) fazer efeito em minutos, não em até 40 min.
- Retry da fila local: a cada 2 min (`INTERVALO_TENTATIVA_FILA_MS`).
- Verificação de WiFi: a cada 30s (`INTERVALO_VERIFICA_WIFI_MS`), ver "Recuperação de falhas" abaixo.

**Colunas da planilha (aba por estação):** A-F = timestamp/data/hora/umidade/temperatura/botão (escritas automaticamente a cada sync do sensor). Linha 2 tem células de controle/status, lidas e escritas por `lerComandosDaPlanilha()`/`atualizarStatusNaPlanilha()`:
- G2 = override permanente Setor 1 (`1`/`0`/vazio=segue horário), H2 = override LED, I2 = comando de reset remoto (firmware limpa para "0" sozinho), J2 = override permanente Setor 2.
- K2/L2 = **comando remoto para ligar manualmente** Setor 1/Setor 2 por N minutos (escreva um número, ex. `20`, na célula) — mesmo mecanismo do override temporizado do dashboard local (`iniciarOverrideManual`), útil pra ligar de outra cidade sem precisar estar na rede local. O firmware limpa a célula de volta pra "" depois de aplicar (só limpa e ativa se a escrita de limpeza funcionar, pra não reiniciar o cronômetro a cada ciclo se a limpeza falhar).
- M2 = **status atual** (somente leitura, sobrescrito a cada ciclo de 2 min por `atualizarStatusNaPlanilha()`), string tipo `Setor1=LIGADO;Origem1=HORARIO;ManualRestanteMin1=0;Setor2=DESLIGADO;Origem2=AUTO;ManualRestanteMin2=12;LED=LIGADO;FilaPendente=0;Atualizado=14:32:10` — dá pra ver tudo que está valendo agora só olhando essa célula, sem depender do log. `Origem` é `HORARIO` (seguindo a tabela), `PLANILHA` (override permanente G2/J2) ou `MANUAL` (override temporizado ativo, local ou via K2/L2).

Não mexer na ordem/posição dessas colunas sem atualizar `montarCabecalho()`, `lerComandosDaPlanilha()` e `atualizarStatusNaPlanilha()`. `montarCabecalho()` verifica a **última** célula do cabeçalho (não só a primeira) antes de decidir se reescreve — importante pra estações já em uso ganharem as colunas novas (K/L/M) automaticamente no próximo boot, sem precisar apagar a planilha.

**Log de eventos (aba "Log"):** toda mudança de estado de relé, todo boot (com o motivo do reset, ver abaixo), toda sincronização NTP (uma vez no boot), toda queda/recuperação de WiFi e todo envio de sensor geram uma linha via `registrarLog()` (categoria/evento/resultado/detalhe). Isso passa pela mesma fila de resiliência abaixo. `registrarLog()` usa "Desconhecida" como nome da estação quando `identificacao` ainda não foi definida (ex.: falha de WiFi antes de conectar em qualquer rede).

**Recuperação de falhas (boot e WiFi):** no boot, `esp_reset_reason()` é traduzido por `motivoReset()` e logado via `registrarLog("Sistema", "boot", ...)` — `ESP_RST_POWERON`/`ESP_RST_BROWNOUT` são tratados como possível recuperação de falta de energia (resultado `RECUPERADO`), os demais como `REINICIADO`. Em runtime, `verificarConexaoWifi()` (chamada a cada `INTERVALO_VERIFICA_WIFI_MS`) detecta queda de WiFi, tenta `WiFi.reconnect()` e, se continuar sem conexão por `LIMITE_WIFI_SEM_CONEXAO_MS` (5 min), força `ESP.restart()` — o log da falha fica na fila local (sem WiFi não há como enviar na hora) e é sincronizado no boot seguinte, assim como o log de `boot` desse reinício.

**Fila local (LittleFS):** `escreverEmLista`/`escreverLogEmLista` passam por `enviarOuEnfileirar()` — se o POST falhar, o payload JSON é gravado como uma linha em `/fila.jsonl` na flash (`enfileirar()`), e `tentarEsvaziarFila()` tenta reenviar periodicamente, sobrevivendo a reboots. Limite de 150 registros (`MAX_REGISTROS_FILA`); estoura, descarta o mais antigo. **`escreverEmCelula()` (usado só para limpar o reset e escrever cabeçalho) não passa por essa fila de propósito** — se a limpeza do reset falhar, o firmware aborta o restart em vez de enfileirar, pra não cair num boot loop.

**Override manual temporizado (dashboard local ou remoto pela planilha):** duas portas de entrada pro mesmo mecanismo (`iniciarOverrideManual`, limite `MANUAL_MINUTOS_MAXIMO`=180 min): `GET /manual?setor=1|2&minutos=N` no dashboard local (rede do sítio) e a célula K2/L2 na planilha (de qualquer lugar, ver "Colunas da planilha" acima). Ambos ligam o setor por N minutos independente da planilha/horário/NTP; `minutos=0` no `/manual` cancela. Expira sozinho em `verificarEAplicarRele()` via `expirarOverrideManualSeVencido()`, e tem prioridade sobre o override permanente da planilha e a tabela de horários enquanto ativo. Tem botões próprios no dashboard (`montarCardManual`).

**Dashboard web local:** `WebServer` na porta 80 + `ESPmDNS` (`http://automacao-<identificacao minúsculo>.local`). Mostra leitura atual, estado dos relés/LED, fila pendente (`filaTamanho`), controle manual temporizado por setor e os últimos eventos (ring buffer em RAM, `logRecentes[]`, não persistido).

Pinos: `RELE_SETOR1_PIN=25`, `RELE_SETOR2_PIN=33` (evite pinos input-only como GPIO34-39 se adicionar mais saídas), `LED_PIN=26`, `BOTAO_PIN=27` (INPUT_PULLUP), `DHT_PIN=4`. Tabela completa com fiação em [README.md](README.md).

**Polaridade do relé (`RELE_ATIVO_EM_LOW`):** atualmente `false` (ativo em HIGH) — já foi trocado para `true` uma vez com base num teste físico que depois se mostrou incorreto (relé ligava sozinho no boot e invertia com o comando de ligar). Se voltar a inverter, teste fisicamente antes de mudar de novo: grave, confira se o relé fica DESLIGADO no boot e LIGA quando mandado ligar (horário, override da planilha ou `/manual`).

## Coisas para não fazer de novo

- Não coloque dois `.ino` de sketches diferentes na mesma pasta (ver "Compilar" acima).
- Não faça a verificação do relé compartilhar cadência com o envio à planilha.
- SSID/senha do WiFi e a URL do Apps Script estão hardcoded no `.ino` como credenciais reais já commitadas — preserve-as ao editar, a menos que o usuário peça pra rotacionar.
- Não confie cegamente num teste físico anterior de polaridade de relé registrado em commit — já houve uma inversão (`false`→`true`→`false`) porque o resultado observado mudou; sempre valide no hardware atual antes de assumir.
