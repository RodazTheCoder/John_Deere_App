import threading
import time

from flask import Flask, Response, jsonify, render_template, request

import config
from estado_compartilhado import estado
from deteccao import camera_worker
from comunicacao.entidade_receiver import processar_entidade
from logica_alerta import calcular_alerta

app = Flask(__name__)

# Id fixo usado pelos botões de demo do dashboard (ver ENTIDADE_DEMO_ID em
# dashboard.html -- tem que ser o mesmo valor dos dois lados).
ENTIDADE_DEMO_ID = "demo-pessoa"


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
    todas_entidades = estado.ler_entidades(config.ENTIDADE_TIMEOUT_S)

    # Modo demo ligado: só a entidade falsa dos botões conta (dado real de
    # LoRa é ignorado). Modo demo desligado: só entidade real conta (clicar
    # nos botões de demo não tem efeito nenhum). Nunca mistura os dois.
    if estado.modo_demo():
        entidades = {k: v for k, v in todas_entidades.items() if k == ENTIDADE_DEMO_ID}
    else:
        entidades = {k: v for k, v in todas_entidades.items() if k != ENTIDADE_DEMO_ID}

    deteccoes = estado.ler_deteccoes()
    alerta = calcular_alerta(entidades, deteccoes, camera_online)
    if estado.som_silenciado(alerta["nivel"]):
        # Silenciado pelo operador pra esse nível específico -- vale pro
        # dashboard E pro buzzer físico (ambos leem esse mesmo campo). O LED
        # não é tocado aqui, continua refletindo a situação real.
        alerta = {**alerta, "som": {"estado": "silenciado"}}
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
        "modo_demo": estado.modo_demo(),
    })


@app.route("/api/modo-demo", methods=["POST"])
def modo_demo():
    """Interruptor "Modo Demo" do dashboard. Ligado: só a entidade falsa dos
    botões de demo conta (dado real do LoRa é ignorado). Desligado: só
    entidade real conta. Nunca mistura mock com dado real -- sem isso, uma
    tag desconectada podia "empatar" com um clique de demo e dar resultado
    imprevisível."""
    payload = request.get_json(silent=True) or {}
    ativo = bool(payload.get("ativo", False))
    estado.definir_modo_demo(ativo)
    return jsonify({"ok": True, "modo_demo": ativo})


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


@app.route("/api/silenciar", methods=["POST"])
def silenciar():
    """Botão SILENCIAR/RECONHECER do dashboard. Silencia o som -- no
    dashboard E no buzzer físico do ESP32, já que os dois leem o mesmo
    `alerta["som"]` -- até o nível de alerta mudar de verdade. O LED nunca é
    afetado por isso."""
    _, _, _, alerta = _alerta_atual()
    estado.silenciar_som(alerta["nivel"])
    return jsonify({"ok": True})


def main():
    threading.Thread(target=camera_worker.run, daemon=True).start()
    app.run(host=config.FLASK_HOST, port=config.FLASK_PORT, threaded=True)


if __name__ == "__main__":
    main()
