"""Testes de validação do payload recebido do ESP32 — não depende de rede,
Flask nem hardware. Rodar com: python -m unittest tests.test_entidade_receiver
(a partir de raspberry_pi_app/).
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from comunicacao.entidade_receiver import processar_entidade
from estado_compartilhado import EstadoCompartilhado


class TestEntidadeReceiver(unittest.TestCase):
    def setUp(self):
        self.estado = EstadoCompartilhado()

    def test_payload_valido_atualiza_estado(self):
        ok, erro = processar_entidade(
            {"id": "a1b2c3", "tipo": "pessoa", "distancia_m": 34.2, "angulo_deg": 175.0},
            self.estado,
        )
        self.assertTrue(ok)
        self.assertIsNone(erro)
        entidades = self.estado.ler_entidades()
        self.assertEqual(entidades["a1b2c3"]["distancia_m"], 34.2)
        self.assertEqual(entidades["a1b2c3"]["tipo"], "pessoa")
        self.assertTrue(self.estado.esp_online(5))

    def test_payload_valido_sem_angulo_e_aceito(self):
        ok, erro = processar_entidade(
            {"id": "a1b2c3", "tipo": "trator", "distancia_m": 10},
            self.estado,
        )
        self.assertTrue(ok)
        self.assertIsNone(erro)

    def test_corpo_nao_e_objeto_json_e_rejeitado(self):
        ok, erro = processar_entidade(None, self.estado)
        self.assertFalse(ok)
        self.assertIsNotNone(erro)

    def test_id_ausente_e_rejeitado(self):
        ok, erro = processar_entidade({"tipo": "pessoa", "distancia_m": 5}, self.estado)
        self.assertFalse(ok)

    def test_tipo_invalido_e_rejeitado(self):
        ok, erro = processar_entidade(
            {"id": "x1", "tipo": "cachorro", "distancia_m": 5}, self.estado
        )
        self.assertFalse(ok)

    def test_distancia_negativa_e_rejeitada(self):
        ok, erro = processar_entidade(
            {"id": "x1", "tipo": "pessoa", "distancia_m": -1}, self.estado
        )
        self.assertFalse(ok)

    def test_distancia_ausente_e_rejeitada(self):
        ok, erro = processar_entidade({"id": "x1", "tipo": "pessoa"}, self.estado)
        self.assertFalse(ok)

    def test_payload_invalido_nao_atualiza_estado(self):
        processar_entidade({"id": "x1", "tipo": "invalido", "distancia_m": 5}, self.estado)
        self.assertEqual(self.estado.ler_entidades(), {})


if __name__ == "__main__":
    unittest.main()
