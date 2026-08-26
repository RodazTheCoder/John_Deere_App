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


def _alerta_atual():
    camera_online = estado.camera_online(config.HEARTBEAT_TIMEOUT_S)
    entidades = estado.ler_entidades()
    deteccoes = estado.ler_deteccoes()
    alerta = calcular_alerta(entidades, deteccoes, camera_online)
    return camera_online, entidades, deteccoes, alerta


@app.route("/api/status")
def status():
    camera_online, entidades, deteccoes, alerta = _alerta_atual()
    return jsonify({
        "camera_online": camera_online,
        "esp_online": estado.esp_online(config.HEARTBEAT_TIMEOUT_S),
        "deteccoes": deteccoes,
        "entidades": entidades,
        "alerta": alerta,
    })


@app.route("/api/alerta-fisico")
def alerta_fisico():
    """Versão compacta em texto simples (não JSON) do alerta atual -- pro
    ESP32 do trator conseguir fazer o LED/buzzer físico refletir a lógica
    cruzada (câmera + LoRa) sem precisar de biblioteca de JSON no firmware.

    Formato: "cor,piscando,som" -- ex: "vermelho,0,continuo".

    Se o ESP32 não conseguir consultar isso (Pi caiu, sem WiFi, câmera
    travada, etc.), ele cai sozinho de volta pro fallback local (só
    distância) -- ver atualizarAlertaFisico() no firmware. O LED nunca fica
    sem lógica nenhuma só porque essa rota parou de responder.
    """
    _, _, _, alerta = _alerta_atual()
    piscando = 1 if alerta["led"]["piscando"] else 0
    corpo = f"{alerta['led']['cor']},{piscando},{alerta['som']['estado']}"
    return Response(corpo, mimetype="text/plain")


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
