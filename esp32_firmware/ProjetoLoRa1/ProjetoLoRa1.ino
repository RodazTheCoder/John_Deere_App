/*
 * Nó de rastreamento (pessoa ou trator) — GPS + LoRa + WiFi opcional.
 *
 * O MESMO código roda em qualquer ESP32 do projeto (tag de pessoa, tag de
 * outro trator, ou o trator que carrega o Raspberry Pi). Cada dispositivo:
 *   - lê sua própria posição via GPS;
 *   - transmite periodicamente por LoRa: id, tipo, posição, curso;
 *   - escuta o que os outros nós transmitem e calcula distância/direção
 *     até eles.
 *
 * Só o ESP32 que fica junto do Raspberry Pi (o do trator com câmera) precisa
 * também repassar pro Pi, via WiFi, tudo que ouve por LoRa — inclusive de
 * nós que não têm Raspberry nenhum (tags de pessoa, por exemplo). É assim
 * que, tendo só 1 Raspberry Pi, qualquer módulo GPS+LoRa aparece certo na
 * rosa dos ventos do único painel que existe. Ver TRATOR_COM_PI abaixo.
 *
 * Ver README.md do repositório (raiz do projeto) pra contexto completo do
 * sistema, a tabela de verdade e as decisões de arquitetura já tomadas.
 */

#include <SPI.h>
#include <LoRa.h>
#include <TinyGPSPlus.h>
#include <HardwareSerial.h>
#include <Preferences.h>
#include <esp_system.h>
#include <cmath>

// ============== CONFIGURAÇÃO POR DISPOSITIVO (editar antes de gravar) ==============

// "pessoa" ou "trator" — decide o ícone que aparece na rosa dos ventos do dashboard.
#define TIPO_ENTIDADE "trator"

// 1 SÓ no ESP32 fisicamente junto do Raspberry Pi. 0 em todos os outros
// (tags de pessoa, outros tratores sem Pi). Usar 0/1 aqui, não true/false —
// o pré-processador do Arduino nem sempre entende bool em #if.
#define TRATOR_COM_PI 1

#if TRATOR_COM_PI
  #include <WiFi.h>
  #include <HTTPClient.h>

  // Rede criada pelo próprio Raspberry Pi (é ele que sobe o ponto de acesso —
  // no meio do mato não tem roteador nenhum pra conectar os dois). Ver
  // docs/configurar_pi_como_ap.md.
  const char* WIFI_SSID = "JohnDeere-Trator";
  const char* WIFI_SENHA = "12345678";
  const char* PI_URL = "http://10.42.0.1:5050/api/entidade"; // IP do hotspot criado pelo nmcli no Pi (trixie)
  const char* PI_ALERTA_URL = "http://10.42.0.1:5050/api/alerta-fisico";

  // Consulta o alerta já cruzado (câmera + LoRa) que o Pi calculou, pra
  // fazer o LED/buzzer físico bater exatamente com o semáforo do dashboard
  // -- não só a distância crua. Se o Pi não responder por tempo demais
  // (ALERTA_PI_TIMEOUT_MS), o LED cai sozinho de volta pro fallback local
  // (atualizarAlertaFisico, só distância) -- nunca fica sem alerta nenhum
  // só porque o Pi caiu ou o WiFi oscilou.
  // consultarAlertaPi() é uma chamada HTTP bloqueante -- enquanto ela roda,
  // o loop() não chama LoRa.parsePacket(), então qualquer pacote LoRa que
  // chegar nesse meio tempo é perdido. Com INTERVALO_CONSULTA_ALERTA_MS
  // curto (1s) e timeout HTTP alto, isso ficava roubando boa parte do tempo
  // de escuta do trator (testado: só ele parava de receber da tag, a tag
  // recebia normal, porque só ele tem essa chamada). 2s de intervalo ainda é
  // responsivo pro LED físico e sobra bem mais tempo de escuta.
  const unsigned long INTERVALO_CONSULTA_ALERTA_MS = 2000;
  const unsigned long ALERTA_PI_TIMEOUT_MS = 5000;

  unsigned long ultimoAlertaPiOk = 0;
  // char[] fixo em vez de String de propósito -- essas variáveis são escritas
  // pela tarefa de consulta ao Pi (core 0) e lidas pelo loop() principal
  // (core 1, ver xTaskCreatePinnedToCore em setup()). String faz alocação
  // dinâmica na hora de copiar, que não é seguro dentro da seção crítica que
  // protege essa troca entre núcleos; char[] com strncpy não aloca nada.
  char corAlertaPi[12] = "";
  bool piscandoAlertaPi = false;
  char somAlertaPi[12] = "";
  portMUX_TYPE alertaPiMux = portMUX_INITIALIZER_UNLOCKED;

  // O hotspot do Pi às vezes recusa a primeira tentativa de associação (bug
  // conhecido do driver WiFi onboard do Pi, não tem fix definitivo do lado
  // dele) -- sem isso aqui, se o boot falhasse e nenhum pacote LoRa chegasse
  // pra disparar o retry de enviarEntidadeProPi(), o trator ficava sem WiFi
  // pra sempre até alguém resetar a placa manualmente. WiFi.begin() é
  // assíncrono, então isso não trava o LED/buzzer enquanto tenta.
  const unsigned long INTERVALO_RETRY_WIFI_MS = 10000;
  unsigned long ultimaTentativaWiFi = 0;
