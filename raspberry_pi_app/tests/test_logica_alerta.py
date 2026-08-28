"""Testes da tabela de verdade com dados falsos — não depende do ESP32/LoRa
existir de verdade. Rodar com: python -m unittest tests.test_logica_alerta
(a partir de raspberry_pi_app/).
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from logica_alerta import (
    calcular_alerta,
    SEGURO,
    ATENCAO,
    CRITICO_PENDENTE,
    CRITICO_CONFIRMADO,
    NAO_IDENTIFICADO,
    CAMERA_OFFLINE,
)


def entidade(distancia_m, tipo="pessoa", angulo_deg=0):
    return {"tipo": tipo, "distancia_m": distancia_m, "angulo_deg": angulo_deg}


def deteccao():
    return [{"x1": 0, "y1": 0, "x2": 10, "y2": 10, "conf": 0.9}]


class TestTabelaDeVerdade(unittest.TestCase):
    def test_nada_no_alcance_e_camera_quieta_fica_seguro(self):
        r = calcular_alerta({}, [], camera_online=True)
        self.assertEqual(r["nivel"], SEGURO)

    def test_acima_de_100m_fica_seguro(self):
        r = calcular_alerta({"p1": entidade(142)}, [], camera_online=True)
        self.assertEqual(r["nivel"], SEGURO)

    def test_entre_50_e_100m_fica_atencao(self):
        r = calcular_alerta({"p1": entidade(63)}, [], camera_online=True)
        self.assertEqual(r["nivel"], ATENCAO)
        self.assertEqual(r["som"]["estado"], "espacado")

    def test_abaixo_de_50m_sem_deteccao_fica_critico_pendente_piscando(self):
        r = calcular_alerta({"p1": entidade(34)}, [], camera_online=True)
        self.assertEqual(r["nivel"], CRITICO_PENDENTE)
        self.assertTrue(r["led"]["piscando"])

    def test_abaixo_de_50m_com_deteccao_fica_critico_confirmado_solido(self):
        r = calcular_alerta({"p1": entidade(27)}, deteccao(), camera_online=True)
        self.assertEqual(r["nivel"], CRITICO_CONFIRMADO)
        self.assertFalse(r["led"]["piscando"])

    def test_camera_detecta_sem_entidade_nenhuma_fica_nao_identificado(self):
        r = calcular_alerta({}, deteccao(), camera_online=True)
        self.assertEqual(r["nivel"], NAO_IDENTIFICADO)

    def test_camera_detecta_e_ha_entidade_registrada_mesmo_que_distante_nao_fica_nao_identificado(self):
        # limitação conhecida e aceita: sem correlação por ângulo, qualquer
        # entidade registrada no LoRa "explica" a detecção da câmera, mesmo
        # estando bem mais longe do que o alcance real da câmera.
        r = calcular_alerta({"p1": entidade(63)}, deteccao(), camera_online=True)
        self.assertEqual(r["nivel"], ATENCAO)

    def test_trator_confirmado_a_60m_nao_vira_vermelho(self):
        # caso discutido: confirmação visual não aumenta o risco por si só
        r = calcular_alerta({"t1": entidade(60, tipo="trator")}, deteccao(), camera_online=True)
        self.assertEqual(r["nivel"], ATENCAO)

    def test_camera_offline_e_sinalizada_explicitamente(self):
        r = calcular_alerta({"p1": entidade(34)}, [], camera_online=False)
        self.assertEqual(r["nivel"], CAMERA_OFFLINE)

    def test_entidade_mais_proxima_e_a_priorizada(self):
        entidades = {"p1": entidade(63), "p2": entidade(20)}
        r = calcular_alerta(entidades, [], camera_online=True)
        self.assertEqual(r["distancia_m"], 20)
        self.assertEqual(r["nivel"], CRITICO_PENDENTE)

    def test_escala_customizada_muda_classificacao(self):
        # 15m com os limiares padrão (50/100) é crítico; com uma escala menor
        # (10/20, pra testar em sala) os mesmos 15m viram só atenção.
        padrao = calcular_alerta({"p1": entidade(15)}, [], camera_online=True)
        self.assertEqual(padrao["nivel"], CRITICO_PENDENTE)

        escala_sala = calcular_alerta(
            {"p1": entidade(15)}, [], camera_online=True,
            distancia_verde_m=20, distancia_amarelo_m=10,
        )
        self.assertEqual(escala_sala["nivel"], ATENCAO)


if __name__ == "__main__":
    unittest.main()
