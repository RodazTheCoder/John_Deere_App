# Sistema de Detecção e Prevenção de Colisão — Challenge John Deere

Sistema de segurança para tratores florestais que detecta pessoas e outros tratores próximos à máquina, combinando duas camadas de sensoriamento independentes:

1. **LoRa + GPS + ESP32** — cada trator e cada pessoa no terreno carrega um nó com GPS + rádio LoRa, transmitindo posição continuamente. Funciona em qualquer distância/visibilidade (atrás de árvore, à noite), mas depende de todo mundo estar usando o dispositivo.
2. **Câmera + Raspberry Pi + IA (visão computacional)** — uma câmera na traseira do trator detecta pessoas/veículos visíveis, com ou sem dispositivo LoRa. É o que cobre o caso de alguém sem proteção nenhuma.

O cruzamento dessas duas fontes gera os níveis de alerta (semáforo verde/amarelo/vermelho) mostrados num painel dentro da cabine.

> **Este README é o ponto de partida pra quem entrar no projeto agora.** Ele documenta o que já está pronto, o que falta, e por que várias decisões foram tomadas do jeito que foram — várias alternativas já foram tentadas e descartadas, não vale a pena repetir os becos sem saída.

---

## Estado atual (resumo rápido)

| Parte | Status |
|---|---|
| Detecção de pessoa por câmera (YOLOv8n + NCNN) | ✅ Funcionando, testado |
| Servidor web (Flask) servindo vídeo + dados | ✅ Funcionando, testado |
| Dashboard do operador (painel visual) | ✅ Funcionando com dados reais da câmera |
| Lógica da tabela de verdade (decide o nível de alerta) | ✅ Escrita e testada (21 testes automatizados no total) |
| Botões de demo no dashboard (plano B pra apresentação) | ✅ Mandam entidade falsa pro backend real, sem precisar do ESP32 |
| Firmware ESP32 (GPS + LoRa, com ID autônomo e tipo) | ✅ Compilado e gravado numa placa real — ID e LoRa confirmados, GPS sem fix ainda (precisa de céu mais aberto) |
| Comunicação ESP32 → Pi (WiFi/HTTP) | ✅ Implementada dos dois lados (`POST /api/entidade`), testada com requisições simuladas — falta o ESP32 de verdade mandando |
| LoRa entre duas placas de verdade | ❌ Só testado com 1 placa até agora, falta uma segunda pra validar troca de pacote |
| Raspberry Pi como ponto de acesso WiFi | ⚠️ Funciona, mas só aceita 1 cliente por vez ("Association refused... max allowed 1") — precisa investigar |
| LED físico + buzzer (camada redundante, ligada direto no ESP32) | ✅ Código pronto e gravado na placa (pinos 32/33/27/26 são placeholder — trocar pelos reais e testar fiação) |
| Testado no Raspberry Pi de verdade | ✅ Rodando e calibrado (câmera + YOLO + dashboard) |
| Testes de campo (alcance real LoRa/câmera na floresta) | ❌ Não feito |
| Calibração dos parâmetros de distância por RSSI | ❌ Valores são estimativa, marcados com `TODO CALIBRAR` no firmware |

---

## Arquitetura

```
Trator (com câmera + Pi)                       Pessoa / outro trator no mato
┌───────────────────────────┐                  ┌──────────────────────┐
│ Raspberry Pi 4B            │                  │ ESP32 + GPS + LoRa    │
│  ├─ Câmera traseira        │                  │ (TRATOR_COM_PI = 0)   │
│  ├─ YOLOv8n (NCNN)         │                  │ transmite id/tipo/    │
│  ├─ Flask (dashboard)      │                  │ posição por LoRa      │
│  │    ↑ cria o WiFi (AP)   │                  └──────────┬────────────┘
│  └─ POST /api/entidade  ◄──┼── WiFi ── ESP32 do trator ───┘  LoRa
│      (recebe do ESP32)     │  (TRATOR_COM_PI = 1: ouve LoRa de
└──────────────┬─────────────┘   todo mundo, repassa pro Pi por WiFi)
               │ WiFi
┌──────────────▼─────────────┐
│ Tablet na cabine             │
│ (abre o dashboard no         │
│  navegador)                   │
└───────────────────────────┘
```

