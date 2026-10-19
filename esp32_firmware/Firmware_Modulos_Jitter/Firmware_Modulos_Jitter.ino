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
  // "Modo demo" ligado no dashboard -- repassado por LoRa (ver
  // transmitirPosicao()/processarPacoteRecebido()) pra tag de pessoa também
  // mostrar a cor do demo, em vez de continuar com a distância real dela.
  bool modoDemoAtivoPi = false;
  portMUX_TYPE alertaPiMux = portMUX_INITIALIZER_UNLOCKED;

  // O hotspot do Pi às vezes recusa a primeira tentativa de associação (bug
  // conhecido do driver WiFi onboard do Pi, não tem fix definitivo do lado
  // dele) -- sem isso aqui, se o boot falhasse e nenhum pacote LoRa chegasse
  // pra disparar o retry de enviarEntidadeProPi(), o trator ficava sem WiFi
  // pra sempre até alguém resetar a placa manualmente. WiFi.begin() é
  // assíncrono, então isso não trava o LED/buzzer enquanto tenta.
  const unsigned long INTERVALO_RETRY_WIFI_MS = 10000;
  unsigned long ultimaTentativaWiFi = 0;
#else
  // Nó sem Pi (tag de pessoa, ou outro trator sem Raspberry) -- precisa
  // funcionar INDEPENDENTE do trator/Pi, porque o ponto todo é a pessoa
  // poder ficar longe dele com segurança. Em vez de entrar na rede do Pi
  // (que exigiria estar perto), este nó cria a PRÓPRIA rede WiFi (ponto de
  // acesso) só pra servir uma página de status local -- um "mini-dashboard"
  // sem câmera, com o mesmo dado que já decide o LED/buzzer físico (ver
  // paginaStatus() mais abaixo). Continua ouvindo LoRa normalmente.
  #include <WiFi.h>
  #include <WebServer.h>

  const char* AP_SENHA = "cristake2b"; // >= 8 caracteres, exigido pelo SoftAP

  WebServer servidorStatus(80);
#endif

// Protege a leitura/escrita das variáveis que a página HTML mostra
// (distância, limiares, demo) entre o core 0 (servidorStatus, só existe no
// !TRATOR_COM_PI) e o core 1 (loop()/processarPacoteRecebido(), onde elas
// são calculadas) -- mesmo motivo do alertaPiMux acima. Declarado sem
// depender do #if pra poder usar os mesmos portENTER/EXIT_CRITICAL em
// processarPacoteRecebido() nos dois builds, sem duplicar código; no build
// do trator (TRATOR_COM_PI=1) fica só alguns bytes sem uso, nada é lido daqui
// por outro núcleo nesse caso.
portMUX_TYPE estadoWebMux = portMUX_INITIALIZER_UNLOCKED;

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
float DISTANCIA_VERDE_M = 15.0;
float DISTANCIA_AMARELO_M = 10.0;

const unsigned long BLINK_INTERVALO_MS = 500;
const unsigned long BEEP_ESPACADO_INTERVALO_MS = 5000;
const unsigned long BEEP_ESPACADO_DURACAO_MS = 150;

unsigned long ultimoToggleLed = 0;
bool ledVermelhoAceso = false;
float distanciaMaisRecenteM = -1; // -1 = nenhum dado recebido ainda (fica tudo apagado)
// -1 = direção desconhecida (sem fix de GPS dos dois lados -- só RSSI, que
// não informa ângulo nenhum). Usado na rosa dos ventos da página de status
// (ver paginaDados()/"/dados" mais abaixo) -- mesmo campo que já ia pro Pi
// em enviarEntidadeProPi(), só que agora também guardado localmente.
float anguloMaisRecenteDeg = -1;

// "Modo demo" recebido do trator por LoRa -- disponível em QUALQUER nó
// (inclusive sem WiFi), ao contrário de modoDemoAtivoPi (que só existe no
// TRATOR_COM_PI, lido direto do Pi). O loop() principal decide a cada
// volta se usa isto ou a distância real -- ver atualizarLedComOuSemDemo().
// Sem timeout de segurança, se o trator sumisse (desligou, saiu de alcance)
// o LED ficaria preso na última cor do demo pra sempre.
char corDemoRemoto[12] = "";
bool modoDemoRemotoAtivo = false;
unsigned long ultimoModoDemoRecebido = 0;
const unsigned long DEMO_REMOTO_TIMEOUT_MS = 5000;

