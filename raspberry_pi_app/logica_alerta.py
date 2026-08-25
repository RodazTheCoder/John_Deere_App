"""Tabela de verdade do sistema: cruza distância (LoRa) com detecção da câmera
para decidir o nível de alerta.

A câmera não mede distância — só confirma "tem alguém na área". O casamento
com uma entidade LoRa é por presença/ausência dentro do alcance da câmera, não
por correlação fina de ângulo (descartada — depende de bússola + GPS preciso,
pouco confiáveis sob dossel florestal).

Simplificação assumida: só considera a entidade LoRa mais próxima por vez —
não há rastreamento multi-entidade (dois tratores próximos ao mesmo tempo).
"""

import config

SEGURO = "seguro"
ATENCAO = "atencao"
CRITICO_PENDENTE = "critico_pendente"
CRITICO_CONFIRMADO = "critico_confirmado"
NAO_IDENTIFICADO = "nao_identificado"
CAMERA_OFFLINE = "camera_offline"


def _entidade_mais_proxima(entidades):
    if not entidades:
        return None
    return min(entidades.values(), key=lambda e: e["distancia_m"])


def calcular_alerta(entidades, deteccoes, camera_online):
    """
    entidades: dict id -> {"tipo", "distancia_m", "angulo_deg", ...} (estado.ler_entidades())
    deteccoes: lista de caixas da câmera (estado.ler_deteccoes())
    camera_online: bool (estado.camera_online())

    Retorna um dict pronto pro dashboard consumir: nivel, distancia_m,
    tipo_entidade, led (cor + piscando), som (estado) e mensagem pro tablet.
    """
    if not camera_online:
        return {
            "nivel": CAMERA_OFFLINE,
            "distancia_m": None,
            "tipo_entidade": None,
            "led": {"cor": "vermelho", "piscando": True},
            "som": {"estado": "off"},
            "mensagem": "CÂMERA OFFLINE — sem sinal do Raspberry Pi",
        }

    camera_detectando = len(deteccoes) > 0
    entidade = _entidade_mais_proxima(entidades)

    # "Detectar já significa estar perto": câmera vendo alguém sem nenhum
    # registro no LoRa (nenhuma entidade, de forma alguma) é o caso mais
    # grave. Não há correlação fina por ângulo — se existe QUALQUER entidade
    # registrada, a detecção é tratada como confirmação dela (ver caso do
    # trator a 60m abaixo: uma entidade distante ainda "explica" a detecção).
    if entidade is None:
        if camera_detectando:
            return {
                "nivel": NAO_IDENTIFICADO,
                "distancia_m": None,
                "tipo_entidade": None,
                "led": {"cor": "vermelho", "piscando": False},
                "som": {"estado": "urgente"},
                "mensagem": "NÃO IDENTIFICADO — AÇÃO IMEDIATA",
            }
        return {
            "nivel": SEGURO,
            "distancia_m": None,
            "tipo_entidade": None,
            "led": {"cor": "verde", "piscando": False},
            "som": {"estado": "off"},
            "mensagem": "Radar normal — ninguém em risco",
        }

    dist = entidade["distancia_m"]
    tipo = entidade["tipo"]

    if dist > config.DISTANCIA_VERDE_M:
        return {
            "nivel": SEGURO,
            "distancia_m": dist,
            "tipo_entidade": tipo,
            "led": {"cor": "verde", "piscando": False},
            "som": {"estado": "off"},
            "mensagem": "Radar normal — ninguém em risco",
        }

    if dist > config.DISTANCIA_AMARELO_M:
        # fora do alcance realista da câmera — uma confirmação visual aqui
        # não existe e não mudaria o nível mesmo que existisse (ver caso do
        # trator confirmado a 60m no contexto do projeto).
        return {
            "nivel": ATENCAO,
            "distancia_m": dist,
            "tipo_entidade": tipo,
            "led": {"cor": "amarelo", "piscando": False},
            "som": {"estado": "espacado"},
            "mensagem": f"Alerta a {dist:.0f} m — sem confirmação (fora do alcance)",
        }

    # dist <= ALCANCE_CAMERA_M: zona vermelha
    if camera_detectando:
        return {
            "nivel": CRITICO_CONFIRMADO,
            "distancia_m": dist,
            "tipo_entidade": tipo,
            "led": {"cor": "vermelho", "piscando": False},
            "som": {"estado": "continuo"},
            "mensagem": f"Confirmado visualmente a {dist:.0f} m",
        }

    return {
        "nivel": CRITICO_PENDENTE,
        "distancia_m": dist,
        "tipo_entidade": tipo,
        "led": {"cor": "vermelho", "piscando": True},
        "som": {"estado": "continuo"},
        "mensagem": f"Risco crítico a {dist:.0f} m — aguardando confirmação visual",
    }
