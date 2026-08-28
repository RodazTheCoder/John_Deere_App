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

    # Escala configurável (ver /api/escala): mantém sempre a proporção 1:2
    # entre atenção e crítico, só muda o "tamanho" da régua -- útil pra
    # testar em ambientes menores (sala) sem precisar alcançar 100m de
    # verdade. None = usa o padrão de config.py.
    escala_verde = estado.escala_verde_m()
    if escala_verde is None:
        escala_verde = config.DISTANCIA_VERDE_M
    escala_amarelo = escala_verde / 2

    alerta = calcular_alerta(entidades, deteccoes, camera_online, escala_verde, escala_amarelo)
    if estado.som_silenciado(alerta["nivel"]):
        # Silenciado pelo operador pra esse nível específico -- vale pro
        # dashboard E pro buzzer físico (ambos leem esse mesmo campo). O LED
        # não é tocado aqui, continua refletindo a situação real.
        alerta = {**alerta, "som": {"estado": "silenciado"}}
    return camera_online, entidades, deteccoes, alerta, escala_verde, escala_amarelo


@app.route("/api/status")
def status():
    camera_online, entidades, deteccoes, alerta, escala_verde, escala_amarelo = _alerta_atual()
    return jsonify({
        "camera_online": camera_online,
        "esp_online": estado.esp_online(config.HEARTBEAT_TIMEOUT_S),
        "deteccoes": deteccoes,
        "entidades": entidades,
        "alerta": alerta,
        "modo_demo": estado.modo_demo(),
        "escala_verde_m": escala_verde,
        "escala_amarelo_m": escala_amarelo,
    })


@app.route("/api/escala", methods=["POST"])
def escala():
    """Reconfigura os limiares verde/amarelo em tempo real, mantendo a
    proporção 1:2 -- pra testar em ambientes menores (sala) sem precisar de
    100m de verdade. {"verde_m": 20} -> vermelho <10m, amarelo 10-20m, verde
    >20m. {"verde_m": null} reseta pro padrão de config.py (100m)."""
    payload = request.get_json(silent=True) or {}
    verde_m = payload.get("verde_m")
    if verde_m is not None:
        verde_m = float(verde_m)
    estado.definir_escala(verde_m)
    return jsonify({"ok": True, "escala_verde_m": verde_m})


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

    Formato: "cor,piscando,som,verde_m,amarelo_m" -- ex:
    "vermelho,0,continuo,20.0,10.0".

    Os dois últimos campos (a escala atual, ver /api/escala) são pro ESP32
    guardar como cache -- se o Pi cair, o fallback local (só distância, ver
    atualizarAlertaFisico() no firmware) passa a usar a ÚLTIMA escala
    confirmada em vez do padrão de fábrica (100m). Sem isso, um teste em
    escala menor (sala) "voltaria" pra 100m de repente se a rede oscilasse
    no pior momento possível.

    Se o ESP32 não conseguir consultar isso (Pi caiu, sem WiFi, câmera
    travada, etc.), ele cai sozinho pro fallback local. O LED nunca fica
    sem lógica nenhuma só porque essa rota parou de responder.
    """
    _, _, _, alerta, escala_verde, escala_amarelo = _alerta_atual()
    piscando = 1 if alerta["led"]["piscando"] else 0
    corpo = f"{alerta['led']['cor']},{piscando},{alerta['som']['estado']},{escala_verde},{escala_amarelo}"
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
    _, _, _, alerta, _, _ = _alerta_atual()
    estado.silenciar_som(alerta["nivel"])
    return jsonify({"ok": True})


def main():
    threading.Thread(target=camera_worker.run, daemon=True).start()
    app.run(host=config.FLASK_HOST, port=config.FLASK_PORT, threaded=True)


if __name__ == "__main__":
    main()