#endif

// =====================================================================================

TinyGPSPlus gps;
HardwareSerial gpsSerial(2);
Preferences preferencias;

#define SS 5
#define RST 21
#define DIO0 4

#define RXD2 16
#define TXD2 17
#define GPS_BAUD 9600

// Pinos do LED/buzzer físico -- livres, sem conflito com LoRa/GPS (4, 5, 16,
// 17, 18, 19, 21, 23) nem com os pinos reservados do ESP32 (flash: 6-11;
// strapping/boot: 0, 2, 12, 15; só entrada: 34-39).
#define LED_VERDE_PIN 14
#define LED_AMARELO_PIN 27
#define LED_VERMELHO_PIN 26
#define BUZZER_PIN 13

const double RAIO_TERRA_M = 6371000.0;

// LORA_RSSI_1M calibrado de verdade com as duas placas a exatos 1m (testado
// em campo: -54, -54, -58 dBm com SF9, média ~-55). Esse valor é específico
// do SF atual (RSSI reportado pelo chip varia com o spreading factor) --
// recalibrar se o SF mudar de novo. EXPOENTE_PERDA_AMBIENTE continua chute
// (só temos 1 ponto de medição) -- ainda TODO CALIBRAR EM CAMPO com mais
// distâncias conhecidas (5m, 20m, 50m) pra ajustar a curva de verdade.
const int LORA_RSSI_1M = -55;
const float EXPOENTE_PERDA_AMBIENTE = 2.0;

// RSSI varia bastante entre leituras mesmo na mesma posição física (reflexo,
// orientação da antena, ruído de rádio) -- sem suavizar, a distância mostrada
// no dashboard "pula" de um jeito que parece bug mas é só ruído normal de
// sinal. Média móvel exponencial: quanto menor ALPHA, mais suave (mas mais
// devagar pra reagir a uma mudança real de distância); quanto maior, mais
// responsivo (mas mais ruidoso). Simplificação aceita: um cache global só,
// não por entidade -- coerente com o projeto já assumir "uma entidade
// prioritária por vez" em outros lugares.
const float RSSI_ALPHA_SUAVIZACAO = 0.35;
float distanciaRSSISuavizada = -1;

// Limiares do LED/buzzer FÍSICO (camada de segurança local, só distância --
// o ESP32 não tem câmera, então não dá pra diferenciar "pendente" de
// "confirmado" aqui, fica sempre piscando no vermelho por segurança).
// Começam batendo com o padrão de raspberry_pi_app/config.py (100m/50m),
// mas NÃO são mais fixos: se TRATOR_COM_PI=1, toda vez que o Pi responde
// /api/alerta-fisico com sucesso, esses dois valores são atualizados com a
// escala configurada de lá (ver /api/escala, "config.py > DISTANCIA_..."
// não é mais a fonte única). Isso é de propósito -- se o Pi cair no meio de
// um teste em escala menor (sala), o fallback local usa a ÚLTIMA escala
// confirmada em vez de voltar pro padrão de fábrica sem avisar ninguém.
float DISTANCIA_VERDE_M = 100.0;
float DISTANCIA_AMARELO_M = 50.0;

const unsigned long BLINK_INTERVALO_MS = 500;
const unsigned long BEEP_ESPACADO_INTERVALO_MS = 5000;
const unsigned long BEEP_ESPACADO_DURACAO_MS = 150;

