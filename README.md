# automacao_sitio

Projeto de Monitoramento Climático e Automação de Irrigação com ESP32 Doit Devkit Esp32-wroom 32 Modulo Microcontrolador em Alagoas

Frequancia: 240 MHz
Memória Flash: 4 MB
30 pinos
Alimentação 4,5V a 9V
Marca Blutu

📖 Sobre o Projeto
Este repositório contém o código-fonte e a documentação de um sistema de monitoramento climático e automação de irrigação distribuído. O projeto utiliza microcontroladores ESP32 DOIT DEVKIT V1 equipados com sensores DHT22 para coletar dados climáticos em tempo real, controlar relés de irrigação por horário e expor um dashboard web local, além de sincronizar tudo com uma planilha Google.

O principal objetivo é criar uma rede de baixo custo para a coleta de dados ambientais e automação de irrigação, que podem ser utilizados para análise meteorológica, projetos acadêmicos, automação na agricultura ou simples consulta pública.

📍 Locais de Monitoramento
As estações de coleta de dados foram estrategicamente instaladas nas seguintes cidades:

Belo Monte, AL

Arapiraca, AL

União dos Palmares, AL (planejada — ainda sem firmware próprio neste repositório)

🔥 Firmware único: `automacao_sitio.ino`

A partir desta versão, **existe um único arquivo de firmware** (`automacao_sitio.ino`) usado em todas as estações — não é mais necessário manter um `.ino` separado por estação. Os arquivos antigos `UmidTempDth22Arapiraca.ino` e `UmidTempDth22BeloMonte.ino` continuam no repositório apenas como referência histórica e não devem mais ser usados para novas gravações.

**Como funciona a identificação automática da estação:**
No boot, o firmware tenta conectar em cada rede WiFi de uma lista conhecida, na ordem abaixo. A primeira rede que conectar define automaticamente em qual estação (aba da planilha) os dados serão gravados — não é preciso configurar nada manualmente por placa.

| Ordem | SSID | Estação identificada |
|---|---|---|
| 1 | `CLARO_2GC3B8C0` | Arapiraca |
| 2 | `Sitio` | BeloMonte |

**Funcionalidades do firmware único:**
- Leitura de temperatura/umidade (DHT22) a cada 5s — alimenta o dashboard em tempo real, sem custo de rede.
- Envio periódico dos dados para a planilha Google a cada **40 min** (mesmo intervalo do projeto original, agora sem deep sleep).
- Servidor web local com dashboard de leitura (veja seção abaixo).
- Sincronização de horário via NTP.
- Controle de **dois relés de irrigação** por tabela de horários, verificado a cada 1s (independente do envio à planilha, para respeitar com precisão as janelas de 30 min), com possibilidade de override manual pela planilha.
- **Log de eventos** (relé ligado/desligado, sucesso/falha da sincronização NTP, sucesso/falha ao gravar temperatura/umidade) registrado numa aba própria ("Log") da planilha.
- **Fila local em flash (LittleFS)**: se a planilha não puder ser gravada no momento (sem WiFi, Apps Script fora do ar, etc.), o registro fica guardado no próprio ESP32 e é reenviado automaticamente assim que possível — sobrevive até a reinicializações.
- Sem deep sleep: o ESP32 fica sempre ligado e conectado, para o dashboard e os relés funcionarem a qualquer momento.

## 🌐 Dashboard web local

Cada ESP32 sobe um servidor web na porta 80, na rede WiFi em que ele conectou. Para acessar:

1. Veja o IP local no Monitor Serial logo após o boot (`WIFI: IP local: ...`), **ou**
2. Acesse pelo nome mDNS, sem precisar saber o IP:
   - Arapiraca: `http://automacao-arapiraca.local`
   - BeloMonte: `http://automacao-belomonte.local`
   - (mDNS funciona no mesmo WiFi/rede local; em alguns Windows pode exigir instalar o "Bonjour" ou usar o IP direto.)

O dashboard mostra temperatura, umidade, estado dos dois relés, estado do LED, horário sincronizado via NTP, IP local, **quantidade de registros pendentes na fila local** (não sincronizados com a planilha ainda) e os **últimos eventos** registrados (relé ligado/desligado, sincronização NTP, gravação de sensor), atualizando automaticamente a cada 15s.

## 🔌 Tabela de conexões (etiqueta de consulta rápida)

