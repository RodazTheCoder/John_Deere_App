# Sistema de Detecção e Prevenção de Colisão — Challenge John Deere

Sistema de segurança para tratores florestais que detecta pessoas e outros tratores próximos à máquina, combinando duas camadas de sensoriamento independentes:

1. **LoRa + GPS + ESP32** — cada trator e cada pessoa no terreno carrega um nó com GPS + rádio LoRa, transmitindo posição continuamente. Funciona em qualquer distância/visibilidade (atrás de árvore, à noite), mas depende de todo mundo estar usando o dispositivo.
2. **Câmera + Raspberry Pi + visão computacional** — uma câmera na traseira do trator detecta pessoas/veículos visíveis, com ou sem dispositivo LoRa.

O cruzamento dessas duas fontes gera um alerta de 3 níveis (semáforo verde/amarelo/vermelho), mostrado num dashboard web (tablet na cabine) **e** refletido em LED/buzzer ligados direto no ESP32 — essa segunda via é redundante de propósito: continua funcionando mesmo se o Raspberry Pi travar ou perder rede.

---

## Quick start

### Centralizador de dados em 5 minutos

1. Crie o ambiente virtual e instale as dependências do dashboard:

```powershell
cd "C:\Users\tonii\OneDrive\Área de Trabalho\John_Deere_App"
py -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install Flask pyserial
```

2. Grave o ESP32 em modo centralizador editando o firmware no topo do arquivo:

```cpp
#define TRATOR_COM_PI 0
#define MODULO_CENTRALIZADOR 1
```

3. Compile e envie o firmware para o ESP:

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32 esp32_firmware/ProjetoLoRa1
arduino-cli upload -p COM3 --fqbn esp32:esp32:esp32 esp32_firmware/ProjetoLoRa1
```

4. Inicie o dashboard centralizador:

```powershell
cd centralizador_de_dados
python app.py
```

5. Acesse:

```text
http://localhost:5001
```

Se a porta não for a COM correta, use:

```powershell
$env:CENTRALIZADOR_PORT = "COM3"
python app.py
```

> O app já ignora mensagens de diagnóstico do GPS e do firmware, e automaticamente fecha a serial ao encerrar para evitar bloqueio da porta.

---

## Arquitetura

```
Trator (com câmera + Pi)                       Pessoa / outro trator no mato
┌───────────────────────────┐                  ┌──────────────────────┐
│ Raspberry Pi 4B             │                  │ ESP32 + GPS + LoRa    │
│  ├─ Câmera traseira         │                  │ (TRATOR_COM_PI = 0)   │
│  ├─ YOLOv8n (NCNN)          │                  │ transmite id/tipo/    │
│  ├─ Flask (dashboard)       │                  │ posição por LoRa      │
│  └─ POST /api/entidade  ◄───┼── WiFi ── ESP32 do trator ───┘  LoRa
│      (recebe do ESP32)      │  (TRATOR_COM_PI = 1: ouve LoRa de
└──────────────┬──────────────┘   todo mundo, repassa pro Pi por WiFi)
               │ WiFi
