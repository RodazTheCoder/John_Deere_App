from centralizador_de_dados.app import descobrir_porta_serial, fechar_serial, linha_serial_deve_ser_ignorada, parse_payload


def test_parse_json_payload():
    payload = '{"id":"tractor-01","tipo":"trator","latitude":-23.5505,"longitude":-46.6333,"timestamp":"2026-09-18T00:00:00Z"}'
    result = parse_payload(payload)
    assert result["id"] == "tractor-01"
    assert result["tipo"] == "trator"
    assert result["latitude"] == -23.5505
    assert result["longitude"] == -46.6333


def test_parse_csv_payload():
    payload = "tractor-01,trator,-23.5505,-46.6333,42.0,7.2,90.0,-60"
    result = parse_payload(payload)
    assert result["id"] == "tractor-01"
    assert result["tipo"] == "trator"
    assert result["latitude"] == -23.5505
    assert result["longitude"] == -46.6333
    assert result["rssi_dbm"] == -60


def test_linha_serial_deve_ser_ignorada_para_diagnostico_gps():
    assert linha_serial_deve_ser_ignorada("[GPS] chars processados=0 satelites=0 fix=NAO") is True
    assert linha_serial_deve_ser_ignorada("--Modo centralizador ativo: somente recepcao LoRa--") is True


def test_descobrir_porta_serial_fallback_para_porta_disponivel(monkeypatch):
    monkeypatch.setattr("centralizador_de_dados.app.serial", object())
    monkeypatch.setattr("centralizador_de_dados.app.os.getenv", lambda key, default=None: default)

    def fake_portas():
        return ["COM5", "COM3"]

    monkeypatch.setattr("centralizador_de_dados.app.listar_portas_disponiveis", lambda: fake_portas())
    assert descobrir_porta_serial() == "COM5"


def test_fechar_serial_zera_handle(monkeypatch):
    class FakePorta:
        def __init__(self):
            self.closed = False

        def close(self):
            self.closed = True

    fake_porta = FakePorta()
    monkeypatch.setattr("centralizador_de_dados.app.SERIAL_HANDLE", fake_porta)
    fechar_serial()
    assert fake_porta.closed is True
    assert __import__("centralizador_de_dados.app", fromlist=["SERIAL_HANDLE"]).SERIAL_HANDLE is None