unsigned long ultimoToggleLed = 0;
bool ledVermelhoAceso = false;
float distanciaMaisRecenteM = -1; // -1 = nenhum dado recebido ainda (fica tudo apagado)

unsigned long ultimoEnvio = 0;
const unsigned long INTERVALO_JANELA_MS = 3000;
// Variação aleatória somada ao intervalo acima (ver proximoIntervaloEnvioMs).
// SF12 deixa cada pacote no ar por ~1-1.5s -- se os dois nós ligarem/resetarem
// perto um do outro no tempo, ficam "em fase" e cada transmissão bate exatamente
// em cima da anterior, colidindo sempre (não por acaso, de forma sistemática --
// CRC falha e o pacote é descartado em silêncio). Sem esse jitter, dois nós que
// nascem em fase nunca mais se dessincronizam sozinhos. Achado testando com
// esboços isolados de TX-só e RX-só, que trocavam pacote perfeitamente.
const unsigned long JITTER_ENVIO_MAX_MS = 700;
unsigned long proximoIntervaloEnvioMs = INTERVALO_JANELA_MS;

unsigned long ultimoDiagnosticoGPS = 0;
const unsigned long INTERVALO_DIAGNOSTICO_GPS_MS = 2000;

String meuId;

void setup() {
  Serial.begin(115200);

  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, RXD2, TXD2);

  configurarAlertaFisico();

  meuId = obterOuCriarId();
  Serial.print("--ID deste no: ");
  Serial.println(meuId);
  Serial.println("--Tipo: " TIPO_ENTIDADE);

  randomSeed(esp_random());

  LoRa.setPins(SS, RST, DIO0);
  while (!LoRa.begin(915E6)) {
    Serial.println("--LoRa nao funcionando--");
    delay(500);
  }
  LoRa.setSyncWord(0x34);
  // SF12 (era o valor anterior) deixava cada pacote ~3.3s no ar com o
  // payload atual (~50 bytes) -- quase o intervalo inteiro entre
  // transmissões (3s + jitter), fazendo as duas placas colidirem quase
  // sempre, mesmo com jitter (testado: só ~10-15% dos pacotes chegavam).
  // SF9 reduz isso pra ~0.5s no ar (bem menos alcance que SF12, mas ainda
  // considerável), deixando bastante folga na janela pro jitter funcionar
  // de verdade. Se precisar de mais alcance de novo (mata fechada, testado
  // fora de ambiente urbano), subir o SF exige também aumentar
  // INTERVALO_JANELA_MS proporcionalmente, senão a colisão volta.
  LoRa.setSpreadingFactor(9);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(8);
  LoRa.enableCrc();
  // Potência de transmissão reduzida (padrão da biblioteca é ~17dBm, no
  // máximo) -- testado indoor: na potência máxima, o sinal chega "estourado"
  // tanto perto quanto a alguns metros, e o RSSI vira reflexo de parede
  // (multipath) em vez de refletir distância. Com menos potência, o sinal
  // fica mais perto do limite de detecção e volta a variar de verdade com a
  // distância real. Se for pra alcance de campo aberto de novo, subir isso
  // (até uns 20 no pino PA_BOOST).
  LoRa.setTxPower(5);
  Serial.println("--LoRa configurado--");

#if TRATOR_COM_PI
  conectarWiFi();
  // Core 0 -- deixa o core 1 (loop() padrão do Arduino) livre pra escutar o
  // LoRa sem ser interrompido pela espera bloqueante do HTTPClient. Ver
  // comentário grande em cima de consultarAlertaPi().
  xTaskCreatePinnedToCore(tarefaConsultaAlertaPi, "consultaAlertaPi", 8192, NULL, 1, NULL, 0);
#endif
}