┌──────────────▼─────────────┐
│ Tablet na cabine             │
│ (abre o dashboard no         │
│  navegador)                  │
└───────────────────────────┘
```

Qualquer ESP32+GPS+LoRa "solto" (sem Raspberry próprio, ex: tag de pessoa) aparece no único dashboard que existe porque o ESP32 do trator ouve tudo que está no alcance do LoRa e repassa pro Pi.

## A tabela de verdade

A câmera **não mede distância** — só confirma "tem alguém na área". Quem calcula distância é sempre o LoRa/GPS. O casamento entre os dois é por **presença/ausência**, não por correlação fina de ângulo (exigiria bússola + GPS precisos, pouco confiáveis sob dossel florestal).

**Regra de ouro:** câmera detectando alguém tem prioridade sobre a distância calculada pela entidade LoRa — em mata fechada a câmera só enxerga poucos metros, então "detectando" já significa perto de verdade, mesmo que o LoRa diga o contrário (pode até ser alguém não rastreado que entrou na área).

| Câmera detecta? | Distância (LoRa) | Nível |
|---|---|---|
| Sim | qualquer valor | **Crítico confirmado** — vermelho sólido |
| Sim | nenhuma entidade registrada | **Não identificado** — vermelho + alarme urgente |
| Não | > 100 m | Seguro — verde |
| Não | 50–100 m | Atenção — amarelo |
| Não | < 50 m | Crítico pendente — vermelho piscando |

Limiares (100m/50m) são configuráveis em tempo real (`POST /api/escala`), mantendo a proporção 1:2 — útil pra testar em ambientes pequenos sem precisar de 100m reais.

### Limitações conhecidas (aceitas, não são bugs)

- Só há câmera na traseira — pra frente e os lados, a única proteção é o LoRa.
- Distância por rádio (RSSI) é pouco confiável em ambientes fechados a curta distância (multipath domina o sinal); o sistema foi pensado pra distâncias reais de campo aberto.
- Sistema rastreia só a entidade LoRa mais próxima por vez — sem multi-rastreamento.
- Câmera e LoRa não se correlacionam por ângulo, só por presença/ausência.

## Decisões de arquitetura (não reabrir sem motivo novo)

- **1 câmera, traseira, fixa (sem pan-tilt)** — único ponto cego real do operador; mais câmeras excederia o processamento do Pi 4B e seria mais uma abertura física frágil.
- **LED + buzzer ligados direto no ESP32** — camada redundante que nunca depende de WiFi/Pi/tablet.
- **YOLOv8n + NCNN via `ultralytics`** — venceu MediaPipe (incompatível com o Pi 4B), ONNXRuntime (lento) e TFLite (menos preciso).
- **Um único firmware serve todo tipo de nó** — o que muda é só `TIPO_ENTIDADE` e `TRATOR_COM_PI` no topo do `.ino`.
- **ID de cada ESP32 é gerado a partir do MAC de fábrica**, não digitado manualmente.
- **O Pi cria a rede WiFi (Access Point)**, não o ESP32 — ver `docs/configurar_pi_como_ap.md`.

---

## Estrutura do repositório

```
raspberry_pi_app/
├── app.py                    Flask: rotas do dashboard e da API
├── config.py                 Limiares, porta, câmera — único lugar a editar entre ambientes
├── estado_compartilhado.py   Estado global thread-safe (câmera + entidades LoRa)
├── logica_alerta.py          A tabela de verdade em código
├── comunicacao/
│   └── entidade_receiver.py  Valida o payload que o ESP32 manda por WiFi
├── deteccao/
│   └── camera_worker.py      Thread de câmera: captura + YOLO/NCNN
├── templates/dashboard.html  Painel do operador
└── tests/                    29 testes automatizados, sem depender de hardware

esp32_firmware/ProjetoLoRa1/  Firmware único (GPS + LoRa + WiFi opcional)
yolov8n_ncnn_model/           Modelo exportado (YOLOv8n → NCNN, imgsz=320 fixo)
docs/configurar_pi_como_ap.md Guia de configuração do hotspot do Pi
```

**Endpoints principais do Flask:**
- `GET /` — dashboard
- `GET /snapshot.jpg` — frame JPEG (usado pelo dashboard; compatível com Safari/iOS, diferente do `/video_feed` em MJPEG)
- `GET /api/status` — JSON com estado completo (câmera, entidades, alerta calculado)
- `GET /api/alerta-fisico` — versão texto simples do alerta, consumida pelo ESP32 pro LED/buzzer físico
- `POST /api/entidade` — recebido do ESP32 do trator
- `POST /api/escala` — reconfigura os limiares em tempo real
- `POST /api/modo-demo` / `POST /api/silenciar` — controles do dashboard

## Firmware ESP32 (`esp32_firmware/ProjetoLoRa1/`)

O mesmo código roda em todo ESP32 do projeto — o que muda por unidade é só:

```cpp
#define TIPO_ENTIDADE "pessoa"   // "pessoa" ou "trator"
#define TRATOR_COM_PI 0          // 1 SÓ no ESP32 junto do Raspberry Pi
```

Cada nó lê sua posição por GPS, transmite por LoRa periodicamente, e calcula distância/direção de quem ouve. O ESP32 do trator (`TRATOR_COM_PI=1`) também repassa tudo que ouve pro Pi via WiFi, e consulta o alerta já cruzado pra acionar o LED/buzuer físico — com fallback local (só distância) se o Pi ficar inacessível por mais de 5s.

**Valores que exigem calibração de campo real** (não indoor a curta distância — RSSI não é confiável nessa faixa): `LORA_RSSI_1M` e `EXPOENTE_PERDA_AMBIENTE`.

---

## Setup local (Mac, Linux ou Windows)

Python puro + Flask — roda em qualquer sistema com Python 3.9+ e uma webcam.

```bash
git clone https://github.com/RodazTheCoder/John_Deere_App.git
cd John_Deere_App
python3 -m venv venv          # Windows: py -m venv venv
source venv/bin/activate      # Windows PowerShell: venv\Scripts\Activate.ps1
pip install torch torchvision --index-url https://download.pytorch.org/whl/cpu
pip install -r raspberry_pi_app/requirements.txt
cd raspberry_pi_app
python app.py
```

Abre em `http://127.0.0.1:5050/` (porta 5050, não 5000 — no macOS a 5000 é ocupada pelo AirPlay Receiver). O modelo NCNN já vem no repositório, não precisa exportar de novo.

