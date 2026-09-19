"""Recebe, por WiFi, as entidades (pessoa/trator) que o ESP32 do trator ouviu
por LoRa e repassa pro Pi. Chamado pela rota POST /api/entidade em app.py.

O ESP32 fala HTTP/JSON, não mais porta serial (decisão de arquitetura: não há
cabo entre o ESP32 do trator e o Pi — ver README.md, seção "Pendências").

Payload esperado (um POST por entidade):
    {"id": "a1b2c3", "tipo": "pessoa", "distancia_m": 34.2, "angulo_deg": 175.0, "fonte": "gps"}

"fonte" ("gps" ou "rssi") é opcional e só serve pra debug visual no dashboard
-- nunca entra em nenhuma lógica de alerta, só mostra de onde vem o número de
distância exibido (RSSI é bem menos preciso que GPS, útil saber qual dos
dois gerou uma leitura que parece estranha).
"""

TIPOS_VALIDOS = {"pessoa", "trator"}
FONTES_VALIDAS = {"gps", "rssi"}


def processar_entidade(payload, estado):
    """Valida o payload recebido do ESP32 e, se válido, atualiza o estado
    compartilhado. Retorna (ok: bool, erro: str|None) — não levanta exceção
    pra dado malformado, só recusa.
    """
    if not isinstance(payload, dict):
        return False, "corpo precisa ser um objeto JSON"

    entidade_id = payload.get("id")
    tipo = payload.get("tipo")
    distancia_m = payload.get("distancia_m")
    angulo_deg = payload.get("angulo_deg")
    fonte = payload.get("fonte")

    if not entidade_id or not isinstance(entidade_id, str):
        return False, "'id' é obrigatório e deve ser string"
    if tipo not in TIPOS_VALIDOS:
        return False, f"'tipo' deve ser um de {sorted(TIPOS_VALIDOS)}"
    if not isinstance(distancia_m, (int, float)) or distancia_m < 0:
        return False, "'distancia_m' é obrigatório e deve ser um número >= 0"
    if angulo_deg is not None and not isinstance(angulo_deg, (int, float)):
        return False, "'angulo_deg', se enviado, deve ser um número"
    if fonte is not None and fonte not in FONTES_VALIDAS:
        return False, f"'fonte', se enviado, deve ser um de {sorted(FONTES_VALIDAS)}"

    estado.atualizar_entidade(entidade_id, tipo, float(distancia_m), angulo_deg, fonte)
    return True, None
