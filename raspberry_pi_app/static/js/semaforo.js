// Constantes de status e atualizacao do semaforo de risco
// ---------- STATUS REAL ----------
const RADAR_ALCANCE_M = 160; // escala visual do radar — ajustar quando soubermos o alcance real do LoRa
const severityMap = { seguro:'g', atencao:'a', critico_pendente:'r', critico_confirmado:'r', nao_identificado:'r', camera_offline:'n' };
const rTextSubMap = {
  seguro: '> 100 m', atencao: '50 – 100 m',
  critico_pendente: 'aguardando confirmação visual',
  critico_confirmado: 'confirmado visualmente',
  nao_identificado: 'sem registro no LoRa',
};
let rTextSubFallback = '< 50 m'; // atualizado em renderEscala() conforme a escala configurada

let nivelAnterior = null;

// "fonte" (gps/rssi) é só debug visual -- mostra de onde vem a distância
// exibida, nunca influencia o nível do alerta (ver comunicacao/entidade_receiver.py).
const fonteLabelMap = { gps: 'GPS', rssi: 'RSSI' };
function atualizarSemaforo(nivel, distanciaM, fonte){
  [rowG,rowA,rowR].forEach(r=>r.classList.remove('active'));
  const riscoCor = nivel==='seguro' ? 'g' : (nivel==='atencao' ? 'a' : 'r');
  document.body.dataset.risk = riscoCor;
  document.getElementById('camRiskTxt').textContent = {g:'Seguro', a:'Atenção', r:'Risco crítico'}[riscoCor];
  document.getElementById('camRiskDist').textContent = (distanciaM===null || distanciaM===undefined) ? '—' : Math.round(distanciaM) + ' m';
  ledR.classList.remove('blink');
  if(nivel==='seguro') rowG.classList.add('active');
  else if(nivel==='atencao') rowA.classList.add('active');
  else { // critico_pendente, critico_confirmado, nao_identificado
    rowR.classList.add('active');
    ledR.classList.toggle('blink', nivel==='critico_pendente');
  }
  rTextSub.textContent = rTextSubMap[nivel] || rTextSubFallback;
  distNum.textContent = (distanciaM===null || distanciaM===undefined) ? '—' : Math.round(distanciaM);
  if(fonteLabelMap[fonte]){
    distFonte.textContent = fonteLabelMap[fonte];
    distFonte.className = 'fonte ' + fonte;
    distFonte.style.display = '';
  } else {
    distFonte.style.display = 'none';
  }
}
