import os
import sys

try:
    import serial
except ImportError:
    print("PySerial não está instalado.")
    print("Instale com: py -m pip install pyserial")
    sys.exit(1)

PORTA = os.getenv("CENTRALIZADOR_PORT", "COM3")


def fechar_porta():
    try:
        porta = serial.Serial(PORTA, timeout=1)
        porta.close()
        print(f"Porta {PORTA} fechada com sucesso.")
        return True
    except Exception as exc:
        print(f"Não foi possível fechar {PORTA}: {exc}")
        return False


if __name__ == "__main__":
    fechar_porta()