void configurarAlertaFisico() {
  pinMode(LED_VERDE_PIN, OUTPUT);
  pinMode(LED_AMARELO_PIN, OUTPUT);
  pinMode(LED_VERMELHO_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(LED_VERDE_PIN, LOW);
  digitalWrite(LED_AMARELO_PIN, LOW);
  digitalWrite(LED_VERMELHO_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);
}

// Chamada todo loop() (não só quando chega pacote nova) -- precisa rodar
// sempre pra animação de piscar/bipe funcionar sem travar o resto do
// programa com delay(). Se BUZZER_PIN estiver ligado a um buzzer PASSIVO (que
// precisa de um tom, não só liga/desliga), trocar os digitalWrite(BUZZER_PIN,
// HIGH/LOW) abaixo por tone(BUZZER_PIN, 2000)/noTone(BUZZER_PIN).
void atualizarAlertaFisico(float distanciaM) {
  digitalWrite(LED_VERDE_PIN, LOW);
  digitalWrite(LED_AMARELO_PIN, LOW);
  digitalWrite(LED_VERMELHO_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  if (distanciaM < 0) return; // sem dado recebido ainda -- tudo apagado

  if (distanciaM > DISTANCIA_VERDE_M) {
    digitalWrite(LED_VERDE_PIN, HIGH);
  } else if (distanciaM > DISTANCIA_AMARELO_M) {
    digitalWrite(LED_AMARELO_PIN, HIGH);
    bool tocando = (millis() % BEEP_ESPACADO_INTERVALO_MS) < BEEP_ESPACADO_DURACAO_MS;
    digitalWrite(BUZZER_PIN, tocando ? HIGH : LOW);
  } else {
    if (millis() - ultimoToggleLed > BLINK_INTERVALO_MS) {
      ledVermelhoAceso = !ledVermelhoAceso;
      ultimoToggleLed = millis();
    }
    digitalWrite(LED_VERMELHO_PIN, ledVermelhoAceso ? HIGH : LOW);
    digitalWrite(BUZZER_PIN, HIGH);
  }
}

#if TRATOR_COM_PI
// Busca no Pi o alerta já cruzado (câmera + LoRa). Formato da resposta:
// "cor,piscando,som" em texto simples (ex: "vermelho,0,continuo") -- sem
// biblioteca de JSON de propósito, pra manter o firmware leve. Se falhar por
// qualquer motivo, simplesmente não atualiza nada -- quem decide cair pro
// modo local é o loop(), com base em há quanto tempo a última consulta OK.
//
// Roda numa tarefa própria no core 0 (ver tarefaConsultaAlertaPi/setup()),
// separada do loop() principal (core 1, onde o LoRa é escutado). http.GET()
// é bloqueante -- testado e confirmado que, rodando no mesmo núcleo/loop do
// LoRa, essa espera roubava a janela de recepção com frequência suficiente
// pra o trator nunca receber pacote nenhum da tag, mesmo a 1m de distância.
void consultarAlertaPi() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[alerta-pi] WiFi desconectado, nao consultou");
    return;
  }

  HTTPClient http;
  http.begin(PI_ALERTA_URL);
  http.setTimeout(800);
  int codigo = http.GET();
  if (codigo == 200) {
    // formato: cor,piscando,som,verde_m,amarelo_m
    String resposta = http.getString();
    int p1 = resposta.indexOf(',');
    int p2 = resposta.indexOf(',', p1 + 1);
    int p3 = resposta.indexOf(',', p2 + 1);
    int p4 = resposta.indexOf(',', p3 + 1);
    if (p1 > 0 && p2 > p1 && p3 > p2 && p4 > p3) {
      String cor = resposta.substring(0, p1);
      bool piscando = resposta.substring(p1 + 1, p2).toInt() == 1;
      String som = resposta.substring(p2 + 1, p3);
      som.trim();
      float verdeM = resposta.substring(p3 + 1, p4).toFloat();
      float amareloM = resposta.substring(p4 + 1).toFloat();

      // Seção crítica bem curta -- só cópia de bytes, sem alocação nem
      // I/O -- pra não travar o outro núcleo por muito tempo.
      portENTER_CRITICAL(&alertaPiMux);
      strncpy(corAlertaPi, cor.c_str(), sizeof(corAlertaPi) - 1);
      corAlertaPi[sizeof(corAlertaPi) - 1] = '\0';
      piscandoAlertaPi = piscando;
      strncpy(somAlertaPi, som.c_str(), sizeof(somAlertaPi) - 1);
      somAlertaPi[sizeof(somAlertaPi) - 1] = '\0';
      // Atualiza o cache da escala do fallback local -- ver comentário
      // acima de DISTANCIA_VERDE_M. Só muda quando o Pi confirma de verdade.
      DISTANCIA_VERDE_M = verdeM;
      DISTANCIA_AMARELO_M = amareloM;
      ultimoAlertaPiOk = millis();
      portEXIT_CRITICAL(&alertaPiMux);

      Serial.print("[alerta-pi] OK: ");
      Serial.println(resposta);
    } else {
      Serial.print("[alerta-pi] resposta em formato inesperado: ");
      Serial.println(resposta);
    }
  } else {
    Serial.print("[alerta-pi] FALHOU, codigo=");
    Serial.println(codigo);
    if (codigo < 0) Serial.println(http.errorToString(codigo));
  }
  http.end();
}

