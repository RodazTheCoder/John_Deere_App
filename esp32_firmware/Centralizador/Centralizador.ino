// ESP32 centralizador: so escuta LoRa e repassa cada pacote cru pela serial USB
// para o centralizador_de_dados/app.py. Nao transmite, nao usa GPS, WiFi nem LED.
//
// Formato do pacote (mesmo de ProjetoLoRa1.ino): id,tipo,lat,lon,alt,vel,curso
//
// ATENCAO: os parametros de radio abaixo DEVEM ser iguais aos de
// ../ProjetoLoRa1/ProjetoLoRa1.ino (frequencia, sync word, SF, BW, CR, CRC).
// Se divergirem, os modulos deixam de se ouvir sem nenhum erro visivel.

#include <SPI.h>
#include <LoRa.h>

#define SS 5
#define RST 21
#define DIO0 4

#define SERIAL_BAUD 9600 // mesmo valor de CENTRALIZADOR_BAUD em centralizador_de_dados/app.py

void setup() {
  Serial.begin(SERIAL_BAUD);

  LoRa.setPins(SS, RST, DIO0);
  while (!LoRa.begin(915E6)) {
    delay(500);
  }
  LoRa.setSyncWord(0x34);
  LoRa.setSpreadingFactor(9);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(8);
  LoRa.enableCrc();
}

void loop() {
  if (LoRa.parsePacket()) {
    String mensagem = "";
    while (LoRa.available()) {
      mensagem += (char)LoRa.read();
    }
    if (mensagem.length() > 0) {
      Serial.println(mensagem);
    }
  }
}
