# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Visão geral do projeto

Firmware para uma rede de monitoramento climático distribuído: várias placas ESP32 DOIT DEVKIT V1, cada uma conectada a um sensor de temperatura/umidade DHT22 (ou DHT11, no sketch genérico/mais antigo), que enviam leituras para uma planilha Google compartilhada via um Google Apps Script Web App funcionando como backend REST simples. Não há build system, gerenciador de pacotes ou suíte de testes — é um conjunto de sketches Arduino `.ino` independentes, feitos para serem abertos e gravados individualmente pela Arduino IDE.

## Estrutura do repositório

Cada arquivo `.ino` é uma **imagem de firmware completa e independente para uma estação física** — eles não se incluem/importam entre si, são cópias derivadas umas das outras:

- `UmidTempDth22Arapiraca.ino` — estação de Arapiraca, AL (DHT22, loop com deep sleep).
- `UmidTempDth22BeloMonte.ino` — estação de Belo Monte, AL (DHT22, loop com deep sleep). Estruturalmente idêntico ao sketch de Arapiraca, exceto pelas credenciais WiFi, a `googleScriptURL` e a string `identificacao` ("BeloMonte") passada nas chamadas de comunicação com a planilha.
- `umidadetemperatura.ino` — versão genérica/mais antiga do mesmo firmware, usando DHT11 e um loop baseado em `delay()` em vez de deep sleep.
- `googleplanilhas_Modelo.ino` — arquivo de referência/modelo contendo só as funções auxiliares de HTTP para a planilha Google (`escreverEmLista`, `escreverEmCelula`, `lerCelula`, `lerLinha`, `montarCabecalho`), usado como template ao configurar o sketch de uma nova estação. Não compila sozinho (sem `setup()`/`loop()`).
- `ScriptPlanilha_GOOGLEDrive` — o Google Apps Script (JavaScript, handler `doPost`) que precisa ser implantado como Web App do lado da planilha; é para onde a `googleScriptURL` de cada sketch aponta. Inclui o passo a passo de implantação.
- `PromptGrafico.md` — um template de prompt (em português) para gerar um dashboard climático comparativo (gráficos de linha + box plots) a partir dos dados coletados, usando uma ferramenta de BI/IA externa. Não é código.
- O README menciona uma terceira estação, "União dos Palmares", mas ainda não existe um `.ino` correspondente no repositório.

Não há header/biblioteca compartilhada entre os sketches das estações — a lógica comum (conexão WiFi, POST HTTP para o Apps Script, bootstrap do cabeçalho) está duplicada em cada arquivo. Ao corrigir um bug ou mudar o protocolo de comunicação, verifique se o mesmo código existe nos outros sketches de estação e também precisa da correção.

## Arquitetura: protocolo firmware ↔ planilha

Toda a comunicação é um único `HTTPClient::POST` de um corpo JSON para `googleScriptURL`, com um campo `action` selecionando o comportamento no handler `doPost` do Apps Script:

- `escreverEmLista` — adiciona uma linha `[timestamp, data, hora, ...dados]` na aba da planilha nomeada por `identificacao` (uma aba por estação, ex.: "Arapiraca", "BeloMonte"). `dados` é `[umidade, temperatura, statusBotao]`.
- `escreverEmCelula` — escreve um valor único em uma célula específica (`celula`, ex.: `"G2"`).
- `lerCelula` — lê e retorna o valor de uma célula.
- `lerLinha` — lê uma linha inteira (usada pelo helper `lerLinha` em `googleplanilhas_Modelo.ino`; não é chamada atualmente pelos sketches de estação).

A cada iteração do loop, os sketches de estação DHT22 também **consultam a planilha de volta** para controle remoto: leem a célula `G2` (estado do relé/`RELE_PIN`), `H2` (estado do LED/`LED_PIN`) e `I2` (flag de reset remoto — escrever "1" em `I2` na planilha dispara `ESP.restart()`, e o firmware limpa a célula de volta para "0" antes disso, para evitar boot loop). `montarCabecalho()` verifica a célula `A1` contra o cabeçalho esperado no boot e (re)escreve a linha de cabeçalho a partir da coluna A caso não confira, usando o nome da aba (`identificacao`) como ID da placa.

Layout de pinos usado de forma consistente nos sketches DHT22: `RELE_PIN=25`, `LED_PIN=26`, `BOTAO_PIN=27` (INPUT_PULLUP, ativo em nível baixo), `DHT_PIN=4`.

Os dois sketches de estação DHT22 dormem via `esp_deep_sleep_start()` entre ciclos (`TIME_TO_SLEEP=2400`s / 40 min) em vez de fazer loop com `delay()`; já o `umidadetemperatura.ino` (DHT11), mais antigo, faz loop infinito com `delay(INTERVALO_LOOP)` (30 min) e sem deep sleep.

## Trabalhando com estes arquivos

- Não há nada para build/lint/test com ferramental padrão — a verificação é "compila e sobe pela Arduino IDE" (Placa: "DOIT ESP32 DEVKIT V1") e depois ler a saída do Monitor Serial a 115200 baud.
- Bibliotecas Arduino necessárias: `DHT sensor library` (Adafruit), `Adafruit Unified Sensor`, `ArduinoJson`, além de `WiFi.h`/`HTTPClient.h` já inclusas no Core do ESP32.
- SSID/senha do WiFi e a URL de implantação do Google Apps Script estão hardcoded em cada arquivo como literais `const char*`/`String` perto do topo do sketch — são credenciais reais já commitadas no repositório, não placeholders. Ao editar o sketch de uma estação, preserve suas credenciais/URL existentes a menos que o usuário peça explicitamente para rotacioná-las.
- Se for pedido para adicionar uma nova estação (ex.: a de "União dos Palmares", citada no README mas ainda ausente aqui), copie `UmidTempDth22Arapiraca.ino` ou `UmidTempDth22BeloMonte.ino` como template (não `googleplanilhas_Modelo.ino`), e atualize: `ssid`/`password`, `googleScriptURL` (se usar uma implantação/planilha diferente do Apps Script), e toda string `identificacao` passada para `escreverEmLista`/`lerCelula`/`escreverEmCelula`/`montarCabecalho`.
- Se mudar o protocolo JSON (nomes de campos, novos valores de `action`), atualize também `ScriptPlanilha_GOOGLEDrive` (o switch do `doPost`) — firmware e Apps Script precisam concordar no formato do payload, já que não há schema/tipagem compartilhada entre eles.
