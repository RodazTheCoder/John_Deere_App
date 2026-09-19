// Rosa dos ventos: posicao e limpeza dos pontos
const radar = document.getElementById('radar');
const R = 119; // radar radius in px (half of 238)

function polarBlip(angleDeg, distFrac, type, pending){
  // angleDeg: 0 = front(top), clockwise
  const rad = (angleDeg - 90) * Math.PI/180;
  const r = Math.min(distFrac, 1) * (R - 12) / R; // fração do raio; posição em % pra o radar poder mudar de tamanho (celular)
  const x = 50 + 50 * r * Math.cos(rad);
  const y = 50 + 50 * r * Math.sin(rad);
  const el = document.createElement('div');
  el.className = 'blip ' + (type === 'tractor' ? 'tractor' : 'person') + (pending ? ' pending' : '');
  el.style.left = x+'%';
  el.style.top = y+'%';
  el.textContent = type === 'tractor' ? '▣' : '●';
  return el;
}

function clearBlips(){
  radar.querySelectorAll('.blip').forEach(b=>b.remove());
}