Qualquer ESP32+GPS+LoRa "solto" (sem Raspberry próprio, ex: tag de pessoa) aparece no único dashboard que existe porque o ESP32 do trator ouve tudo que está no alcance do LoRa e repassa pro Pi — não é preciso ter um Pi por entidade.

- **1 câmera só, na traseira** — decisão deliberada. É o único ponto cego real do operador. Múltiplas câmeras foram descartadas: o Pi 4B já processa 1 stream no limite, e cada câmera é mais uma abertura na cabine pra proteger contra impacto de galhos.
- **Sem pan-tilt** — partes mecânicas móveis são o que mais quebra num ambiente de vibração/impacto constante. Câmera fixa grande-angular.
- **Painel no tablet, não OLED pequeno** — um OLED pequeno não mostra vídeo, e mostrar vídeo é requisito. LED físico + buzzer continuam existindo em paralelo, ligados direto no ESP32, como camada crítica que nunca depende de Wi-Fi/tablet.

---

## A tabela de verdade (lógica central do sistema)

A câmera **não mede distância** — só confirma "tem algo nessa área". Quem calcula distância é sempre o LoRa/GPS. O casamento entre os dois é por **presença/ausência**, não por correlação fina de ângulo (isso foi cogitado e descartado — exigiria bússola do trator + GPS preciso, pouco confiáveis sob dossel florestal).

Alcance realista assumido pra câmera na floresta: **< 50m** (estimativa conservadora, ainda não validada em campo).

| Distância (LoRa) | Câmera detecta? | LED | Buzzer | Tablet |
|---|---|---|---|---|
| > 100 m | Fora de alcance (irrelevante) | Verde sólido | Silêncio | Radar normal |
| 50–100 m | Fora do alcance da câmera | Amarelo sólido | Bipe espaçado (~5s) | "Alerta a X m — sem confirmação" |
| < 50 m | Ainda não detectou | 🔴 Vermelho **piscando** | Bipe contínuo | "Risco crítico — aguardando confirmação visual" |
| < 50 m | Detectou e confirmou | Vermelho sólido | Bipe contínuo | "Confirmado visualmente a X m" |
| Qualquer (câmera detecta, LoRa sem nenhum registro) | — | 🔴 Vermelho sólido direto | Alarme contínuo, tom urgente | "NÃO IDENTIFICADO — AÇÃO IMEDIATA" |

**Regra de ouro:** *"Detectar já significa estar perto"* — a vegetação bloqueia a visão a poucos metros, então qualquer detecção da câmera já é evidência de proximidade real.

**Piscando vs. sólido:** piscando = alerta pendente, ainda há incerteza a resolver. Sólido = estado estável (seguro, ou já confirmado).

Um detalhe não óbvio testado durante o desenvolvimento: uma entidade confirmada visualmente a 60m (além dos 50m assumidos de alcance da câmera) **não vira vermelho** — o risco real continua sendo o da distância (fica âmbar). A confirmação da câmera só adiciona certeza, nunca aumenta o nível de risco por si só. Isso já está coberto por teste automatizado (ver `logica_alerta.py`).

### Limitações conhecidas (aceitas por ora, não são bugs)

- Só há câmera na traseira — pra frente e os lados, a única proteção é o LoRa.
- Alcance da câmera é curto (<50m assumido) e não validado em campo.
- **Uma entidade prioritária por vez** — o sistema não faz rastreamento multi-entidade (não resolve 2 tratores próximos simultaneamente). Hoje, qualquer entidade LoRa registrada "explica" uma detecção da câmera, mesmo que essa entidade esteja mais longe do que o alcance real da câmera — é uma simplificação aceita, não correlação fina.
- Falha nunca deve ser silenciosa — precisa de heartbeat Pi ↔ ESP32; se um lado parar, o outro indica isso explicitamente (ex: "CÂMERA OFFLINE"), nunca fica quieto.

---

## O que já está pronto

### 1. Detecção por câmera (YOLOv8n + NCNN)

Histórico de tentativas (documentado pra não repetir os becos sem saída):

