// Imagem da camera (snapshot) e consulta de tela
// Atualiza a imagem em loop (funciona em qualquer navegador, inclusive
// Safari/iPhone/iPad, que não suporta o streaming multipart de /video_feed).
const camImg = document.getElementById('camImg');
const mobileMQ = window.matchMedia('(max-width:860px), (max-height:520px) and (orientation:landscape)');
// No celular só baixa o quadro da câmera com a vista "Câmera" aberta: é só a
// imagem da tela. Detecção, alertas e buzzer rodam no Pi e não dependem disso.
function refreshSnapshot(){
  if(mobileMQ.matches && document.body.dataset.mview !== 'camera') return;
  camImg.src = '/snapshot.jpg?t=' + Date.now();
}
refreshSnapshot();
setInterval(refreshSnapshot, 150); // ~6-7 quadros/s, suficiente pra visualização
