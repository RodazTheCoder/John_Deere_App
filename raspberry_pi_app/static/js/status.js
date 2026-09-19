// Conexao com o Pi e consulta de /api/status a cada segundo
// Conexão com o Pi: se o /api/status não responde (celular sem WiFi, Pi fora
// do ar), avisa e apaga os dados, pra ninguém confiar num "Seguro" velho.
let ultimoStatusOk = Date.now();
const connBanner = document.getElementById('connBanner');
const connAge = document.getElementById('connAge');
function marcarConexao(ok){
  document.body.classList.toggle('stale', !ok);
  connBanner.classList.toggle('show', !ok);
  if(!ok) connAge.textContent = 'há ' + Math.round((Date.now() - ultimoStatusOk)/1000) + ' s';
}

async function pollStatusReal(){
  const ctl = new AbortController();
  const timer = setTimeout(()=>ctl.abort(), 3000);
  try{
    const r = await fetch('/api/status', {signal: ctl.signal});
    const s = await r.json();
    clearTimeout(timer);
    ultimoStatusOk = Date.now();
    marcarConexao(true);

    dotPi.classList.toggle('off', !s.camera_online);
    dotEsp.classList.toggle('off', !s.esp_online);
    renderModoDemo(!!s.modo_demo);
    renderEscala(s.escala_verde_m, s.escala_amarelo_m);

    if(!s.camera_online){
      camCaption.innerHTML = '<b>CÂMERA OFFLINE</b> — sem sinal do Raspberry Pi';
    } else if(s.deteccoes.length > 0){
      camCaption.innerHTML = `<b>Detecção</b> — ${s.deteccoes.length} pessoa(s) no quadro atual`;
    } else {
      camCaption.innerHTML = '<b>Aguardando</b> — nenhuma detecção no quadro atual';
    }

    atualizarAlertaReal(s.alerta, s.entidades);
  }catch(e){
    clearTimeout(timer);
    marcarConexao(false);
    dotPi.classList.add('off');
    camCaption.innerHTML = '<b>SEM CONEXÃO</b> — não foi possível falar com o servidor';
  }
}
pollStatusReal();
setInterval(pollStatusReal, 1000);
