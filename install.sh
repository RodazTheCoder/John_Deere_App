#!/usr/bin/env bash
# Instala as dependências do raspberry_pi_app um pacote por vez.
#
# Rodar tudo de uma vez (pip install -r requirements.txt) faz o resolvedor de
# dependências do pip tentar casar torch + opencv + ultralytics ao mesmo
# tempo, o que consome bastante RAM -- no Raspberry Pi (pouca RAM, sem swap
# grande por padrão) isso trava ou mata o processo (erro "Killed") no meio da
# instalação. Instalando um pacote por vez, cada passo usa bem menos memória.
#
# Uso: ative a venv antes e rode "./install.sh" (ou "bash install.sh").
set -e

echo "Instalando torch (CPU-only)..."
pip install --no-cache-dir torch --index-url https://download.pytorch.org/whl/cpu

echo "Instalando torchvision (CPU-only)..."
pip install --no-cache-dir torchvision --index-url https://download.pytorch.org/whl/cpu

echo "Instalando flask..."
pip install --no-cache-dir flask

echo "Instalando opencv-python..."
pip install --no-cache-dir opencv-python

echo "Instalando ultralytics..."
pip install --no-cache-dir ultralytics

echo "Pronto. Se algum passo travar ou aparecer 'Killed', rode so aquele
comando de novo (com --no-cache-dir), ou aumente o swap do Pi antes de tentar
outra vez (ver README.md, secao 'Setup local')."