| Pino ESP32 (GPIO) | Função | Conectado a | Observação |
|---|---|---|---|
| **GPIO 4** | Dados do sensor DHT22 | Pino DATA do DHT22 | Resistor de pull-up 10kΩ entre VCC e DATA |
| **GPIO 25** | Relé Setor 1 (irrigação dentro do sítio) | Sinal/IN do módulo relé 1 | Nível ALTO (HIGH) energiza o relé |
| **GPIO 33** | Relé Setor 2 (irrigação atrás do salão) | Sinal/IN do módulo relé 2 | Nível ALTO (HIGH) energiza o relé |
| **GPIO 26** | LED indicativo de funcionamento | LED + resistor em série | Aceso = firmware rodando normalmente |
| **GPIO 27** | Botão/chave manual | Botão ligado ao GND | `INPUT_PULLUP` — pressionado = nível BAIXO |
| **3V3** | Alimentação do sensor | VCC do DHT22 | — |
| **GND** | Referência comum (terra) | GND do DHT22, dos dois relés e do botão | — |

> Dica: essa tabela pode ser copiada e impressa como etiqueta para colar dentro da caixa de cada estação, junto ao ESP32.

## ⏱️ Tabela de horários de irrigação

Horários configurados em `horariosSetor1[]` e `horariosSetor2[]`, dentro de `automacao_sitio.ino`. Para alterar, edite esses arrays e regrave o firmware.

**Setor 1 — irrigação dentro do sítio (GPIO 25):**

| Liga | Desliga |
|---|---|
| 19:00 | 19:30 |
| 20:00 | 20:30 |
| 21:00 | 21:30 |
| 22:00 | 22:30 |
| 23:00 | 23:30 |

**Setor 2 — irrigação atrás do salão (GPIO 33):**

| Liga | Desliga |
|---|---|
| 00:10 | 00:30 |
| 01:00 | 01:30 |
| 02:00 | 02:30 |
| 03:00 | 03:30 |
| 04:00 | 04:30 |

O horário é sincronizado por NTP (fuso de Brasília, UTC-3, sem horário de verão). Enquanto o horário não sincroniza no boot, os relés mantêm o último estado conhecido em vez de agir sobre hora incorreta.

## 📊 Colunas usadas na planilha Google (por aba/estação)

| Coluna | Conteúdo | Preenchida por |
|---|---|---|
| A | Data completa (timestamp) | ESP32, a cada envio |
| B | Data | ESP32, a cada envio |
| C | Hora | ESP32, a cada envio |
| D | Umidade (%) | ESP32, a cada envio |
| E | Temperatura (°C) | ESP32, a cada envio |
| F | Estado do botão físico | ESP32, a cada envio |
| G2 | Override manual do Relé Setor 1 (`1`=força ligado, `0`=força desligado, vazio=segue tabela de horários) | Você, na planilha |
| H2 | Override manual do LED (`1`/`0`) | Você, na planilha |
| I2 | Comando de reset remoto (`1`=reinicia o ESP32; o firmware limpa de volta para `0` automaticamente) | Você, na planilha |
| J2 | Override manual do Relé Setor 2 (`1`/`0`/vazio=segue tabela de horários) | Você, na planilha |

O cabeçalho (linha 1) é escrito automaticamente pelo firmware no primeiro boot, caso ainda não esteja configurado na aba. Se a aba de uma estação (ou a aba "Log") ainda não existir na planilha, o Apps Script cria automaticamente na primeira gravação — não precisa criar manualmente.

## 📝 Log de eventos (aba "Log")

Toda vez que algo relevante acontece, o firmware grava uma linha na aba **"Log"** da planilha (criada automaticamente na primeira vez):

| Coluna | Conteúdo |
|---|---|
| A | Data completa (timestamp) |
| B | Data |
| C | Hora |
| D | Estação (Arapiraca / BeloMonte) |
| E | Categoria (`Sensor`, `Rele`, `NTP`) |
| F | Evento (ex.: `leitura_temp_umidade`, `Setor1`, `Setor2`, `sincronizacao_hora`) |
| G | Resultado (`SUCESSO`, `FALHA`, `FALHA_ENFILEIRADO`, `LIGADO`, `DESLIGADO`) |
| H | Detalhe (texto livre, ex. motivo da falha) |

O que é registrado:
- **Sensor** — a cada envio periódico (40 min): se a leitura de temperatura/umidade foi gravada com sucesso, ficou pendente na fila local (`FALHA_ENFILEIRADO`), ou falhou porque o sensor não retornou leitura válida (`FALHA`).
- **Rele** — toda vez que o Setor 1 ou o Setor 2 liga ou desliga (por horário ou por override manual).
- **NTP** — uma vez no boot, se a hora foi sincronizada com sucesso ou não.

## 📦 Fila local (funciona mesmo sem planilha)