// Tarefa própria (core 0) que só fica consultando o Pi em loop, sem nunca
// competir por tempo de CPU com o loop() principal (core 1), que é onde o
// LoRa precisa ser escutado com a maior frequência possível.
void tarefaConsultaAlertaPi(void *parametro) {
  for (;;) {
    consultarAlertaPi();
    vTaskDelay(pdMS_TO_TICKS(INTERVALO_CONSULTA_ALERTA_MS));
  }
}

// Aciona o LED/buzzer com o alerta que já veio cruzado do Pi (câmera + LoRa)
// -- ao contrário de atualizarAlertaFisico(), aqui dá pra mostrar "confirmado"
// (sólido) de verdade, porque o Pi já sabe o que a câmera viu.
void atualizarAlertaFisicoComPi(const String& cor, bool piscando, const String& som) {
  digitalWrite(LED_VERDE_PIN, cor == "verde" ? HIGH : LOW);
  digitalWrite(LED_AMARELO_PIN, cor == "amarelo" ? HIGH : LOW);

  if (cor == "vermelho") {
    if (piscando) {
      if (millis() - ultimoToggleLed > BLINK_INTERVALO_MS) {
        ledVermelhoAceso = !ledVermelhoAceso;
        ultimoToggleLed = millis();
      }
      digitalWrite(LED_VERMELHO_PIN, ledVermelhoAceso ? HIGH : LOW);
    } else {
      digitalWrite(LED_VERMELHO_PIN, HIGH); // confirmado -- sólido, só o Pi consegue saber disso
    }
  } else {
    digitalWrite(LED_VERMELHO_PIN, LOW);
  }

  if (som == "espacado") {
    bool tocando = (millis() % BEEP_ESPACADO_INTERVALO_MS) < BEEP_ESPACADO_DURACAO_MS;
    digitalWrite(BUZZER_PIN, tocando ? HIGH : LOW);
  } else if (som == "continuo" || som == "urgente") {
    digitalWrite(BUZZER_PIN, HIGH); // "urgente" (caso MÁXIMO) tratado igual a contínuo por simplicidade
  } else {
    digitalWrite(BUZZER_PIN, LOW); // "off"
  }
}
#endif

// Gera um ID globalmente único a partir do MAC de fábrica do chip (nunca se
// repete entre placas) e guarda na memória não-volátil na primeira vez que
// roda; nas próximas vezes só lê o que já foi salvo. Cada dispositivo fica
// autônomo — não precisa editar código nem configurar nada pra identificar
// fisicamente cada unidade antes de gravar.
String obterOuCriarId() {
  preferencias.begin("no", false);
  String id = preferencias.getString("id", "");
  if (id == "") {
    id = String((unsigned long)(ESP.getEfuseMac() & 0xFFFFFF), HEX);
    preferencias.putString("id", id);
  }
  preferencias.end();
  return id;
}

#if TRATOR_COM_PI
void conectarWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_SENHA);
  Serial.print("Conectando no WiFi do Pi");
  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < 15000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? "\nWiFi conectado" : "\nWiFi FALHOU -- tenta de novo sozinho depois");
}