**Rodar os testes** (sem precisar de câmera ou hardware):
```bash
cd raspberry_pi_app
python -m pytest tests/ -v
```

**Se a webcam não for a esperada:** ajustar `CAMERA_SOURCE` em `raspberry_pi_app/config.py` (`0`, `1`, `2`...).

**Gravar o firmware num ESP32** (via `arduino-cli`):
```bash
arduino-cli compile --fqbn esp32:esp32:esp32 esp32_firmware/ProjetoLoRa1
arduino-cli upload -p <porta> --fqbn esp32:esp32:esp32 esp32_firmware/ProjetoLoRa1
```

---

## Atualização: centralizador de dados via serial

A arquitetura do projeto foi estendida para incluir um dashboard centralizado em Python, alimentado por um ESP32 dedicado em modo centralizador. Nesse fluxo, os módulos LoRa espalhados no campo enviam os dados para o ESP centralizador, que apenas recebe e repassa a informação pela porta serial USB para a aplicação Flask.

### Fluxo atual

```text
Módulos LoRa no campo
        ↓
ESP32 centralizador
        ↓
Serial USB (COMx / /dev/ttyUSBx)
        ↓
centralizador_de_dados/app.py
        ↓
Dashboard em Flask com mapa e histórico de posições
```

### O que foi implementado

- Novo app em `centralizador_de_dados/app.py` para receber dados serializados de um ESP32 centralizador.
- Descoberta automática da porta serial com fallback para `COM3` / `/dev/ttyUSB0`.
- Suporte a payload em JSON ou CSV, mantendo o mesmo formato do firmware do ESP32.
- Filtro de linhas de diagnóstico do GPS e mensagens internas do firmware para não poluir o payload real.
- Fechamento correto da serial em `atexit` e ao encerrar a aplicação.
- Desativação do reloader do Flask para evitar duas threads simultâneas lendo a mesma COM.
- Dashboard com mapa, lista de veículos e histórico de localização por veículo.

### Estrutura da nova parte do projeto

```text
centralizador_de_dados/
├── app.py
├── templates/
│   └── dashboard.html
├── tests/
│   └── test_serial_parser.py
└── __init__.py
```

---

## Como rodar o app do centralizador

### 1) Preparar o ambiente Python

No Windows PowerShell:

```powershell
cd "C:\Users\tonii\OneDrive\Área de Trabalho\John_Deere_App"
py -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install Flask pyserial
```

No Linux/macOS:

```bash
cd /caminho/para/John_Deere_App
python3 -m venv .venv
source .venv/bin/activate
pip install Flask pyserial
```

### 2) Executar o dashboard

```bash
cd centralizador_de_dados
python app.py
```

A aplicação abre em:

```text
http://localhost:5001
```

### 3) Configurar a porta serial (opcional)

Se a porta do ESP centralizador não for a padrão, defina manualmente a variável de ambiente:

PowerShell:

```powershell
$env:CENTRALIZADOR_PORT = "COM3"
python app.py
```

Linux/macOS:

```bash
export CENTRALIZADOR_PORT=/dev/ttyUSB0
python app.py
```

Também é possível ajustar a baud rate:

```powershell
$env:CENTRALIZADOR_BAUD = "9600"
```

### 4) Verificar se a aplicação está funcionando

Abra no navegador:

```text
http://localhost:5001
```

E teste a API de saúde:

```text
http://localhost:5001/api/health
```

Resposta esperada:

```json
{"ok": true, "veiculos_ativos": 0, "porta_serial": "COM3"}
```

---

## Como configurar o ESP centralizador

No firmware principal em `esp32_firmware/ProjetoLoRa1/ProjetoLoRa1.ino`, as configurações principais ficam no topo do arquivo:

```cpp
#define TIPO_ENTIDADE "trator"
#define TRATOR_COM_PI 0
#define MODULO_CENTRALIZADOR 1
```

### Regras de configuração

- `MODULO_CENTRALIZADOR = 1`
  - ativa o modo de recepção exclusiva do LoRa
  - o módulo não transmite dados próprios por rádio
  - ele apenas recebe todos os pacotes e envia pela serial para a dashboard
- `TRATOR_COM_PI = 0`
  - o ESP centralizador não usa WiFi nem Raspberry Pi
  - ele fica isolado como gateway serial para o dashboard
- `TIPO_ENTIDADE` pode ser `"trator"` ou `"pessoa"`, conforme o papel do nó

### Exemplo de comportamento esperado no firmware

Quando `MODULO_CENTRALIZADOR` for `1`, o firmware deve:

- iniciar a serial do ESP
- inicializar o LoRa em modo de recepção
- escutar pacotes recebidos
- filtrar mensagens internas de debug e GPS
- enviar apenas o payload útil via `Serial.println(...)`

