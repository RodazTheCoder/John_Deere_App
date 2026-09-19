// Relogio da cabecalho
// clock
function tick(){
  document.getElementById('clock').textContent = new Date().toLocaleTimeString('pt-BR');
}
tick(); setInterval(tick, 1000);