void enviarEntidadeProPi(const String& id, const String& tipo, float distanciaM, float anguloDeg, const String& fonteDistancia) {
  if (WiFi.status() != WL_CONNECTED) {
    conectarWiFi();
    if (WiFi.status() != WL_CONNECTED) return; // sem rede agora, perde esse pacote e segue
  }

  HTTPClient http;
  http.begin(PI_URL);
  http.addHeader("Content-Type", "application/json");

  String corpo = "{";
  corpo += "\"id\":\"" + id + "\",";
  corpo += "\"tipo\":\"" + tipo + "\",";
  corpo += "\"distancia_m\":" + String(distanciaM, 1) + ",";
  corpo += "\"angulo_deg\":" + String(anguloDeg, 1) + ",";
  // "fonte" (gps ou rssi) e so debug visual no dashboard, nao entra em nenhuma logica de alerta.
  corpo += "\"fonte\":\"" + fonteDistancia + "\"";
  corpo += "}";

  int codigo = http.POST(corpo);
  Serial.print("[entidade->Pi] corpo=");
  Serial.print(corpo);
  Serial.print(" codigo=");
  Serial.println(codigo);
  if (codigo <= 0) {
    Serial.print("Falha ao enviar pro Pi: ");
    Serial.println(http.errorToString(codigo));
  } else if (codigo != 200) {
    Serial.print("[entidade->Pi] Pi recusou, resposta: ");
    Serial.println(http.getString());
  }
  http.end();
}
#endif

// Direção do nó A até o nó B, calculada pelas duas posições de GPS — essa é
// a única fonte confiável que temos pra isso. O LoRa NÃO informa direção
// nenhuma (só força de sinal via RSSI/SNR — pra ter direção de verdade via
// rádio precisaria de antena direcional/array, que não temos). E o "course"
// que o próprio GPS relata é o rumo de quem está se movendo, não a direção
// até o outro nó — por isso o ângulo usado na rosa dos ventos é sempre
// calculado aqui, nunca simplesmente recebido de um sensor.
float calcularBearing(double lat1, double lon1, double lat2, double lon2) {
  double lat1Rad = radians(lat1);
  double lat2Rad = radians(lat2);
  double deltaLonRad = radians(lon2 - lon1);

  double y = sin(deltaLonRad) * cos(lat2Rad);
  double x = cos(lat1Rad) * sin(lat2Rad) - sin(lat1Rad) * cos(lat2Rad) * cos(deltaLonRad);
  double bearing = degrees(atan2(y, x));
  return fmod(bearing + 360.0, 360.0); // normaliza pra 0-360
}

float calcularDistanciaGPS(double lat1, double lon1, double lat2, double lon2) {
  double lat1Rad = radians(lat1);
  double lon1Rad = radians(lon1);
  double lat2Rad = radians(lat2);
  double lon2Rad = radians(lon2);

  double deltaLat = lat2Rad - lat1Rad;
  double deltaLon = lon2Rad - lon1Rad;

  double a = sin(deltaLat / 2) * sin(deltaLat / 2) + cos(lat1Rad) * cos(lat2Rad) * sin(deltaLon / 2) * sin(deltaLon / 2);
  double c = 2 * atan2(sqrt(a), sqrt(1 - a));
  return RAIO_TERRA_M * c;
}

float calcularDistanciaRSSI(int rssi) {
  float bruta = pow(10.0, (LORA_RSSI_1M - rssi) / (10.0 * EXPOENTE_PERDA_AMBIENTE));
  if (distanciaRSSISuavizada < 0) {
    distanciaRSSISuavizada = bruta; // primeira leitura -- sem histórico pra suavizar ainda
  } else {
    distanciaRSSISuavizada = RSSI_ALPHA_SUAVIZACAO * bruta + (1 - RSSI_ALPHA_SUAVIZACAO) * distanciaRSSISuavizada;
  }
  return distanciaRSSISuavizada;
}

// Diagnóstico do GPS, independente de qualquer pacote LoRa -- sem isso, com
// só 1 placa testando sozinha, não tem como saber se o módulo de GPS está
// fisicamente respondendo ou não. `charsProcessed()` sobe sempre que o
// módulo está mandando alguma coisa pela serial (mesmo sem fix ainda) -- se
// isso ficar em 0, é fiação/alimentação do GPS, não falta de sinal de
// satélite. `satellites.value()` e `location.isValid()` mostram se já
// conseguiu fix de verdade (normalmente precisa de céu aberto).
void imprimirStatusGPS(bool valido, double lat, double lon) {
  Serial.print("[GPS] chars processados=");
  Serial.print(gps.charsProcessed());
  Serial.print(" satelites=");
  Serial.print(gps.satellites.isValid() ? gps.satellites.value() : 0);
  Serial.print(" fix=");
  Serial.print(valido ? "SIM" : "NAO");
  if (valido) {
    Serial.print(" lat="); Serial.print(lat, 6);
    Serial.print(" lon="); Serial.print(lon, 6);
  }
  Serial.println();

  if (gps.charsProcessed() < 10) {
    Serial.println("[GPS] AVISO: quase nada chegando do modulo -- checar fiacao/alimentacao (RX=16, TX=17).");
  }
}

