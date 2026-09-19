// Direcao, lista de entidades, linha de contexto e pontos do radar
// Direção só aparece quando vem de GPS: sem GPS (só RSSI) o ângulo não existe.
function esc(t){ return String(t).replace(/[&<>"']/g, c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c])); }
const DIRECOES = ['à frente','à frente, à direita','à direita','atrás, à direita','atrás','atrás, à esquerda','à esquerda','à frente, à esquerda'];
function direcaoDe(e){
  if(typeof e.angulo_deg !== 'number') return null;
  return DIRECOES[Math.round((((e.angulo_deg%360)+360)%360)/45)%8];
}

// Lista de todas as entidades (ordenadas por distância), cada uma com a cor
// do nível em que ela mesma estaria -- o semáforo continua olhando só a mais próxima.
function nivelDaDistancia(d){ return d > escalaVerdeAtual ? 'g' : (d > escalaAmareloAtual ? 'a' : 'r'); }
function renderListaEntidades(entidades){
  const lista = Object.entries(entidades||{}).map(([id,e])=>({id, ...e})).sort((a,b)=>a.distancia_m - b.distancia_m);
  document.getElementById('entCount').textContent = lista.length ? '(' + lista.length + ')' : '';
  document.getElementById('entBadge').textContent = lista.length;
  document.getElementById('entMeta').textContent = lista.length ? lista.length + (lista.length===1 ? ' entidade' : ' entidades') + ' · a mais próxima define o nível' : 'nenhuma no momento';
  if(!lista.length){
    const vazio = '<div class="ent-vazio">Nenhuma entidade no alcance</div>';
    document.getElementById('entList').innerHTML = vazio;
    document.getElementById('entListPC').innerHTML = vazio;
    return;
  }
  const html = lista.map(e=>{
    const trator = e.tipo==='trator';
    const dir = e.fonte==='rssi' ? 'sem direção' : (direcaoDe(e) || '');
    const fonte = fonteLabelMap[e.fonte] ? `<span class="fonte ${e.fonte}">${fonteLabelMap[e.fonte]}</span>` : '';
    return `<div class="ent-row ${nivelDaDistancia(e.distancia_m)}">
      <span class="ent-ico ${trator ? 'tractor' : 'person'}">${trator ? '▣' : '●'}</span>
      <div class="ent-main">${trator ? 'Trator' : 'Pessoa'}<small>${esc(e.id)}</small>
        <div class="ent-sub">${esc(dir)} ${fonte}</div></div>
      <div class="ent-dist">${Math.round(e.distancia_m)}<small> m</small></div>
    </div>`;
  }).join('');
  document.getElementById('entList').innerHTML = html;
  document.getElementById('entListPC').innerHTML = html;
}

function atualizarContexto(e, total){
  const el = document.getElementById('ctxLine');
  if(!e){ el.textContent = ''; return; }
  let txt = e.tipo==='trator' ? 'Trator' : 'Pessoa';
  if(e.fonte==='rssi') txt += ' · direção indisponível (RSSI)';
  else if(direcaoDe(e)) txt += ' · ' + direcaoDe(e);
  if(total>1) txt += ' · +' + (total-1) + (total-1===1 ? ' outra' : ' outras');
  el.textContent = txt;
}

function atualizarRadar(entidades){
  clearBlips();
  Object.values(entidades||{}).forEach(e=>{
    const distFrac = Math.min(e.distancia_m / RADAR_ALCANCE_M, 1);
    const tipo = e.tipo==='trator' ? 'tractor' : 'person';
    radar.appendChild(polarBlip(e.angulo_deg||0, distFrac, tipo, false));
  });
}
