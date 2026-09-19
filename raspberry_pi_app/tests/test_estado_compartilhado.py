"""Testes do estado compartilhado — sem hardware, sem rede."""

import os
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from estado_compartilhado import EstadoCompartilhado


class TestEstadoCompartilhado(unittest.TestCase):
    def setUp(self):
        self.estado = EstadoCompartilhado()

    def test_reativar_som_desfaz_o_silencio(self):
        self.estado.silenciar_som("critico_confirmado")
        self.assertTrue(self.estado.som_silenciado("critico_confirmado"))
        self.estado.reativar_som()
        self.assertFalse(self.estado.som_silenciado("critico_confirmado"))

    def test_remover_entidade_existente(self):
        self.estado.atualizar_entidade("x1", "pessoa", 10, 90)
        self.assertIn("x1", self.estado.ler_entidades())
        self.estado.remover_entidade("x1")
        self.assertNotIn("x1", self.estado.ler_entidades())

    def test_remover_entidade_inexistente_nao_quebra(self):
        self.estado.remover_entidade("nao-existe")  # não deve levantar exceção
        self.assertEqual(self.estado.ler_entidades(), {})

    def test_remover_uma_entidade_mantem_as_outras(self):
        self.estado.atualizar_entidade("x1", "pessoa", 10, 90)
        self.estado.atualizar_entidade("x2", "trator", 50, 180)
        self.estado.remover_entidade("x1")
        entidades = self.estado.ler_entidades()
        self.assertNotIn("x1", entidades)
        self.assertIn("x2", entidades)

    def test_ler_entidades_sem_timeout_devolve_tudo_mesmo_antiga(self):
        self.estado.atualizar_entidade("x1", "pessoa", 10, 90)
        self.estado._entidades["x1"]["ultimo_update"] = time.time() - 999
        self.assertIn("x1", self.estado.ler_entidades())

    def test_ler_entidades_com_timeout_esconde_entidade_velha(self):
        self.estado.atualizar_entidade("x1", "pessoa", 10, 90)
        self.estado._entidades["x1"]["ultimo_update"] = time.time() - 999
        self.assertNotIn("x1", self.estado.ler_entidades(timeout_s=15))

    def test_ler_entidades_com_timeout_mantem_entidade_recente(self):
        self.estado.atualizar_entidade("x1", "pessoa", 10, 90)
        self.assertIn("x1", self.estado.ler_entidades(timeout_s=15))

    def test_modo_demo_comeca_desligado(self):
        self.assertFalse(self.estado.modo_demo())

    def test_modo_demo_liga_e_desliga(self):
        self.estado.definir_modo_demo(True)
        self.assertTrue(self.estado.modo_demo())
        self.estado.definir_modo_demo(False)
        self.assertFalse(self.estado.modo_demo())

    def test_escala_comeca_none_usa_padrao(self):
        self.assertIsNone(self.estado.escala_verde_m())

    def test_escala_customizada_e_resetada(self):
        self.estado.definir_escala(20)
        self.assertEqual(self.estado.escala_verde_m(), 20)
        self.estado.definir_escala(None)
        self.assertIsNone(self.estado.escala_verde_m())


if __name__ == "__main__":
    unittest.main()