void transmitirPosicao(double lat, double lon, float alt, float vel, float curso) {
  LoRa.beginPacket();
  LoRa.print(meuId);
  LoRa.print(",");
  LoRa.print(TIPO_ENTIDADE);
  LoRa.print(",");
  LoRa.print(lat, 6);
  LoRa.print(",");
  LoRa.print(lon, 6);
  LoRa.print(",");
  LoRa.print(alt, 1);
  LoRa.print(",");
  LoRa.print(vel, 1);
  LoRa.print(",");
  LoRa.print(curso, 1);
  LoRa.endPacket();
}

void processarPacoteRecebido(double minhaLat, double minhaLon, bool meuGpsValido) {
  String mensagem = "";
  while (LoRa.available()) {
    mensagem += (char)LoRa.read();
  }

  // formato: id,tipo,lat,lon,alt,vel,curso
  int p1 = mensagem.indexOf(',');
  int p2 = mensagem.indexOf(',', p1 + 1);
  int p3 = mensagem.indexOf(',', p2 + 1);
  int p4 = mensagem.indexOf(',', p3 + 1);
  int p5 = mensagem.indexOf(',', p4 + 1);
  int p6 = mensagem.indexOf(',', p5 + 1);

  if (p1 < 0 || p2 < 0 || p3 < 0 || p4 < 0 || p5 < 0 || p6 < 0) {
    Serial.print("Pacote LoRa com formato inesperado, ignorado. Conteudo cru: '");
    Serial.print(mensagem);
    Serial.println("'");
    return;
  }

  String idRecebido = mensagem.substring(0, p1);
  if (idRecebido == meuId) return; // eco do proprio pacote (broadcast), ignora

  String tipoRecebido = mensagem.substring(p1 + 1, p2);
  double latRecebida = mensagem.substring(p2 + 1, p3).toDouble();
  double lonRecebida = mensagem.substring(p3 + 1, p4).toDouble();
  float altRecebida = mensagem.substring(p4 + 1, p5).toFloat();
  float velRecebida = mensagem.substring(p5 + 1, p6).toFloat();
  float cursoRecebido = mensagem.substring(p6 + 1).toFloat();
  (void)altRecebida; (void)velRecebida; (void)cursoRecebido; // ainda não usados no cálculo, mantidos pro log/uso futuro

  int rssi = LoRa.packetRssi();
  float snr = LoRa.packetSnr();
  float distanciaRSSI = calcularDistanciaRSSI(rssi);

  bool gpsFixDosDois = meuGpsValido && latRecebida != 0 && lonRecebida != 0;
  float distanciaGPS = gpsFixDosDois ? calcularDistanciaGPS(minhaLat, minhaLon, latRecebida, lonRecebida) : -1;
  float anguloAteONo = gpsFixDosDois ? calcularBearing(minhaLat, minhaLon, latRecebida, lonRecebida) : -1;

  // GPS é mais confiável quando os dois lados têm fix bom; RSSI é o fallback
  // (sem ângulo, porque RSSI não informa direção nenhuma).
  float distanciaFinal = gpsFixDosDois ? distanciaGPS : distanciaRSSI;

  Serial.println();
  Serial.print("De: "); Serial.print(idRecebido);
  Serial.print(" ("); Serial.print(tipoRecebido); Serial.println(")");
  Serial.print("RSSI="); Serial.print(rssi);
  Serial.print(" SNR="); Serial.println(snr);
  Serial.print("Distancia RSSI="); Serial.println(distanciaRSSI);
  if (gpsFixDosDois) {
    Serial.print("Distancia GPS="); Serial.println(distanciaGPS);
    Serial.print("Angulo ate o no="); Serial.println(anguloAteONo);
  } else {
    Serial.println("Sem fix de GPS dos dois lados -- so RSSI disponivel, sem angulo.");
  }

  distanciaMaisRecenteM = distanciaFinal;
  Serial.println(distanciaFinal > DISTANCIA_VERDE_M ? "Local seguro (fallback fisico)"
    : distanciaFinal > DISTANCIA_AMARELO_M ? "Atencao (fallback fisico)"
    : "ALERTA DE PROXIMIDADE (fallback fisico)");

#if TRATOR_COM_PI
  enviarEntidadeProPi(idRecebido, tipoRecebido, distanciaFinal, anguloAteONo, gpsFixDosDois ? "gps" : "rssi");
#endif
}

