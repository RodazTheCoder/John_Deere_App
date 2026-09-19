import threading
import time


class EstadoCompartilhado:
    """Estado global compartilhado entre a thread de câmera e o recebimento de
    dados do ESP32, protegido por lock.

    A câmera não mede distância — só confirma "tem algo na área". Quem calcula
    distância é sempre o LoRa/GPS, publicado separadamente em `_entidades`.
    """

    def __init__(self):
        self._lock = threading.Lock()

        # câmera (deteccao/camera_worker.py)
        self._frame_jpeg = None
        self._deteccoes = []  # [{"x1", "y1", "x2", "y2", "conf"}, ...]
        self._camera_ultimo_heartbeat = None

        # LoRa / GPS, recebido via WiFi do ESP32 do trator (comunicacao/entidade_receiver.py)
        self._entidades = {}  # id -> {"tipo": "pessoa"|"trator", "distancia_m", "angulo_deg", "fonte", "ultimo_update"}
        self._esp_ultimo_heartbeat = None

        # Botão de silenciar/reconhecer do dashboard -- guarda PRA QUAL nível
        # o som foi silenciado, não só um booleano. Assim, quando o nível
        # mudar de verdade, o silêncio deixa de valer sozinho (comparação
        # simplesmente para de bater), sem precisar de nenhuma ação extra
        # pra "desmutar". O LED nunca é afetado por isso, só o som.
        self._som_silenciado_nivel = None

        # Modo demo: quando ativo, calcular_alerta só considera a entidade
        # falsa dos botões de demo (id ENTIDADE_DEMO_ID); quando inativo, só
        # considera entidades reais (do ESP32/LoRa de verdade). Nunca mistura
        # os dois -- ver app.py > _alerta_atual().
        self._modo_demo = False

        # Escala configurável dos limiares verde/amarelo (ver /api/escala em
        # app.py) -- None = usa o padrão de config.py (100m). Pensado pra
        # testar em ambientes menores (sala) sem precisar alcançar 100m de
        # verdade, mantendo a mesma proporção 1:2 entre atenção e crítico.
        self._escala_verde_m = None

    # ---- câmera ----
    def atualizar_frame(self, frame_jpeg, deteccoes):
        with self._lock:
            self._frame_jpeg = frame_jpeg
            self._deteccoes = deteccoes
            self._camera_ultimo_heartbeat = time.time()

    def ler_frame(self):
        with self._lock:
            return self._frame_jpeg

    def ler_deteccoes(self):
        with self._lock:
            return list(self._deteccoes)

    def camera_online(self, timeout_s):
        with self._lock:
            if self._camera_ultimo_heartbeat is None:
                return False
            return (time.time() - self._camera_ultimo_heartbeat) <= timeout_s

    # ---- ESP32 / LoRa ----
    def atualizar_entidade(self, entidade_id, tipo, distancia_m, angulo_deg=None, fonte=None):
        with self._lock:
            self._entidades[entidade_id] = {
                "tipo": tipo,
                "distancia_m": distancia_m,
                "angulo_deg": angulo_deg,
                "fonte": fonte,  # "gps" ou "rssi" -- só debug visual, ver comunicacao/entidade_receiver.py
                "ultimo_update": time.time(),
            }
            self._esp_ultimo_heartbeat = time.time()

    def ler_entidades(self, timeout_s=None):
        """Sem `timeout_s`, devolve tudo (inclusive entidades velhas) --
        usado nos testes. Com `timeout_s`, filtra entidades que não
        atualizam há mais tempo que isso (ex: tag desligada) -- é o que
        `app.py` usa de verdade, pra uma tag desconectada não ficar
        "grudada" no último valor pra sempre."""
        with self._lock:
            if timeout_s is None:
                return dict(self._entidades)
            agora = time.time()
            return {
                eid: dados for eid, dados in self._entidades.items()
                if (agora - dados["ultimo_update"]) <= timeout_s
            }

    def remover_entidade(self, entidade_id):
        with self._lock:
            self._entidades.pop(entidade_id, None)

    def esp_online(self, timeout_s):
        with self._lock:
            if self._esp_ultimo_heartbeat is None:
                return False
            return (time.time() - self._esp_ultimo_heartbeat) <= timeout_s

    # ---- silenciar/reconhecer ----
    def silenciar_som(self, nivel):
        with self._lock:
            self._som_silenciado_nivel = nivel

    def som_silenciado(self, nivel):
        with self._lock:
            return self._som_silenciado_nivel == nivel

    # ---- modo demo ----
    def definir_modo_demo(self, ativo):
        with self._lock:
            self._modo_demo = bool(ativo)

    def modo_demo(self):
        with self._lock:
            return self._modo_demo

    # ---- escala configurável ----
    def definir_escala(self, verde_m):
        with self._lock:
            self._escala_verde_m = verde_m  # None reseta pro padrão de config.py

    def escala_verde_m(self):
        with self._lock:
            return self._escala_verde_m


estado = EstadoCompartilhado()
