import threading
import time

from flask import Flask, Response, jsonify, render_template, request

import config
from estado_compartilhado import estado
from deteccao import camera_worker
from comunicacao.entidade_receiver import processar_entidade
from logica_alerta import calcular_alerta

app = Flask(__name__)


@app.route("/")
def index():
    return render_template("dashboard.html")


def _gerar_stream_mjpeg():
    while True:
        frame_jpeg = estado.ler_frame()
        if frame_jpeg is None:
            time.sleep(0.1)
            continue
        yield (
            b"--frame\r\n"
            b"Content-Type: image/jpeg\r\n\r\n" + frame_jpeg + b"\r\n"
        )
        time.sleep(1 / 30)  # limita a ~30fps, sem isso o loop reenvia o mesmo frame o mais rápido possível


@app.route("/video_feed")
def video_feed():
    # multipart/x-mixed-replace não é suportado pelo Safari/WebKit (iPhone/iPad).
    # O dashboard usa /snapshot.jpg em polling em vez deste endpoint.
    return Response(_gerar_stream_mjpeg(), mimetype="multipart/x-mixed-replace; boundary=frame")


@app.route("/snapshot.jpg")
def snapshot():
    frame_jpeg = estado.ler_frame()
    if frame_jpeg is None:
        return "", 503
    return Response(frame_jpeg, mimetype="image/jpeg", headers={"Cache-Control": "no-store"})


@app.route("/api/status")
def status():
    camera_online = estado.camera_online(config.HEARTBEAT_TIMEOUT_S)
    entidades = estado.ler_entidades()
    deteccoes = estado.ler_deteccoes()
    return jsonify({
        "camera_online": camera_online,
        "esp_online": estado.esp_online(config.HEARTBEAT_TIMEOUT_S),
        "deteccoes": deteccoes,
        "entidades": entidades,
        "alerta": calcular_alerta(entidades, deteccoes, camera_online),
    })


@app.route("/api/entidade", methods=["POST"])
def entidade():
    """Recebido do ESP32 do trator via WiFi — ver comunicacao/entidade_receiver.py
    e esp32_firmware/ProjetoLoRa1/ProjetoLoRa1.ino (enviarEntidadeProPi)."""
    payload = request.get_json(silent=True)
    ok, erro = processar_entidade(payload, estado)
    if not ok:
        return jsonify({"erro": erro}), 400
    return jsonify({"ok": True})


@app.route("/api/entidade/<entidade_id>", methods=["DELETE"])
def remover_entidade(entidade_id):
    """Usado pelo botão de demo do dashboard, pra simular a entidade LoRa
    sumindo (ex: estado "não identificado", sem nenhum registro)."""
    estado.remover_entidade(entidade_id)
    return jsonify({"ok": True})


def main():
    threading.Thread(target=camera_worker.run, daemon=True).start()
    app.run(host=config.FLASK_HOST, port=config.FLASK_PORT, threaded=True)


if __name__ == "__main__":
    main()