1. **MediaPipe** — abandonado. O binário exige instrução AES em hardware que o SoC do Pi 4B (BCM2711/Cortex-A72) não tem. `FATAL ERROR: compiled with aes enabled... Illegal instruction`. Limitação de hardware permanente.
2. **YOLOv8 + ONNXRuntime** — funcionou, mas lento no Pi (motor genérico, não otimizado pra ARM). Removido do projeto.
3. **TFLite (EfficientDet-Lite0)** — fluido, mas reconhece pessoas pior que a opção 4. Existe como comparação separada em outra pasta (`Challenge_John_Deere_TFLite`, fora deste repositório), não é o caminho principal.
4. **YOLOv8n → NCNN, via `ultralytics` (caminho atual)** — mais rápido e mais preciso no Pi 4B. Exportado com `yolo export model=yolov8n.pt format=ncnn imgsz=320`.

**Importante ao instalar no Pi:** `pip install torch torchvision --index-url https://download.pytorch.org/whl/cpu` — o índice genérico do PyPI puxa um CUDA Toolkit inteiro (>2GB, inútil sem GPU NVIDIA) e pode estourar o `/tmp` (tmpfs pequeno, ~1.9GB).

### 2. `raspberry_pi_app/` — o servidor que roda no Pi

```
raspberry_pi_app/
├── app.py                    Flask + inicia a thread de câmera
├── config.py                 CAMERA_SOURCE, limiares de distância — único lugar a editar Mac ↔ Pi
├── estado_compartilhado.py   Estado global com lock: frame/detecções da câmera + entidades LoRa + heartbeats
├── logica_alerta.py          A tabela de verdade em código (testada)
├── requirements.txt
├── deteccao/
│   └── camera_worker.py      Thread: captura + YOLO/NCNN, publica no estado compartilhado
├── comunicacao/
│   └── entidade_receiver.py  Valida o payload que o ESP32 manda por WiFi e atualiza o estado (testado)
├── templates/
│   └── dashboard.html        Painel do operador, servido pelo Flask
└── tests/
    ├── test_logica_alerta.py      10 testes da tabela de verdade, com dados falsos
    └── test_entidade_receiver.py  8 testes de validação do payload do ESP32
```

(Todos os testes rodam sem hardware nenhum: `python -m unittest discover -s tests`.)

**Endpoints do Flask:**
- `GET /` — dashboard completo (HTML/CSS/JS)
- `GET /video_feed` — stream MJPEG (funciona em Chrome/Firefox; **não funciona no Safari/iOS**, ver nota abaixo)
- `GET /snapshot.jpg` — um frame JPEG por vez; é isso que o dashboard usa de verdade (compatível com qualquer navegador, inclusive Safari/iPhone/iPad)
- `GET /api/status` — JSON com `camera_online`, `esp_online`, `deteccoes`, `entidades`, e `alerta` (o resultado já calculado da tabela de verdade)
- `POST /api/entidade` — recebido do ESP32 do trator: `{"id": "a1b2c3", "tipo": "pessoa", "distancia_m": 34.2, "angulo_deg": 175.0}`

**Como rodar (desenvolvimento, hoje no Mac):**
```bash
source venv/bin/activate   # ou: venv/bin/python
cd raspberry_pi_app
python app.py
```
Abre em `http://127.0.0.1:5050/` (porta 5050, não 5000 — no Mac a 5000 é ocupada pelo AirPlay Receiver). De outro aparelho na mesma rede Wi-Fi: `http://<IP-do-Mac-ou-Pi>:5050/`.

**Rodar os testes** (não precisa de câmera nem hardware nenhum):
```bash
cd raspberry_pi_app
python -m unittest discover -s tests -v
```

### 3. Dashboard do operador

Painel único (HTML/CSS/JS, sem build), tema escuro industrial, pensado pra tela dentro da cabine (tablet):

- Vídeo real da câmera (via `/snapshot.jpg`, atualizado em loop — funciona em qualquer navegador)
- Rosa dos ventos ("radar" estilo GTA — blips ficam grudados na borda quando fora do alcance de exibição)
- Semáforo de risco + distância em destaque
- Painel de Áudio/Buzzer com botão de silenciar
- Banner de alerta máximo com botão RECONHECER (silencia o som, mas **nunca** apaga o LED — só a situação real muda isso)
- Duas abas: Painel e Histórico de eventos (com timestamp, severidade, selo de reconhecido)
- Todos os itens acima já consomem `/api/status` de verdade — **não são mais mockados**