#if !TRATOR_COM_PI
extern String meuId; // declarado mais abaixo no arquivo, usado aqui no título da página

// Casca HTML+CSS+JS no mesmo estilo visual do dashboard do Pi (verde/amarelo
// John Deere, fundo escuro) -- servida UMA vez só; depois disso o JS busca
// /dados (JSON pequeno) a cada 1s e só atualiza a bolinha/texto, sem recarregar
// a página inteira (sem piscar, bem mais parecido com o Pi de verdade). Sem
// fonte/biblioteca externa nenhuma -- este AP não tem internet, nada que não
// esteja aqui dentro carrega.
void paginaStatus() {
  String tipoMaiusculo = String(TIPO_ENTIDADE);
  tipoMaiusculo.toUpperCase();

  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>" TIPO_ENTIDADE "</title><style>";
  html += ":root{--g:#367C2B;--gd:#285C20;--y:#FFDE00;--bg:#11150F;--line:#333C2D;--text:#F1F3EC;--textdim:#A6AE9C}";
  html += "*{margin:0;padding:0;box-sizing:border-box}";
  // body em coluna com o header fixo em cima e o <main> ocupando o resto da
  // tela centralizado (vertical E horizontal) -- "tudo centralizado" pedido.
  html += "body{background:var(--bg);color:var(--text);font-family:-apple-system,Helvetica,Arial,sans-serif;";
  html += "text-align:center;display:flex;flex-direction:column;min-height:100vh}";
  html += "header{background:linear-gradient(180deg,var(--g),var(--gd));border-bottom:4px solid var(--y);padding:14px;flex-shrink:0}";
  html += "header h1{font-size:1.15rem;letter-spacing:.05em}";
  html += "header .id{font-size:.85rem;color:#dfe8d8;opacity:.85;font-family:ui-monospace,Menlo,monospace;margin-top:2px}";
  html += "main{flex:1;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:24px;padding:28px 20px}";
  // Rosa dos ventos -- mesmo visual do dashboard do Pi (ver radar.css),
  // só que calculado em JS puro aqui (sem framework nenhum).
  html += ".radar{position:relative;width:260px;height:260px;border-radius:50%;";
  html += "background:radial-gradient(circle at 50% 50%,#142012 0%,#0B120A 100%);border:2px solid var(--g);";
  html += "box-shadow:0 0 0 1px var(--line),0 0 22px rgba(54,124,43,.25)}";
  html += ".ring{position:absolute;border:1px solid var(--line);border-radius:50%}";
  html += ".r1{inset:0}.r2{inset:19%}.r3{inset:38%}";
  html += ".cl{position:absolute;font-family:ui-monospace,Menlo,monospace;font-size:10.5px;color:var(--textdim);letter-spacing:.05em}";
  html += ".cl-n{top:4px;left:50%;transform:translateX(-50%)}.cl-s{bottom:4px;left:50%;transform:translateX(-50%)}";
  html += ".cl-e{right:6px;top:50%;transform:translateY(-50%)}.cl-w{left:6px;top:50%;transform:translateY(-50%)}";
  html += ".eu{position:absolute;left:50%;top:50%;transform:translate(-50%,-50%);width:24px;height:24px;border-radius:6px;";
  html += "background:var(--y);color:#11150F;display:flex;align-items:center;justify-content:center;font-size:12px;";
  html += "z-index:3;border:2px solid var(--g)}";
  html += ".blip{position:absolute;width:18px;height:18px;margin-left:-9px;margin-top:-9px;border-radius:50%;";
  html += "display:flex;align-items:center;justify-content:center;font-size:10px;z-index:2;transition:left .3s,top .3s;";
  html += "background:#5FC04A;color:#062412;box-shadow:0 0 0 3px rgba(95,192,74,.25)}";
  html += ".dist{font-family:ui-monospace,Menlo,monospace;font-size:2.8rem;font-weight:700;color:var(--y);min-height:1.2em}";
  html += ".sub{color:var(--textdim);font-size:.9rem}";
  // Semáforo -- mesmo visual do dashboard do Pi (ver semaforo.css): 3 níveis
  // empilhados, só o nível atual fica "aceso".
  html += ".sem{display:flex;flex-direction:column;gap:10px;width:260px;text-align:left}";
  html += ".sem-row{display:flex;align-items:center;gap:14px;padding:12px 16px;border-radius:7px;background:#1A2017;";
  html += "border:1px solid var(--line);border-left-width:5px;opacity:.4;transition:opacity .2s,background .2s,border-color .2s}";
  html += ".sem-row.g{border-left-color:#5FC04A}.sem-row.a{border-left-color:#FFDE00}.sem-row.r{border-left-color:#FF5C4D}";
  html += ".sem-row.active{opacity:1}";
  html += ".sem-row.active.g{background:#1E3A19;border-color:rgba(95,192,74,.5)}";
  html += ".sem-row.active.a{background:#403A00;border-color:rgba(255,222,0,.5)}";
  html += ".sem-row.active.r{background:#3D1712;border-color:rgba(255,92,77,.55)}";
  html += ".sem-led{width:16px;height:16px;border-radius:50%;flex-shrink:0}";
  html += ".sem-led.g{background:#5FC04A}.sem-led.a{background:#FFDE00}.sem-led.r{background:#FF5C4D}";
  html += ".sem-row.active .sem-led.g{box-shadow:0 0 10px rgba(95,192,74,.8)}";
  html += ".sem-row.active .sem-led.a{box-shadow:0 0 10px rgba(255,222,0,.8)}";
  html += ".sem-row.active .sem-led.r{box-shadow:0 0 10px rgba(255,92,77,.85)}";
  html += ".sem-row.active.r .sem-led.blink{animation:blink .85s step-start infinite}";
  html += "@keyframes blink{50%{opacity:.15}}";
  html += ".sem-text{font-size:1.05rem;font-weight:600}";
  html += "</style></head><body>";
  html += "<header><h1>MÓDULO DE " + tipoMaiusculo + "</h1><div class='id'>id: " + meuId + "</div></header>";
  html += "<main>";
  html += "<div class='radar' id='radar'>";
  html += "<div class='ring r1'></div><div class='ring r2'></div><div class='ring r3'></div>";
  html += "<span class='cl cl-n'>FRENTE</span><span class='cl cl-s'>TRÁS</span>";
  html += "<span class='cl cl-e'>DIR</span><span class='cl cl-w'>ESQ</span>";
  html += "<div class='eu'>&#9650;</div>";
  html += "<div class='blip' id='blip' style='display:none'>&#9632;</div>";
  html += "</div>";
  html += "<div class='sem'>";
  html += "<div class='sem-row g' id='rowG'><span class='sem-led g'></span><span class='sem-text'>Seguro</span></div>";
  html += "<div class='sem-row a' id='rowA'><span class='sem-led a'></span><span class='sem-text'>Atenção</span></div>";
  html += "<div class='sem-row r' id='rowR'><span class='sem-led r' id='ledR'></span><span class='sem-text'>Crítico</span></div>";
  html += "</div>";
  html += "<div class='dist' id='dist'></div>";
  html += "<div class='sub' id='limiares'></div>";
  html += "</main><script>";
  html += "const RANGE_MIN_M=20;"; // raio minimo do radar, pra nao ficar tudo colado no centro com escala pequena (testes indoor)
  html += "const rowG=document.getElementById('rowG'),rowA=document.getElementById('rowA'),rowR=document.getElementById('rowR'),ledR=document.getElementById('ledR');";
  html += "async function atualizar(){try{";
  html += "const r=await fetch('/dados');const d=await r.json();";
  html += "[rowG,rowA,rowR].forEach(x=>x.classList.remove('active'));ledR.classList.remove('blink');";
  html += "if(d.cor==='#5FC04A')rowG.classList.add('active');";
  html += "else if(d.cor==='#FFDE00')rowA.classList.add('active');";
  html += "else if(d.cor==='#FF5C4D'){rowR.classList.add('active');ledR.classList.add('blink');}";
  html += "document.getElementById('dist').textContent=(d.distancia_m>=0)?(d.distancia_m.toFixed(1)+' m'):'';";
  html += "document.getElementById('limiares').textContent='verde >'+d.verde_m+'m, amarelo >'+d.amarelo_m+'m';";
  html += "const blip=document.getElementById('blip');";
  html += "if(d.distancia_m<0){blip.style.display='none';}else{";
  html += "blip.style.display='';blip.style.background=d.cor;";
  html += "const range=Math.max(d.verde_m*2, RANGE_MIN_M);";
  html += "const frac=Math.min(d.distancia_m/range,1)*0.85;"; // 0.85 pra nao encostar na borda
  html += "const ang=(d.angulo_deg>=0?d.angulo_deg:0)-90;const rad=ang*Math.PI/180;";
  html += "blip.style.left=(50+50*frac*Math.cos(rad))+'%';";
  html += "blip.style.top=(50+50*frac*Math.sin(rad))+'%';";
  html += "}";
  html += "}catch(e){}}";
  html += "atualizar();setInterval(atualizar,1000);";
  html += "</script></body></html>";

  servidorStatus.send(200, "text/html; charset=utf-8", html);
}

