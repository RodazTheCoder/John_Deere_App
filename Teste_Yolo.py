import cv2
from ultralytics import YOLO

# ---- CONFIGURAÇÕES DE PERFORMANCE ----
CAMERA_LARGURA = 640      # captura já em resolução menor (menos trabalho pra decodificar/redimensionar)
CAMERA_ALTURA = 480
INFERENCE_SIZE = 320      # resolução que o modelo recebe (320 é bem mais rápido que 640, e ainda detecta pessoa bem)
PULAR_FRAMES = 1          # roda a detecção a cada N frames (0 = roda todo frame)
CONF_THRESHOLD = 0.4
CLASSE_PESSOA = 0         # índice "person" no COCO

# ---- CARREGAR MODELO NCNN ----
# Aponta pra pasta gerada pelo export (ex: "yolov8n_ncnn_model")
modelo = YOLO("yolov8n_ncnn_model", task="detect")

# ---- CÂMERA ----
cap = cv2.VideoCapture(0)
cap.set(cv2.CAP_PROP_FRAME_WIDTH, CAMERA_LARGURA)
cap.set(cv2.CAP_PROP_FRAME_HEIGHT, CAMERA_ALTURA)
cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)  # evita acumular frames atrasados no buffer

frame_count = 0
ultimas_caixas = []  # guarda a última detecção pra reusar nos frames pulados

while True:
    ret, frame = cap.read()
    if not ret:
        break

    frame_count += 1
    deve_detectar = (PULAR_FRAMES == 0) or (frame_count % (PULAR_FRAMES + 1) == 0)

    if deve_detectar:
        resultados = modelo.predict(
            frame,
            imgsz=INFERENCE_SIZE,
            conf=CONF_THRESHOLD,
            classes=[CLASSE_PESSOA],   # já filtra só pessoa dentro da inferência (mais rápido que filtrar depois)
            verbose=False,
        )[0]

        ultimas_caixas = []
        if resultados.boxes is not None:
            for box in resultados.boxes:
                x1, y1, x2, y2 = map(int, box.xyxy[0])
                conf = float(box.conf[0])
                ultimas_caixas.append((x1, y1, x2, y2, conf))

    # desenha sempre (mesmo nos frames pulados, reaproveitando a última detecção)
    for (x1, y1, x2, y2, conf) in ultimas_caixas:
        cv2.rectangle(frame, (x1, y1), (x2, y2), (0, 255, 0), 2)
        cv2.putText(
            frame, f"Pessoa {conf:.2f}", (x1, y1 - 10),
            cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 0), 2
        )

    print(f"Pessoas detectadas: {len(ultimas_caixas)}")
    cv2.imshow("YOLO NCNN otimizado", frame)
    if cv2.waitKey(1) & 0xFF == ord("q"):
        break

cap.release()
cv2.destroyAllWindows()