Uma faixa de botões (`.demo-strip`, rodapé) existe só pra fins de demonstração visual. Hoje eles só destacam a si mesmos — não controlam mais nada, porque tudo que simulavam virou real. Ficam "mudos" até existir um jeito de injetar uma entidade LoRa falsa (ver Pendências).

**Por que `/snapshot.jpg` e não só `/video_feed`:** o formato de streaming usado em `/video_feed` (`multipart/x-mixed-replace`) não é suportado pelo Safari/WebKit (iPhone, iPad, Mac Safari) — é uma limitação antiga e conhecida do navegador, não bug nosso. O dashboard usa polling de `/snapshot.jpg` a cada ~150ms em vez disso, o que funciona em qualquer navegador.

### 4. `esp32_firmware/ProjetoLoRa1/` — firmware do ESP32 (GPS + LoRa + WiFi opcional)

**O mesmo código roda em todo ESP32 do projeto** — tag de pessoa, tag de outro trator, ou o ESP32 do trator com o Pi. Duas constantes no topo do arquivo mudam o comportamento por dispositivo:

```cpp
#define TIPO_ENTIDADE "pessoa"   // "pessoa" ou "trator" — editar antes de gravar cada unidade
#define TRATOR_COM_PI 0          // 1 SÓ no ESP32 junto do Raspberry Pi; 0 em todos os outros
```

O que o firmware faz:
- Lê a própria posição por GPS (`TinyGPSPlus`) e transmite por LoRa a cada ~3s (com um sorteio simples pra nem todo mundo falar ao mesmo tempo no canal).
- **ID único e autônomo**: gerado a partir do MAC de fábrica do chip (nunca colide entre placas) e salvo na memória não-volátil (`Preferences`) na primeira execução — não precisa configurar nada manualmente por unidade além do `TIPO_ENTIDADE`.
- Ao ouvir outro nó por LoRa, calcula:
  - **Distância** — por GPS (Haversine, mais confiável quando os dois lados têm fix) com fallback por força de sinal/RSSI (quando não há GPS de um dos lados).
  - **Direção (ângulo)** — calculada a partir das duas posições GPS (fórmula de bearing). **Importante:** o LoRa não informa direção nenhuma por si só (só força de sinal); e o "course" que o próprio GPS relata é o rumo de quem está se movendo, não a direção até o outro nó. Por isso o ângulo usado na rosa dos ventos é sempre calculado a partir de duas posições, nunca lido direto de um sensor.
- Se `TRATOR_COM_PI = 1`: conecta na rede WiFi do Pi e manda `POST /api/entidade` (JSON) pra cada nó ouvido — inclusive nós que não têm Raspberry próprio.

**Valores que precisam ser calibrados em campo** (marcados com `TODO CALIBRAR` no arquivo): `LORA_RSSI_1M` e `EXPOENTE_PERDA_AMBIENTE`, usados só no fallback por RSSI. O jeito de calibrar: medir o RSSI com os dois nós a exatamente 1m de distância, depois em 2-3 distâncias conhecidas, e ajustar até a curva bater com a realidade.

**Ainda não testado em hardware nenhum** — o firmware foi escrito e revisado, mas não há como compilar/gravar sem o Arduino IDE (ou `arduino-cli`) e as placas físicas. Testar com atenção antes de confiar nos números.

---

## Pendências, em ordem sugerida

### 1. Testar o firmware com uma segunda placa (LoRa entre dois nós)

`esp32_firmware/ProjetoLoRa1/ProjetoLoRa1.ino` já foi **compilado e gravado numa placa real** com `arduino-cli` (instalado via Homebrew). Confirmado até aqui: ID único gerado sozinho (a partir do MAC do chip), rádio LoRa inicializando sem erro, módulo GPS mandando NMEA válido (fiação e baud rate corretos) — só falta o fix de satélite, que exige céu aberto de verdade (testado numa sacada coberta até agora, sem sucesso; próximo teste é num lugar mais aberto). Ainda falta testar com uma **segunda placa** pra validar a troca de pacote por LoRa (RSSI, distância, ângulo calculado).