Se uma gravação na planilha falhar (sem WiFi, Google Apps Script fora do ar, etc.), o firmware guarda o registro pendente em um arquivo na memória flash do próprio ESP32 (LittleFS, `/fila.jsonl`) em vez de descartar o dado. A cada 2 minutos ele tenta reenviar os registros pendentes, na ordem em que foram gerados; a fila sobrevive a reinicializações do ESP32. O limite é de 150 registros pendentes — se ultrapassar (rede fora do ar por muito tempo), os mais antigos são descartados para abrir espaço.

O tamanho atual da fila aparece em tempo real no dashboard web local (seção anterior).

> **Atenção:** como o Apps Script (`ScriptPlanilha_GOOGLEDrive`) foi alterado para criar a aba "Log" automaticamente, é preciso **reimplantar** o Web App no Google Apps Script para a mudança valer (Implantar → Gerenciar implantações → editar → Nova versão → Implantar). Só editar o código lá dentro não é suficiente.

🛠️ Hardware Utilizado
Cada unidade é composta pelos seguintes componentes:

Componente: ESP32 DOIT DEVKIT V1 (SKU 196)

Sensor de Temperatura e Umidade DHT22: Dht22 Am2302 Arduino Rasp Node SKU: SS21

Resistor de 10kΩ: Resistor de pull-up para garantir a estabilidade na comunicação com o DHT22.

Dois módulos de relé (Setor 1 e Setor 2), para controle da irrigação.

Protoboard e Jumpers

Fonte de Alimentação 3.3V / Fonte de alimentação 5V/3.3V

🔧 Software e Dependências
O firmware do ESP32 foi desenvolvido utilizando a IDE do Arduino com o suporte ao Arduino Core for the ESP32.

Bibliotecas Necessárias
Para compilar o código, é preciso instalar as seguintes bibliotecas através do Gerenciador de Bibliotecas da IDE do Arduino:

DHT sensor library by Adafruit

Adafruit Unified Sensor by Adafruit

ArduinoJson

WiFi.h, HTTPClient.h, WebServer.h, ESPmDNS.h (já inclusas no Core do ESP32, não precisa instalar)

🚀 Como Utilizar
Siga os passos abaixo para configurar e rodar o projeto em um novo ESP32.

Clonar o Repositório:

```
git clone https://github.com/jrmaceio/automacao_sitio.git
```

Configurar o Ambiente:

Abra `automacao_sitio.ino` na IDE do Arduino.

Instale as bibliotecas listadas na seção anterior.

Personalizar o Código (se for uma rede WiFi nova):

Adicione a nova rede em `redesConhecidas[]`, com SSID, senha e o nome (`identificacao`) que ela deve representar na planilha.

Se for adicionar uma nova estação com planilha/script diferente, ajuste também `googleScriptURL`.

Compilar e Fazer Upload:

Conecte o seu ESP32 DOIT DEVKIT V1 ao computador.

Na IDE, vá em Ferramentas > Placa e selecione "DOIT ESP32 DEVKIT V1".

Selecione a porta COM correta em Ferramentas > Porta.

Clique no botão de Upload.

Verificar o Funcionamento:

Após o upload, abra o Monitor Serial (Ctrl+Shift+M) com a taxa de 115200 bauds para visualizar as leituras do sensor, o status da conexão WiFi, o IP local e o status da sincronização NTP.

Acesse o dashboard pelo IP ou pelo endereço mDNS (`http://automacao-<estação>.local`), como explicado na seção "Dashboard web local" acima.

📈 Status e Próximos Passos
O projeto encontra-se em fase de implementação. As estações de Arapiraca e Belo Monte estão operacionais.

Melhorias Futuras
[x] Desenvolver um dashboard web para visualização dos dados em tempo real.

[x] Unificar o firmware das estações em um único `.ino`.

[x] Automatizar a irrigação por tabela de horários.

[ ] Adicionar firmware/estação para União dos Palmares.

[ ] Implementar armazenamento dos dados em um banco de dados na nuvem (ex: Firebase, InfluxDB).

[ ] Criar um sistema de alertas por e-mail ou Telegram para variações climáticas extremas.

[ ] Projetar e imprimir um case em 3D para proteger os componentes em ambiente externo.

🤝 Contribuições
Contribuições são muito bem-vindas! Se você tem alguma ideia para melhorar este projeto, sinta-se à vontade para abrir uma issue para discussão ou enviar um pull request com suas modificações.

📄 Licença
Este projeto está sob a licença MIT. Veja o arquivo LICENSE para mais detalhes.