### Compilar e gravar o firmware

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 esp32_firmware/ProjetoLoRa1
arduino-cli upload -p COM3 --fqbn esp32:esp32:esp32 esp32_firmware/ProjetoLoRa1
```

Atenção:

- se a porta estiver errada, o upload ou a leitura da serial falha
- se a IDE serial do Arduino estiver aberta, a mesma COM pode ficar bloqueada
- certifique-se de que a conexão USB do ESP centralizador seja a correta

---

## Formato do payload esperado

O parser no app aceita dois formatos:

### 1) JSON

```json
{"id":"tractor-01","tipo":"trator","latitude":-23.5505,"longitude":-46.6333,"velocidade_kmh":12.5,"rssi_dbm":-60}
```

### 2) CSV

```text
tractor-01,trator,-23.5505,-46.6333,10.5,12.5,90,-60
```

Estrutura da linha CSV:

```text
id,tipo,latitude,longitude,altitude,velocidade,curso,rssi
```

Se o firmware estiver enviando linhas extras como `[GPS]` ou `AVISO:` no mesmo stream, o app passa a descartar essas linhas como ruído.

---

## Erros comuns e como resolver

### 1) `PermissionError(13, 'Acesso negado.')`

Causa mais comum:

- outra aplicação já está usando a porta serial
- Arduino IDE com Serial Monitor aberto
- outra instância do dashboard rodando

Solução:

```powershell
mode COM3
```

Se a porta estiver em uso, feche o monitor serial e reinicie o app.

Também pode ser útil verificar se o Python está rodando duas vezes:

```powershell
Get-Process python -ErrorAction SilentlyContinue
```

### 2) `Linha serial descartada ...`

Isso acontece quando a linha recebida não bate com o formato esperado ou quando o firmware envia texto de diagnóstico junto com o payload real.

Soluções:

- verificar se o firmware está em modo centralizador correto
- garantir que as mensagens `[GPS]` e `AVISO` não sejam enviadas pela serial de telemetria
- confirmar se o payload continua sendo `id,tipo,latitude,longitude,...` e não com texto extra
- validar com um monitor serial simples antes de abrir o dashboard

### 3) Nenhum ponto aparece no mapa

Possíveis causas:

- ESP centralizador não está recebendo pacotes LoRa
- `MODULO_CENTRALIZADOR` não foi ativado
- a porta serial escolhida está errada
- o payload foi descartado por estar em formato inválido
- o Flask está rodando com reloader ativado e duplicando threads

Validação rápida:

```powershell
cd centralizador_de_dados
python -c "import app; print(app.SERIAL_PORT); print(app.descobrir_porta_serial())"
```

Se o retorno não mostrar a COM correta, ajuste a variável `CENTRALIZADOR_PORT` antes de iniciar.

### 4) O programa abre a porta mais de uma vez

Causa:

- o Flask entrou em debug mode com `use_reloader=True`
- o app foi iniciado mais de uma vez sem encerrar corretamente a serial

Solução já aplicada no código:

```python
app.run(host="0.0.0.0", port=5001, debug=False, use_reloader=False)
```

### 5) `[GPS]` e mensagens de diagnóstico aparecem no dashboard

Causa:

- o firmware estava mandando logs de GPS e avisos para a mesma serial que transporta os dados do LoRa

Solução:

- manter `MODULO_CENTRALIZADOR` em `1`
- encapsular mensagens de diagnóstico com `#if !MODULO_CENTRALIZADOR`
- deixar apenas o payload útil no stream serial

Exemplo:

```cpp
#if !MODULO_CENTRALIZADOR
  Serial.println("[GPS] ...");
#endif
```

### 6) Porta serial fica travada no Windows

Se a COM continuar bloqueada mesmo após encerrar o app, o problema está em outra aplicação usando a porta. O fluxo recomendado é:

1. fechar o Serial Monitor do Arduino
2. fechar qualquer outra janela de debug serial
3. encerrar qualquer processo Python em execução
4. reiniciar o dashboard

---

## Checklist de validação final

Antes de considerar o sistema funcionando:

- [ ] o ESP centralizador está gravado com `MODULO_CENTRALIZADOR 1`
- [ ] a porta serial correta está sendo aberta pela aplicação
- [ ] o app Flask foi iniciado com `debug=False` e `use_reloader=False`
- [ ] o payload recebido está em CSV/JSON compatível com o parser
- [ ] não há linhas `[GPS]` ou `AVISO:` sendo enviadas pela serial de dados
- [ ] o dashboard mostra os veículos e os pontos no mapa

Com esse setup, a centralização de dados fica estável e a dashboard passa a receber os veículos em tempo real sem poluir o stream com mensagens internas do firmware.

---
