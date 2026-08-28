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


def calcular_alerta(entidades, deteccoes, camera_online, distancia_verde_m=None, distancia_amarelo_m=None):
    """
    entidades: dict id -> {"tipo", "distancia_m", "angulo_deg", ...} (estado.ler_entidades())
    deteccoes: lista de caixas da câmera (estado.ler_deteccoes())
    camera_online: bool (estado.camera_online())
    distancia_verde_m / distancia_amarelo_m: limiares a usar -- por padrão
    (None) usa os valores fixos de config.py, mas o Pi permite reconfigurar
    isso em tempo real (ver /api/escala em app.py), útil pra testar em
    ambientes menores (sala) sem precisar alcançar 100m de verdade.

    Retorna um dict pronto pro dashboard consumir: nivel, distancia_m,
    tipo_entidade, led (cor + piscando), som (estado) e mensagem pro tablet.
    """
    if distancia_verde_m is None:
        distancia_verde_m = config.DISTANCIA_VERDE_M
    if distancia_amarelo_m is None:
        distancia_amarelo_m = config.DISTANCIA_AMARELO_M
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

    # Detecção visual é a evidência mais forte que existe -- a câmera só
    # enxerga dentro do alcance real dela (ALCANCE_CAMERA_M), então
    # "detectando" já significa perto de verdade, mesmo que a distância
    # calculada da entidade LoRa diga o contrário (RSSI impreciso, ou pode
    # até ser outra pessoa não rastreada que entrou na área, não
    # necessariamente a entidade registrada). Checado antes dos limiares de
    # distância de propósito -- confirmação visual nunca deve ser ignorada.
    if camera_detectando:
        return {
            "nivel": CRITICO_CONFIRMADO,
            "distancia_m": dist,
            "tipo_entidade": tipo,
            "led": {"cor": "vermelho", "piscando": False},
            "som": {"estado": "continuo"},
            "mensagem": f"Confirmado visualmente ({dist:.0f} m reportado pela entidade)",
        }

    if dist > distancia_verde_m:
        return {
            "nivel": SEGURO,
            "distancia_m": dist,
            "tipo_entidade": tipo,
            "led": {"cor": "verde", "piscando": False},
            "som": {"estado": "off"},
            "mensagem": "Radar normal — ninguém em risco",
        }

    if dist > distancia_amarelo_m:
        return {
            "nivel": ATENCAO,
            "distancia_m": dist,
            "tipo_entidade": tipo,
            "led": {"cor": "amarelo", "piscando": False},
            "som": {"estado": "espacado"},
            "mensagem": f"Alerta a {dist:.0f} m — sem confirmação (fora do alcance)",
        }

    return {
        "nivel": CRITICO_PENDENTE,
        "distancia_m": dist,
        "tipo_entidade": tipo,
        "led": {"cor": "vermelho", "piscando": True},
        "som": {"estado": "continuo"},
        "mensagem": f"Risco crítico a {dist:.0f} m — aguardando confirmação visual",
    }
