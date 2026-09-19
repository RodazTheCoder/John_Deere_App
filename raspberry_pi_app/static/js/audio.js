// Estado de audio/buzzer no painel
// O backend é a única fonte de verdade sobre o som (dashboard E buzzer
// físico leem o mesmo campo `alerta.som.estado` -- ver /api/silenciar em
// app.py) -- nenhum estado de "mudo" só no navegador.
function atualizarAudio(som){
  const labelMap = {
    off: 'Silêncio',
    espacado: 'Bipe espaçado (~5s)',
    continuo: 'Bipe contínuo',
    urgente: 'Alarme contínuo — tom urgente',
    silenciado: 'Som silenciado pelo operador',
  };
  audioIcon.textContent = (som.estado==='off' || som.estado==='silenciado') ? '🔇' : (soundIconMap[som.estado] || '🔈');
  audioLabel.textContent = labelMap[som.estado] || som.estado;
  somSilenciadoAtual = som.estado === 'silenciado';

  if(som.estado === 'silenciado'){
    audioSub.textContent = 'LED continua ativo até a situação mudar';
    muteBtn.style.display = '';
    muteBtn.classList.remove('muted');
    muteBtn.classList.add('reativar');
    muteBtn.textContent = 'REATIVAR SOM';
    muteBtn.disabled = false;
  } else if(som.estado === 'off'){
    audioSub.textContent = '';
    muteBtn.classList.remove('reativar');
    muteBtn.style.display = 'none';
  } else {
    audioSub.textContent = 'Toca até a distância mudar ou ser reconhecida';
    muteBtn.style.display = '';
    muteBtn.classList.remove('muted', 'reativar');
    muteBtn.textContent = 'SILENCIAR SOM';
    muteBtn.disabled = false;
  }
}
