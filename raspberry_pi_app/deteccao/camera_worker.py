import time

import cv2
from ultralytics import YOLO

import config
from estado_compartilhado import estado


def run():
    """Thread de câmera: captura frames, roda YOLO (NCNN) e publica no estado compartilhado.

    O frame anotado vira JPEG e o estado compartilhado é a única saída
    (consumida pelo Flask em app.py).
    """
    modelo = YOLO(config.MODELO_PATH, task="detect")

    cap = cv2.VideoCapture(config.CAMERA_SOURCE)
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, config.CAMERA_LARGURA)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, config.CAMERA_ALTURA)
    cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)

    frame_count = 0
    ultimas_caixas = []

    while True:
        ret, frame = cap.read()
        if not ret:
            time.sleep(0.5)
            continue

        frame_count += 1
        deve_detectar = (config.PULAR_FRAMES == 0) or (frame_count % (config.PULAR_FRAMES + 1) == 0)

        if deve_detectar:
            resultados = modelo.predict(
                frame,
                imgsz=config.INFERENCE_SIZE,
                conf=config.CONF_THRESHOLD,
                classes=[config.CLASSE_PESSOA],
                device=config.DEVICE,
                verbose=False,
            )[0]

            ultimas_caixas = []
            if resultados.boxes is not None:
                for box in resultados.boxes:
                    x1, y1, x2, y2 = map(int, box.xyxy[0])
                    conf = float(box.conf[0])
                    ultimas_caixas.append({"x1": x1, "y1": y1, "x2": x2, "y2": y2, "conf": conf})

        for caixa in ultimas_caixas:
            cv2.rectangle(frame, (caixa["x1"], caixa["y1"]), (caixa["x2"], caixa["y2"]), (0, 255, 0), 2)
            cv2.putText(
                frame, f"Pessoa {caixa['conf']:.2f}", (caixa["x1"], caixa["y1"] - 10),
                cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 0), 2
            )

        # Qualidade reduzida só pra exibição -- não afeta a detecção (o modelo
        # já processou o frame original antes disso). No Pi, o gargalo real é
        # a inferência do YOLO, não a qualidade da imagem; mas um JPEG mais
        # pesado ainda soma tempo de codificação + transferência pela rede
        # sem trazer benefício nenhum.
        ok, jpeg = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, config.JPEG_QUALIDADE])
        if ok:
            estado.atualizar_frame(jpeg.tobytes(), ultimas_caixas)
