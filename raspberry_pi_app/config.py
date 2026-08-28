import os

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
RAIZ_PROJETO = os.path.dirname(BASE_DIR)

# ---- Câmera ----
# Índice/caminho da câmera. Único ponto a trocar entre Mac (webcam embutida,
# geralmente 0) e Raspberry Pi (USB, geralmente também 0, mas pode variar).
CAMERA_SOURCE = 0

# Valores calibrados pro Mac (Apple Silicon) eram mais agressivos; o Pi 4B é
# bem mais lento — reduzida a resolução de captura e aumentado PULAR_FRAMES
# pra rodar mais fluido. INFERENCE_SIZE NÃO pode mudar: o modelo NCNN foi
# exportado com imgsz=320 fixo (`yolo export ... imgsz=320`), um valor
# diferente aqui não redimensiona a entrada de verdade -- só degrada ou
# quebra a detecção silenciosamente. Pra mudar isso de verdade, precisaria
# reexportar o modelo NCNN com outro imgsz (ver README.md).
CAMERA_LARGURA = 480
CAMERA_ALTURA = 360
INFERENCE_SIZE = 320
PULAR_FRAMES = 7
JPEG_QUALIDADE = 60  # 0-100; só afeta o vídeo exibido, não a detecção (já rodou antes)

# "cpu" (padrão, seguro) ou "vulkan:0" pra tentar usar a GPU do Pi (VideoCore
# VI) via Vulkan -- mesma precisão, só processador diferente. NÃO testado
# ainda de verdade no hardware (depende do driver Vulkan do Pi funcionar com
# o pacote `ncnn` instalado). Se der erro ou a câmera parar de funcionar ao
# trocar pra "vulkan:0", é só voltar pra "cpu" e resincronizar.
DEVICE = "cpu"  # "vulkan:0" travou o driver Mesa/v3dv no teste real -- não usar sem investigar mais
CONF_THRESHOLD = 0.4
CLASSE_PESSOA = 0  # índice "person" no COCO

MODELO_PATH = os.path.join(RAIZ_PROJETO, "yolov8n_ncnn_model")

# ---- ESP32 / LoRa ----
# O ESP32 do trator manda os dados por WiFi (POST /api/entidade), não por
# cabo serial — ver esp32_firmware/ProjetoLoRa1/ e comunicacao/entidade_receiver.py.
# Não há nada a configurar aqui do lado do Pi: o SSID/senha da rede ficam no
# firmware do ESP32, e o Pi só precisa estar rodando como Access Point (ver
# README.md, seção "Pendências > Raspberry Pi como ponto de acesso WiFi").

# ---- Tabela de verdade (distâncias) ----
DISTANCIA_VERDE_M = 100      # > 100m: seguro
DISTANCIA_AMARELO_M = 50     # 50-100m: atenção, fora do alcance da câmera
ALCANCE_CAMERA_M = 50        # < 50m: alcance realista de detecção da câmera na floresta

# ---- Heartbeat (falha nunca deve ser silenciosa) ----
HEARTBEAT_TIMEOUT_S = 5

# Tempo sem atualização até uma entidade LoRa ser considerada "sumida" (ex:
# desligou a tag física). Sem isso, o último dado recebido fica valendo pra
# sempre. O ESP32 só transmite a cada ~3s, com 50% de chance de pular a vez
# (ver INTERVALO_JANELA_MS no firmware) -- isso cria uma variação real no
# intervalo entre transmissões (~3-9s típico, mas sequências de "azar"
# acontecem: com 15s de timeout, ~3% de chance a cada checagem de dar 5
# pulos seguidos e a entidade "piscar" pra seguro sem ninguém ter saído do
# lugar). 30s deixa essa chance bem mais rara (~0.1%) sem demorar bobagem
# pra perceber uma tag desligada de verdade.
ENTIDADE_TIMEOUT_S = 30

# ---- Flask ----
FLASK_HOST = "0.0.0.0"
# 5000 conflita no Mac com o AirPlay Receiver (System Settings > General > AirDrop & Handoff)
FLASK_PORT = 5050