// JSON pequeno, montado à mão (sem biblioteca -- os únicos valores
// "textuais" aqui vêm de um conjunto fixo controlado por este mesmo
// firmware, nunca de fora, então não precisa de escape). Mostra a MESMA
// decisão que já acende o LED físico (ver loop()) -- duplicada aqui de
// propósito simples (ler variável, não pino), mas se mudar a lógica de
// prioridade demo-vs-real num lugar, mudar no outro também.
void paginaDados() {
  portENTER_CRITICAL(&estadoWebMux);
  float distanciaLocal = distanciaMaisRecenteM;
  float anguloLocal = anguloMaisRecenteDeg;
  float verdeLocal = DISTANCIA_VERDE_M;
  float amareloLocal = DISTANCIA_AMARELO_M;
  bool demoValidoLocal = modoDemoRemotoAtivo && (millis() - ultimoModoDemoRecebido < DEMO_REMOTO_TIMEOUT_MS);
  String corDemoLocal = String(corDemoRemoto);
  portEXIT_CRITICAL(&estadoWebMux);

  String corHex, texto;
  if (demoValidoLocal) {
    corHex = corDemoLocal == "verde" ? "#5FC04A" : corDemoLocal == "amarelo" ? "#FFDE00" : corDemoLocal == "vermelho" ? "#FF5C4D" : "#555555";
    texto = "Modo demo (controlado pelo dashboard do trator)";
  } else if (distanciaLocal < 0) {
    corHex = "#555555";
    texto = "Sem dado recebido ainda";
  } else if (distanciaLocal > verdeLocal) {
    corHex = "#5FC04A";
    texto = "Seguro";
  } else if (distanciaLocal > amareloLocal) {
    corHex = "#FFDE00";
    texto = "Atenção";
  } else {
    corHex = "#FF5C4D";
    texto = "ALERTA DE PROXIMIDADE";
  }

  String json = "{";
  json += "\"cor\":\"" + corHex + "\",";
  json += "\"texto\":\"" + texto + "\",";
  json += "\"distancia_m\":" + String(demoValidoLocal ? -1.0f : distanciaLocal, 1) + ",";
  json += "\"angulo_deg\":" + String(demoValidoLocal ? -1.0f : anguloLocal, 1) + ",";
  json += "\"verde_m\":" + String(verdeLocal, 0) + ",";
  json += "\"amarelo_m\":" + String(amareloLocal, 0) + ",";
  json += "\"demo\":" + String(demoValidoLocal ? "true" : "false");
  json += "}";

  servidorStatus.send(200, "application/json; charset=utf-8", json);
}

