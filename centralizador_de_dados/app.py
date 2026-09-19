from __future__ import annotations

import atexit
import json
import os
import threading
import time
from datetime import datetime, timezone
from typing import Any

from flask import Flask, jsonify, render_template

try:
    import serial
    import serial.tools.list_ports
except ImportError:  # pragma: no cover - depende do ambiente de execução
    serial = None

app = Flask(__name__, template_folder="templates")


def listar_portas_disponiveis() -> list[str]:
    if serial is None:
        return []
    try:
        return [porta.device for porta in serial.tools.list_ports.comports()]
    except Exception:
        return []


def descobrir_porta_serial() -> str:
    global SERIAL_PORT

    porta_forcada = os.getenv("CENTRALIZADOR_PORT")
    if porta_forcada:
        SERIAL_PORT = porta_forcada
        return porta_forcada

    portas = listar_portas_disponiveis()
    if portas:
        SERIAL_PORT = portas[0]
        return portas[0]

    SERIAL_PORT = "COM3" if os.name == "nt" else "/dev/ttyUSB0"
    return SERIAL_PORT


SERIAL_PORT = "COM3" if os.name == "nt" else "/dev/ttyUSB0"
SERIAL_BAUD = int(os.getenv("CENTRALIZADOR_BAUD", "9600"))
SERIAL_PORT = descobrir_porta_serial()

VEICULOS: dict[str, dict[str, Any]] = {}
ULTIMO_PACOTE_TS: float | None = None
HISTORICO: dict[str, list[dict[str, Any]]] = {}

_lock = threading.Lock()
_serial_thread = None
SERIAL_HANDLE = None
SERIAL_STOP_EVENT = threading.Event()


def fechar_serial() -> None:
    global SERIAL_HANDLE
    try:
        if SERIAL_HANDLE is not None:
            try:
                SERIAL_HANDLE.close()
            except Exception:
                pass
    finally:
        SERIAL_HANDLE = None


def agora_iso() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def adicionar_historico(veiculo_id: str, latitude: float, longitude: float) -> None:
    ponto = {
        "latitude": float(latitude),
        "longitude": float(longitude),
        "timestamp": agora_iso(),
    }
    HISTORICO.setdefault(veiculo_id, []).append(ponto)
    if len(HISTORICO[veiculo_id]) > 120:
        HISTORICO[veiculo_id] = HISTORICO[veiculo_id][-120:]


def normalizar_entidade(payload: dict[str, Any]) -> dict[str, Any]:
    data = dict(payload)
    veiculo_id = str(data.get("id") or data.get("codigo") or "desconhecido").strip()
    tipo = str(data.get("tipo") or data.get("entity_type") or "trator").strip()

    latitude = data.get("latitude", data.get("lat"))
    longitude = data.get("longitude", data.get("lon"))
    if latitude is None or longitude is None:
        raise ValueError("latitude/longitude obrigatórios")

    latitude = float(latitude)
    longitude = float(longitude)
    nome = str(data.get("nome") or veiculo_id).strip()

    veiculo = {
        "id": veiculo_id,
        "nome": nome,
        "tipo": tipo,
        "latitude": latitude,
        "longitude": longitude,
        "status": str(data.get("status") or "ativo"),
        "velocidade_kmh": float(data.get("velocidade_kmh", data.get("velocidade", 0)) or 0.0),
        "timestamp": datetime.now(timezone.utc).timestamp(),
        "ultimo_update": agora_iso(),
    }
    if "distancia_m" in data:
        veiculo["distancia_m"] = float(data["distancia_m"])
    if "angulo_deg" in data:
        veiculo["angulo_deg"] = float(data["angulo_deg"])
    if "rssi_dbm" in data:
        veiculo["rssi_dbm"] = float(data["rssi_dbm"])
    return veiculo


def linha_serial_deve_ser_ignorada(texto: str) -> bool:
    linha = (texto or "").strip()
    if not linha:
        return True
    if linha.startswith("[") and "GPS" in linha:
        return True
    if "chars processados=" in linha and "fix=" in linha:
        return True
    if "AVISO:" in linha and "GPS" in linha:
        return True
    if "LoRa" in linha and "centralizador" in linha.lower():
        return True
    if linha.startswith("--") and linha.endswith("--"):
        return True
    return False


