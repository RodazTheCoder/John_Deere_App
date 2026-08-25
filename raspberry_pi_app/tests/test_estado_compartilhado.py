"""Testes do estado compartilhado — sem hardware, sem rede."""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from estado_compartilhado import EstadoCompartilhado


class TestEstadoCompartilhado(unittest.TestCase):
    def setUp(self):
        self.estado = EstadoCompartilhado()

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


if __name__ == "__main__":
    unittest.main()