// Core 0, igual a tarefaConsultaAlertaPi do trator -- handleClient() roda
// aqui pra nunca competir com o loop() (core 1), que precisa ficar livre pra
// LoRa.parsePacket() com a maior frequência possível.
void tarefaServidorStatus(void *parametro) {
  for (;;) {
    servidorStatus.handleClient();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
#endif

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
#else
  // Ponto de acesso PRÓPRIO -- independente do Pi/trator de propósito (ver
  // comentário grande lá em cima, perto do #include <WebServer.h>). Nome
  // inclui o ID pra distinguir se tiver mais de uma tag por perto.
  String apSsid = String("Tag-") + TIPO_ENTIDADE + "-" + meuId;
  WiFi.softAP(apSsid.c_str(), AP_SENHA);
  Serial.print("--AP proprio criado: "); Serial.print(apSsid);
  Serial.print(" | IP: "); Serial.println(WiFi.softAPIP());
  servidorStatus.on("/", paginaStatus);
  servidorStatus.on("/dados", paginaDados);
  servidorStatus.begin();
  // Core 0 -- mesmo motivo do trator: handleClient() não pode competir com
  // o loop() (core 1) pela escuta do LoRa.
  xTaskCreatePinnedToCore(tarefaServidorStatus, "servidorStatus", 8192, NULL, 1, NULL, 0);
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

// Aciona o LED/buzzer com uma cor/som já decididos por outro lugar -- usada
// pelo trator com o alerta que veio do Pi (câmera + LoRa cruzados, pode
// mostrar "confirmado" sólido de verdade) E por QUALQUER nó (inclusive a tag
// de pessoa, sem WiFi) quando recebe um "modo demo" ativo via LoRa do
// trator -- ver processarPacoteRecebido(). Por isso fica fora do
// TRATOR_COM_PI: é só manipulação de pino, não depende de WiFi/HTTP.
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
    // formato: cor,piscando,som,verde_m,amarelo_m,modo_demo
    String resposta = http.getString();
    int p1 = resposta.indexOf(',');
    int p2 = resposta.indexOf(',', p1 + 1);
    int p3 = resposta.indexOf(',', p2 + 1);
    int p4 = resposta.indexOf(',', p3 + 1);
    int p5 = resposta.indexOf(',', p4 + 1);
    if (p1 > 0 && p2 > p1 && p3 > p2 && p4 > p3 && p5 > p4) {
      String cor = resposta.substring(0, p1);
      bool piscando = resposta.substring(p1 + 1, p2).toInt() == 1;
      String som = resposta.substring(p2 + 1, p3);
      som.trim();
      float verdeM = resposta.substring(p3 + 1, p4).toFloat();
      float amareloM = resposta.substring(p4 + 1, p5).toFloat();
      bool modoDemo = resposta.substring(p5 + 1).toInt() == 1;

      // Seção crítica bem curta -- só cópia de bytes, sem alocação nem
      // I/O -- pra não travar o outro núcleo por muito tempo.
      portENTER_CRITICAL(&alertaPiMux);
      strncpy(corAlertaPi, cor.c_str(), sizeof(corAlertaPi) - 1);
      corAlertaPi[sizeof(corAlertaPi) - 1] = '\0';
      piscandoAlertaPi = piscando;
      strncpy(somAlertaPi, som.c_str(), sizeof(somAlertaPi) - 1);
      somAlertaPi[sizeof(somAlertaPi) - 1] = '\0';
      modoDemoAtivoPi = modoDemo;
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
  LoRa.print(",");
  // Limiar atual deste nó (verde/amarelo) vai junto em todo pacote -- é
  // assim que o limiar configurado no dashboard do Pi chega nos nós sem
  // WiFi (a tag de pessoa, outros tratores sem Pi). Só o nó "trator" (o que
  // tem Pi) é tratado como fonte confiável por quem recebe -- ver
  // processarPacoteRecebido(). Os outros também mandam o próprio valor
  // (pra não deixar o campo vazio), mas ele é ignorado por quem recebe.
  LoRa.print(DISTANCIA_VERDE_M, 1);
  LoRa.print(",");
  LoRa.print(DISTANCIA_AMARELO_M, 1);
  LoRa.print(",");
  // "Modo demo" + a cor atual, só de verdade no nó "trator" (o único que
  // sabe disso, via consultarAlertaPi()) -- é assim que a tag de pessoa
  // (sem WiFi) também mostra a cor do demo em vez da distância real dela
  // enquanto o demo estiver ligado no dashboard. Ver
  // processarPacoteRecebido(). Os outros nós mandam "0," (sem efeito).
#if TRATOR_COM_PI
  portENTER_CRITICAL(&alertaPiMux);
  bool modoDemoLocal = modoDemoAtivoPi;
  char corLocalParaEnvio[12];
  strncpy(corLocalParaEnvio, corAlertaPi, sizeof(corLocalParaEnvio));
  portEXIT_CRITICAL(&alertaPiMux);
  LoRa.print(modoDemoLocal ? 1 : 0);
  LoRa.print(",");
  LoRa.print(corLocalParaEnvio);
#else
  LoRa.print(0);
  LoRa.print(",");
#endif
  LoRa.endPacket();
}

void processarPacoteRecebido(double minhaLat, double minhaLon, bool meuGpsValido) {
  String mensagem = "";
  while (LoRa.available()) {
    mensagem += (char)LoRa.read();
  }

  // formato: id,tipo,lat,lon,alt,vel,curso,verde_m,amarelo_m,modo_demo,cor_demo
  int p1 = mensagem.indexOf(',');
  int p2 = mensagem.indexOf(',', p1 + 1);
  int p3 = mensagem.indexOf(',', p2 + 1);
  int p4 = mensagem.indexOf(',', p3 + 1);
  int p5 = mensagem.indexOf(',', p4 + 1);
  int p6 = mensagem.indexOf(',', p5 + 1);
  int p7 = mensagem.indexOf(',', p6 + 1);
  int p8 = mensagem.indexOf(',', p7 + 1);
  int p9 = mensagem.indexOf(',', p8 + 1);
  int p10 = mensagem.indexOf(',', p9 + 1);

  if (p1 < 0 || p2 < 0 || p3 < 0 || p4 < 0 || p5 < 0 || p6 < 0 || p7 < 0 || p8 < 0 || p9 < 0 || p10 < 0) {
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
  float cursoRecebido = mensagem.substring(p6 + 1, p7).toFloat();
  float verdeRecebido = mensagem.substring(p7 + 1, p8).toFloat();
  float amareloRecebido = mensagem.substring(p8 + 1, p9).toFloat();
  bool modoDemoRecebido = mensagem.substring(p9 + 1, p10).toInt() == 1;
  (void)altRecebida; (void)velRecebida; (void)cursoRecebido; // ainda não usados no cálculo, mantidos pro log/uso futuro

  // Propaga o limiar pela rede LoRa -- só o "trator" é tratado como fonte
  // confiável (é o único tipo de nó que fala com o Pi nesse projeto hoje).
  // Isso é como a tag de pessoa, sem WiFi nenhum, fica sabendo do limiar
  // configurado no dashboard. Nunca sobrescreve com zero/negativo (pacote
  // corrompido ou nó que nunca recebeu nada do Pi ainda).
  if (tipoRecebido == "trator" && verdeRecebido > 0 && amareloRecebido > 0) {
    portENTER_CRITICAL(&estadoWebMux);
    DISTANCIA_VERDE_M = verdeRecebido;
    DISTANCIA_AMARELO_M = amareloRecebido;
    portEXIT_CRITICAL(&estadoWebMux);
  }

  // Modo demo ligado/desligado no dashboard, repassado pelo trator -- só
  // GUARDA o estado aqui (quem decide se usa isso ou a distância real é o
  // loop() principal, toda volta -- ver DEMO_REMOTO_TIMEOUT_MS). Não dá
  // "return" aqui de propósito: o resto da função (cálculo de distância
  // real) continua rodando normalmente mesmo com o demo ligado, só o LED
  // final que é decidido em loop().
  if (tipoRecebido == "trator") {
    if (modoDemoRecebido) {
      String corDemoRecebida = mensagem.substring(p10 + 1);
      corDemoRecebida.trim();
      if (corDemoRecebida.length() > 0) {
        portENTER_CRITICAL(&estadoWebMux);
        strncpy(corDemoRemoto, corDemoRecebida.c_str(), sizeof(corDemoRemoto) - 1);
        corDemoRemoto[sizeof(corDemoRemoto) - 1] = '\0';
        modoDemoRemotoAtivo = true;
        ultimoModoDemoRecebido = millis();
        portEXIT_CRITICAL(&estadoWebMux);
      }
    } else {
      // Desligou o demo no dashboard -- some na hora, não espera o timeout.
      portENTER_CRITICAL(&estadoWebMux);
      modoDemoRemotoAtivo = false;
      portEXIT_CRITICAL(&estadoWebMux);
    }
  }

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

  portENTER_CRITICAL(&estadoWebMux);
  distanciaMaisRecenteM = distanciaFinal;
  anguloMaisRecenteDeg = anguloAteONo;
  portEXIT_CRITICAL(&estadoWebMux);
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
  bool demoRemotoValido = modoDemoRemotoAtivo && (millis() - ultimoModoDemoRecebido < DEMO_REMOTO_TIMEOUT_MS);
  if (demoRemotoValido) {
    atualizarAlertaFisicoComPi(String(corDemoRemoto), true, "off");
  } else {
    atualizarAlertaFisico(distanciaMaisRecenteM);
  }
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