Comandos usados (referência, já configurados neste Mac):
```bash
arduino-cli compile --fqbn esp32:esp32:esp32 esp32_firmware/ProjetoLoRa1
arduino-cli upload -p <porta> --fqbn esp32:esp32:esp32 esp32_firmware/ProjetoLoRa1
```

### 2. ~~Configurar o Raspberry Pi como ponto de acesso WiFi~~ ✅ Feito, com um problema pendente

Hotspot criado com sucesso via `nmcli` (`docs/configurar_pi_como_ap.md`), IP `10.42.0.1` confirmado, e o ESP32 (com `TRATOR_COM_PI=1`, `TIPO_ENTIDADE="trator"`) já conectou nele de verdade pelo menos uma vez ("WiFi conectado" no Serial).

**Problema encontrado, ainda não resolvido:** em testes seguintes, a reconexão do ESP32 às vezes é recusada pelo hotspot, com o erro no Serial:
```
E wifi:Association refused too many times, max allowed 1
```
Isso indica que o hotspot só está aceitando **1 cliente conectado por vez** (ou tem uma conexão antiga/fantasma ocupando a única vaga). Precisa investigar do lado do Pi — provavelmente existe uma opção do `nmcli`/`hostapd` pra aumentar esse limite (o sistema precisa aceitar pelo menos: o ESP32 do trator + o tablet do operador + celular de teste, ao mesmo tempo). Também tentamos adicionar um heartbeat próprio do ESP32 (`POST /api/heartbeat`, independente de ter entidade LoRa pra mandar) pra resolver um sintoma relacionado (indicador "ESP32 online" do dashboard não acendia sozinho) — **essa tentativa foi revertida** (não é o problema raiz, e complicava sem resolver o de verdade). Reavaliar o heartbeat depois de resolver o limite de clientes do hotspot.

### 3. ~~Injetar uma entidade LoRa falsa~~ ✅ Feito

O dashboard agora tem botões de demo que mandam uma entidade **falsa pro backend real** via `POST /api/entidade` (o mesmo endpoint que o ESP32 vai usar) — não é mock só no front-end, é `logica_alerta.py` calculando de verdade em cima de um dado simulado. Serve de **plano B pra demonstração** caso o GPS/ESP32 não funcione ao vivo: dá pra mostrar todos os níveis de alerta (inclusive interagindo com a câmera real — os botões "crítico" pedem pra você ficar dentro ou fora do quadro da câmera pra ver a diferença entre pendente/confirmado).

**Interruptor "Modo Demo"**: com hardware real conectado (ESP32 de verdade transmitindo por LoRa), dado real e dado de demo podiam entrar em conflito (ex: uma tag desconectada ficava "grudada" no último valor, competindo com os botões). Agora existe um botão `MODO DEMO: LIGADO/DESLIGADO` que decide qual fonte conta — nunca mistura os dois (`POST /api/modo-demo`, filtrado em `app.py > _alerta_atual()`). Também foi corrigido o bug de entidade "grudada": `estado.ler_entidades()` agora aceita um timeout (`config.ENTIDADE_TIMEOUT_S = 15`) e esconde entidades que pararam de atualizar (ex: tag desligada), em vez de manter o último valor pra sempre.

### 4. Calibrar os parâmetros de distância por RSSI

`LORA_RSSI_1M` e `EXPOENTE_PERDA_AMBIENTE`, marcados com `TODO CALIBRAR` no firmware — precisam de medição real em campo (ver seção do firmware acima).

### 5. ~~LED físico + buzzer~~ ✅ Código pronto, falta a fiação real

`configurarAlertaFisico()`/`atualizarAlertaFisico()` no firmware já fazem tudo: 3 LEDs (verde/amarelo/vermelho) + buzzer, com o vermelho piscando e o buzzer com bipe espaçado/contínuo dependendo da distância — usando os mesmos limiares (50m/100m) do painel, só que 100% local (nunca depende de Wi-Fi/Pi). Pinos definidos: `LED_VERDE_PIN=14, LED_AMARELO_PIN=27, LED_VERMELHO_PIN=26, BUZZER_PIN=13` (livres, sem conflito com LoRa/GPS: 4, 5, 16, 17, 18, 19, 21, 23; nem com os reservados do ESP32: flash 6-11, strapping/boot 0/2/12/15, só-entrada 34-39).

