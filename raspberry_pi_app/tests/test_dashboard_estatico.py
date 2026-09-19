"""Garante que o dashboard modular esta inteiro: todo arquivo estatico e todo
partial referenciado existe, e todo id usado pelo JavaScript existe no HTML.
Nao depende de rede, Flask nem hardware. Rodar com:
python -m unittest tests.test_dashboard_estatico (a partir de raspberry_pi_app/).
"""

import os
import re
import unittest

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEMPLATES = os.path.join(BASE, "templates")
STATIC = os.path.join(BASE, "static")


def _ler(caminho):
    with open(caminho, encoding="utf-8") as f:
        return f.read()


def _html_completo():
    """dashboard.html com os {% include %} expandidos (so um nivel, como no projeto)."""
    html = _ler(os.path.join(TEMPLATES, "dashboard.html"))
    def expandir(m):
        return _ler(os.path.join(TEMPLATES, m.group(1)))
    return re.sub(r"\{%\s*include\s+'([^']+)'\s*%\}", expandir, html)


class TestDashboardEstatico(unittest.TestCase):
    def test_arquivos_estaticos_referenciados_existem(self):
        html = _ler(os.path.join(TEMPLATES, "dashboard.html"))
        refs = re.findall(r'(?:href|src)="/static/([^"]+)"', html)
        self.assertGreater(len(refs), 0)
        for ref in refs:
            self.assertTrue(os.path.isfile(os.path.join(STATIC, ref)), f"faltando static/{ref}")

    def test_partials_incluidos_existem(self):
        html = _ler(os.path.join(TEMPLATES, "dashboard.html"))
        includes = re.findall(r"\{%\s*include\s+'([^']+)'\s*%\}", html)
        self.assertGreater(len(includes), 0)
        for inc in includes:
            self.assertTrue(os.path.isfile(os.path.join(TEMPLATES, inc)), f"faltando templates/{inc}")

    def test_ids_usados_no_js_existem_no_html(self):
        html = _html_completo()
        ids_html = set(re.findall(r'id="([^"]+)"', html))
        js_dir = os.path.join(STATIC, "js")
        for nome in sorted(os.listdir(js_dir)):
            codigo = _ler(os.path.join(js_dir, nome))
            for id_ in re.findall(r"getElementById\('([^']+)'\)", codigo):
                self.assertIn(id_, ids_html, f"{nome}: id '{id_}' nao existe no HTML")


if __name__ == "__main__":
    unittest.main()