def parse_payload(raw: str) -> dict[str, Any]:
    texto = (raw or "").strip()
    if not texto:
        raise ValueError("payload vazio")
    if linha_serial_deve_ser_ignorada(texto):
        raise ValueError("linha de diagnóstico serial ignorada")

    if texto.startswith("{"):
        data = json.loads(texto)
        return normalizar_entidade(data)

    partes = [parte.strip() for parte in texto.split(",") if parte.strip()]
    if len(partes) < 4:
        raise ValueError("payload em formato inválido")

    data: dict[str, Any] = {
        "id": partes[0],
        "tipo": partes[1],
        "latitude": float(partes[2]),
        "longitude": float(partes[3]),
    }
    if len(partes) >= 7:
        data["altitude_m"] = float(partes[4])
        data["velocidade_kmh"] = float(partes[5])
        data["curso_deg"] = float(partes[6])
    if len(partes) >= 8:
        data["rssi_dbm"] = float(partes[7])
    return normalizar_entidade(data)


def processar_entidade(payload: dict[str, Any]) -> None:
    veiculo = normalizar_entidade(payload)
    veiculo_id = veiculo["id"]

    global ULTIMO_PACOTE_TS
    with _lock:
        VEICULOS[veiculo_id] = veiculo
        ULTIMO_PACOTE_TS = time.time()
        adicionar_historico(veiculo_id, veiculo["latitude"], veiculo["longitude"])


def _ler_serial() -> None:
    global SERIAL_HANDLE
    if serial is None:
        return

    while not SERIAL_STOP_EVENT.is_set():
        porta_atual = None
        try:
            porta_atual = descobrir_porta_serial()
            SERIAL_PORT = porta_atual
            porta = serial.Serial(porta_atual, SERIAL_BAUD, timeout=1.0)
            SERIAL_HANDLE = porta
            while not SERIAL_STOP_EVENT.is_set():
                linha = porta.readline()
                if not linha:
                    continue
                texto = linha.decode("utf-8", errors="replace").strip()
                if not texto:
                    continue
                if linha_serial_deve_ser_ignorada(texto):
                    continue
                try:
                    payload = parse_payload(texto)
                    processar_entidade(payload)
                except Exception as exc:  # pragma: no cover - depende da fonte serial
                    app.logger.warning("Linha serial descartada: %s (%s)", texto, exc)
        except Exception as exc:  # pragma: no cover - depende do hardware real
            app.logger.warning("Não foi possível abrir a serial %s: %s", porta_atual or SERIAL_PORT, exc)
            if SERIAL_STOP_EVENT.is_set():
                break
            time.sleep(5)
        finally:
            fechar_serial()


atexit.register(fechar_serial)


@app.route("/")
def index():
    return render_template("dashboard.html")


@app.route("/api/veiculos")
def listar_veiculos():
    with _lock:
        agora = time.time()
        veiculos = []
        for veiculo in VEICULOS.values():
            veiculos.append({
                **veiculo,
                "segundos_sem_sinal": round(agora - veiculo["timestamp"], 1),
                "historico": HISTORICO.get(veiculo["id"], []),
            })
        return jsonify({"veiculos": veiculos})


@app.route("/api/health")
def healthcheck():
    desde_ultimo = None if ULTIMO_PACOTE_TS is None else round(time.time() - ULTIMO_PACOTE_TS, 1)
    return jsonify({
        "ok": True,
        "veiculos_ativos": len(VEICULOS),
        "porta_serial": SERIAL_PORT,
        "serial_conectada": SERIAL_HANDLE is not None,
        "segundos_desde_ultimo_pacote": desde_ultimo,
    })


if __name__ == "__main__":
    SERIAL_STOP_EVENT.clear()
    if serial is not None:
        _serial_thread = threading.Thread(target=_ler_serial, daemon=True)
        _serial_thread.start()
    try:
        app.run(host="0.0.0.0", port=5001, debug=False, use_reloader=False)
    finally:
        SERIAL_STOP_EVENT.set()
        fechar_serial()