Além disso, quando o Pi está acessível, o ESP32 consulta `GET /api/alerta-fisico` a cada 1s e o LED/buzzer passam a refletir a lógica **cruzada** (câmera + LoRa, igual ao dashboard — inclusive o vermelho "confirmado" sólido, que o modo local sozinho não consegue mostrar). Se o Pi não responder por 5s, cai automaticamente de volta pro modo local.

### 6. ~~Portar pro Raspberry Pi de verdade~~ ✅ Feito e calibrado

Rodando de verdade no Pi 4B, com webcam USB (Logitech C922, detectada em `/dev/video0` — coincidiu com o índice já usado no Mac, `CAMERA_SOURCE = 0`, nenhuma mudança necessária aí).

**Calibração de performance** — o Mac (Apple Silicon) é bem mais rápido que o Pi 4B; os valores em `config.py` foram ajustados especificamente pro Pi:
- `CAMERA_LARGURA/ALTURA` reduzidos (480×360)
- `PULAR_FRAMES = 7` (roda a detecção a cada 8 frames)
- `JPEG_QUALIDADE = 60` (só afeta a imagem exibida, não a detecção — o modelo já processou o frame original antes de comprimir)

**Importante — não mexer em `INFERENCE_SIZE`:** o modelo NCNN foi exportado com `imgsz=320` fixo. Mudar esse valor no código NÃO redimensiona a entrada de verdade (modelos NCNN têm shape de entrada fixo) — só degrada ou quebra a detecção silenciosamente. Pra rodar com um tamanho de entrada diferente de verdade, precisaria reexportar o modelo NCNN com outro `imgsz`.

**Vulkan/GPU testado e descartado por enquanto:** a `ultralytics` tem suporte a rodar o NCNN via Vulkan (`device="vulkan:0"` em vez de `"cpu"`, usando a GPU VideoCore VI do Pi em vez da CPU). Testamos e **travou o processo de verdade** (nem `Ctrl+C` funcionava, precisou matar o processo à força) — o driver Mesa/v3dv do Pi não lidou bem com isso. Ficou como `DEVICE = "cpu"` em `config.py`. Se alguém quiser investigar de novo no futuro (fora de época de demonstração, com tempo pra debugar com calma), a opção está lá, comentada.

**Transferência de arquivos Mac → Pi** via `rsync` (excluindo sempre `venv/` e `__pycache__/`, que não são portáveis entre arquiteturas):
```bash
rsync -avz --progress --exclude 'venv' --exclude '__pycache__' /caminho/do/projeto/no/mac/ tonii@<ip-do-pi>:~/Desktop/Challenge_John_Deere_Central/
```
(é incremental — rodar de novo depois de qualquer mudança no Mac só transfere o que mudou)

### 7. Testes de campo

- Alcance real do LoRa sob dossel de eucalipto
- Precisão do GPS sob copa densa
- Alcance real de detecção da câmera (a estimativa de <50m é conservadora, nunca testada de verdade)

### 8. Ainda não decidido / fora de escopo por ora

- Gateway central pra visualizar todos os tratores/pessoas numa central da floresta (adiado deliberadamente)
- Câmera térmica, LIDAR
- Design físico definitivo do case da câmera e das tags vestíveis (ideias discutidas: impressão 3D em ASA, caixas prontas IP65/67 tipo Hammond — cuidado pra não bloquear antena LoRa nem visão de céu do GPS)

---

## Decisões de arquitetura já tomadas (não reabrir sem motivo novo)

Pra evitar retrabalho, aqui vai o resumo do que já foi decidido e descartado, com o porquê:

- **1 câmera, traseira, fixa (sem pan-tilt)** — ponto cego real + limite de processamento do Pi + partes móveis quebram com vibração.
- **Tablet, não OLED pequeno** — precisa mostrar vídeo.
- **LED + buzzer ligados direto no ESP32** — camada redundante que nunca depende de Wi-Fi/tablet/servidor.
- **YOLOv8n + NCNN via `ultralytics`** — venceu MediaPipe (incompatível com o Pi), ONNXRuntime (lento) e TFLite (menos preciso).
- **Câmera não faz correlação de ângulo com o LoRa** — só presença/ausência. Correlação fina foi cogitada e descartada (exigiria bússola + GPS precisos, não confiáveis sob floresta).
- **Sistema não rastreia múltiplas entidades simultâneas** — limitação aceita, não bug.
- **Cor do radar é sempre neutra (azul)** — nunca reaproveitar verde/amarelo/vermelho ali, porque essas cores já têm significado fixo de risco no semáforo.
- **ESP32 do trator fala com o Pi por WiFi, não cabo serial.**
- **O Pi cria a rede WiFi (Access Point), não o ESP32** — o Pi já hospeda o dashboard de qualquer forma, é a máquina mais capaz (Linux completo) e não compete por recursos com o loop de GPS/LoRa do ESP32, que já está ocupado. Ver `docs/configurar_pi_como_ap.md`.
- **Um único firmware serve todo tipo de nó** (pessoa, trator sem Pi, trator com Pi) — o que muda é só `TIPO_ENTIDADE` e `TRATOR_COM_PI` no topo do `.ino`. Evita manter várias variantes de código pra manter sincronizadas.
- **ID de cada ESP32 é gerado a partir do MAC de fábrica do chip**, não digitado manualmente — garante unicidade sem precisar de configuração por unidade além do tipo.

---

## Setup local (qualquer notebook — Mac, Linux ou Windows)

O projeto todo é Python puro + Flask, então roda em qualquer sistema com Python 3.9+ e uma webcam. Só a criação/ativação do ambiente virtual muda de comando entre sistemas — o resto é idêntico.

**1. Clonar o repositório e criar o ambiente virtual:**

```bash
git clone https://github.com/RodazTheCoder/John_Deere_App.git
cd John_Deere_App
python3 -m venv venv   # Windows: py -m venv venv (ou "python -m venv venv")
```

**2. Ativar o ambiente virtual** (comando diferente por sistema):

| Sistema | Comando |
|---|---|
| macOS / Linux (bash, zsh) | `source venv/bin/activate` |
| Windows (PowerShell) | `venv\Scripts\Activate.ps1` |
| Windows (CMD) | `venv\Scripts\activate.bat` |
| Windows (Git Bash) | `source venv/Scripts/activate` |

O prompt do terminal deve mostrar `(venv)` no começo quando ativado com sucesso.

**3. Instalar as dependências** (mesmo comando em qualquer sistema):

```bash
pip install torch torchvision --index-url https://download.pytorch.org/whl/cpu
pip install -r raspberry_pi_app/requirements.txt
```

O modelo NCNN (`yolov8n_ncnn_model/`) já vem no repositório, na raiz do projeto — não precisa exportar de novo (só seria necessário recriá-lo com `yolo export model=yolov8n.pt format=ncnn imgsz=320` se ele não existisse).

**4. Rodar:**

```bash
cd raspberry_pi_app
python app.py
```

Abre em `http://127.0.0.1:5050/` (ou `http://localhost:5050/`).

### Diferenças a esperar entre sistemas

- **Porta 5050, não 5000**: escolhida assim de propósito porque no macOS a porta 5000 é ocupada pelo AirPlay Receiver. Em Linux/Windows isso não é um problema, mas a porta 5050 funciona igual nos três, então não precisa mudar nada.
- **Índice da câmera** (`CAMERA_SOURCE` em `raspberry_pi_app/config.py`): `0` costuma ser a primeira/única webcam em qualquer sistema, mas se o notebook tiver mais de uma câmera (webcam embutida + uma USB, por exemplo), pode precisar trocar pra `1`, `2`, etc. até achar a certa — não tem como saber sem testar.
- **Windows especificamente**: se `pip install` reclamar de compilador ausente ao instalar `opencv-python`, geralmente já existe wheel pré-compilado pra Windows e não deveria precisar compilar nada — mas se der erro, verificar se está usando Python 64-bit (não 32-bit).
- **Linux**: se a webcam não for detectada, conferir se o usuário tem permissão de acesso a `/dev/video0` (grupo `video` no Ubuntu/Debian: `sudo usermod -aG video $USER`, depois logout/login).

Documento de apresentação (`.pptx`, 12 slides) com a lógica de negócio completa existe à parte — é a fonte "oficial" da lógica; qualquer código deve implementar exatamente o que está lá, sem inventar variações sem avisar o grupo.
