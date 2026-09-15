# Sistema de Detecção e Prevenção de Colisão — Challenge John Deere

Sistema de segurança para tratores florestais que detecta pessoas e outros tratores próximos à máquina, combinando duas camadas de sensoriamento independentes:

1. **LoRa + GPS + ESP32** — cada trator e cada pessoa no terreno carrega um nó com GPS + rádio LoRa, transmitindo posição continuamente. Funciona em qualquer distância/visibilidade (atrás de árvore, à noite), mas depende de todo mundo estar usando o dispositivo.
2. **Câmera + Raspberry Pi + visão computacional** — uma câmera na traseira do trator detecta pessoas/veículos visíveis, com ou sem dispositivo LoRa.

O cruzamento dessas duas fontes gera um alerta de 3 níveis (semáforo verde/amarelo/vermelho), mostrado num dashboard web (tablet na cabine) **e** refletido em LED/buzzer ligados direto no ESP32 — essa segunda via é redundante de propósito: continua funcionando mesmo se o Raspberry Pi travar ou perder rede.

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