void loop() {
  // Sem delay() nenhum aqui -- LED/buzzer e a consulta ao Pi precisam rodar
  // todo loop pra animação de piscar/bipe não travar.
#if TRATOR_COM_PI
  // WL_DISCONNECTED especificamente (não "!= WL_CONNECTED" genérico) -- outros
  // estados (WL_IDLE_STATUS etc.) podem significar que uma tentativa anterior
  // ainda está em andamento no driver; chamar WiFi.begin() de novo nesse meio
  // tempo gera erro "sta is connecting, cannot set config" (visto em teste) e
  // ocupa o rádio bem na hora que o LoRa precisaria escutar. disconnect()
  // antes de begin() garante estado limpo antes de tentar de novo.
  if (WiFi.status() == WL_DISCONNECTED && millis() - ultimaTentativaWiFi > INTERVALO_RETRY_WIFI_MS) {
    Serial.println("[wifi] desconectado, tentando reconectar em segundo plano...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_SENHA); // assíncrono -- não bloqueia o loop
    ultimaTentativaWiFi = millis();
  }

  // consultarAlertaPi() não é mais chamada aqui -- roda sozinha na tarefa do
  // core 0 (tarefaConsultaAlertaPi, criada em setup()). Aqui só lê o
  // resultado mais recente, com a mesma seção crítica curta usada lá.
  char corLocal[12];
  char somLocal[12];
  bool piscandoLocal;
  unsigned long ultimoOkLocal;
  portENTER_CRITICAL(&alertaPiMux);
  strncpy(corLocal, corAlertaPi, sizeof(corLocal));
  strncpy(somLocal, somAlertaPi, sizeof(somLocal));
  piscandoLocal = piscandoAlertaPi;
  ultimoOkLocal = ultimoAlertaPiOk;
  portEXIT_CRITICAL(&alertaPiMux);

  bool alertaPiValido = (ultimoOkLocal != 0) && (millis() - ultimoOkLocal < ALERTA_PI_TIMEOUT_MS);
  if (alertaPiValido) {
    atualizarAlertaFisicoComPi(corLocal, piscandoLocal, somLocal);
  } else {
    atualizarAlertaFisico(distanciaMaisRecenteM); // Pi não respondeu a tempo -- cai pro modo local sozinho
  }
#else
  atualizarAlertaFisico(distanciaMaisRecenteM);
#endif

  while (gpsSerial.available() > 0) {
    gps.encode(gpsSerial.read());
  }

  double minhaLat = gps.location.lat();
  double minhaLon = gps.location.lng();
  float minhaVel = gps.speed.kmph();
  float minhaAlt = gps.altitude.meters();
  // Rumo do PRÓPRIO nó (só válido em movimento) — não confundir com o
  // ângulo até o outro nó, que é sempre calculado do lado de quem recebe.
  float meuCurso = gps.course.isValid() ? gps.course.deg() : -1;
  bool meuGpsValido = gps.location.isValid();

  if (millis() - ultimoDiagnosticoGPS > INTERVALO_DIAGNOSTICO_GPS_MS) {
    imprimirStatusGPS(meuGpsValido, minhaLat, minhaLon);
    ultimoDiagnosticoGPS = millis();
  }

  if (millis() - ultimoEnvio > proximoIntervaloEnvioMs) {
    // Com poucos nós (hoje: 2), transmite sempre -- não sorteia mais SE
    // transmite (isso deixava a atualização de distância lenta e instável,
    // ~6s de média). Em vez disso, sorteia QUANDO dentro da janela (jitter),
    // pra dois nós que ligaram em fase não ficarem colidindo pra sempre --
    // ver comentário de JITTER_ENVIO_MAX_MS acima.
    transmitirPosicao(minhaLat, minhaLon, minhaAlt, minhaVel, meuCurso);
    ultimoEnvio = millis();
    proximoIntervaloEnvioMs = INTERVALO_JANELA_MS + random(0, JITTER_ENVIO_MAX_MS + 1);
  }

  if (LoRa.parsePacket()) {
    processarPacoteRecebido(minhaLat, minhaLon, meuGpsValido);
  }
}
