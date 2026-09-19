// Elementos de audio e botao silenciar/reativar
const audioIcon = document.getElementById('audioIcon');
const audioLabel = document.getElementById('audioLabel');
const audioSub = document.getElementById('audioSub');
const muteBtn = document.getElementById('muteBtn');

const soundIconMap = { off:'🔇', espacado:'🔈', continuo:'🔊', urgente:'🚨' };

let somSilenciadoAtual = false;
async function alternarSom(){
  // silenciado -> reativa (DELETE); tocando -> silencia (POST)
  await fetch('/api/silenciar', {method: somSilenciadoAtual ? 'DELETE' : 'POST'});
  pollStatusReal(); // atualiza a tela na hora, sem esperar o próximo tick de 1s
}

muteBtn.addEventListener('click', alternarSom);
ackBtn.addEventListener('click', alternarSom);
