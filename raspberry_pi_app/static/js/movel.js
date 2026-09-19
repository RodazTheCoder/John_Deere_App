// Navegacao do celular, gaveta e tela cheia da camera
// ---------- NAVEGAÇÃO DO CELULAR ----------
// Só vale com tela estreita (regras em @media): uma vista por vez, menu embaixo.
const MVIEWS = ['alerta','camera','hist','ajustes'];
function setMView(v){
  if(!MVIEWS.includes(v)) v = 'alerta';
  document.body.dataset.mview = v;
  document.body.classList.remove('lista-aberta');
  setCamFull(false);
  document.querySelectorAll('#mnav button').forEach(b=>b.classList.toggle('active', b.dataset.mv===v));
  // se estava na aba Histórico do desktop, volta pro painel pra o alerta continuar visível
  document.getElementById('viewPanel').style.display = 'flex';
  if(v==='camera') refreshSnapshot();
  document.querySelectorAll('.tab-btn').forEach(b=>b.classList.toggle('active', b.dataset.view==='panel'));
  if(history.replaceState) history.replaceState(null, '', '#' + v);
}
document.querySelectorAll('#mnav button').forEach(b=>b.addEventListener('click', ()=>setMView(b.dataset.mv)));
document.getElementById('ctxLine').addEventListener('click', ()=>{
  if(mobileMQ.matches && document.getElementById('ctxLine').textContent) document.body.classList.toggle('lista-aberta');
});
// Tela cheia da câmera: cobre o app todo (iPhone não tem a API de tela cheia
// pra isso); onde a API existe (Android) também pede tela cheia real.
const camFsBtn = document.getElementById('camFsBtn');
function setCamFull(on){
  document.body.classList.toggle('cam-full', on);
  try{
    if(on && document.documentElement.requestFullscreen && !document.fullscreenElement){
      document.documentElement.requestFullscreen({navigationUI:'hide'}).catch(()=>{});
    } else if(!on && document.fullscreenElement){
      document.exitFullscreen().catch(()=>{});
    }
  }catch(e){}
}
camFsBtn.addEventListener('click', ()=>setCamFull(!document.body.classList.contains('cam-full')));
document.addEventListener('fullscreenchange', ()=>{ if(!document.fullscreenElement) document.body.classList.remove('cam-full'); });
document.getElementById('listaFechar').addEventListener('click', ()=>document.body.classList.remove('lista-aberta'));
document.body.dataset.mview = 'alerta';
if(mobileMQ.matches) setMView(location.hash.slice(1));
mobileMQ.addEventListener('change', ()=>{ if(mobileMQ.matches) setMView(document.body.dataset.mview); });
