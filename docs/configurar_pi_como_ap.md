# Raspberry Pi como ponto de acesso WiFi (hotspot)

**Por que o Pi cria a rede, e não o ESP32:** no meio da floresta não existe roteador nenhum — alguém precisa criar a rede do zero. O Raspberry Pi já é a máquina mais capaz do sistema (Linux completo, já roda o servidor do dashboard) e já precisa hospedar uma rede de qualquer jeito, pro tablet do operador conseguir abrir o painel. O ESP32 também consegue virar Access Point, mas: (a) o ESP32 do trator já está ocupado lendo GPS + LoRa em loop, então rodar um AP nele competiria por recursos com um microcontrolador bem mais fraco; (b) o ESP32 em modo AP aceita poucos clientes simultâneos (tipicamente ~4), enquanto o Pi como AP lida bem com o tablet + o(s) ESP32(s) que precisarem falar com ele; (c) inverteria a relação cliente/servidor sem necessidade (o Pi já É o servidor do dashboard). Por isso: **Pi cria o hotspot, ESP32(s) e tablet se conectam nele como clientes.**

Isso ainda **não foi testado** — só dá pra fazer com acesso físico ao Raspberry Pi (não dá pra simular no Mac de desenvolvimento). Os passos abaixo são a referência de como fazer quando chegar a hora.

## Raspberry Pi OS Bookworm (2023+) — usa NetworkManager

Bookworm já vem com `NetworkManager`, que tem um comando pronto pra criar hotspot:

```bash
sudo nmcli device wifi hotspot ifname wlan0 ssid "JohnDeere-Trator" password "escolher-uma-senha"
```

Isso já sobe o AP com DHCP incluído. Por padrão o Pi assume o IP `10.42.0.1` nessa rede (não `192.168.4.1` como no exemplo do firmware — **checar o IP real depois de rodar o comando** com `ip addr show wlan0`, e ajustar `PI_URL` no firmware do ESP32 — `esp32_firmware/ProjetoLoRa1/ProjetoLoRa1.ino` — de acordo).

Pra deixar o hotspot subindo sozinho no boot:
```bash
sudo nmcli connection modify Hotspot connection.autoconnect yes
```

## Raspberry Pi OS mais antigo — `hostapd` + `dnsmasq`

Se o Pi ainda estiver numa versão anterior ao Bookworm (sem NetworkManager por padrão), o caminho é `hostapd` (cria o AP) + `dnsmasq` (dá IP pros clientes) + IP estático na interface `wlan0`. É mais passo a passo — a documentação oficial da Raspberry Pi Foundation tem o guia completo ("Setting up a Raspberry Pi as an access point"); vale seguir ela em vez de reproduzir aqui, porque os pacotes/caminhos de configuração mudam entre versões do Raspberry Pi OS.

## Depois de configurar

1. Anotar o SSID, senha e o IP do Pi na rede criada.
2. Atualizar `WIFI_SSID`, `WIFI_SENHA` e `PI_URL` em `esp32_firmware/ProjetoLoRa1/ProjetoLoRa1.ino` (só no ESP32 com `TRATOR_COM_PI` habilitado).
3. Testar com `curl` de outro dispositivo conectado na rede do Pi, simulando o que o ESP32 vai mandar:
   ```bash
   curl -X POST http://<IP-do-Pi>:5050/api/entidade \
     -H "Content-Type: application/json" \
     -d '{"id":"teste","tipo":"pessoa","distancia_m":20,"angulo_deg":90}'
   ```
   Se retornar `{"ok":true}`, o caminho está funcionando — só falta o ESP32 de verdade mandando isso sozinho.
