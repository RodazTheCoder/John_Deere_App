// Atualizacao do alerta completo e registro no historico
function atualizarAlertaReal(alerta, entidades){
  const nivelMudou = alerta.nivel !== nivelAnterior;

  const listaEntidades = Object.values(entidades||{});
  const maisProxima = listaEntidades.length
    ? listaEntidades.reduce((a,b)=> a.distancia_m<=b.distancia_m ? a : b)
    : null;
  atualizarSemaforo(alerta.nivel, alerta.distancia_m, maisProxima ? maisProxima.fonte : null);
  atualizarRadar(entidades);
  atualizarContexto(maisProxima, listaEntidades.length);
  renderListaEntidades(entidades);

  if(nivelMudou){
    alertBanner.classList.remove('acked');
    ackBtn.style.display = '';
    ackBtn.textContent = 'RECONHECER';
    alertTxt.innerHTML = `${alerta.mensagem}<small>Câmera confirma presença sem tag LoRa correspondente</small>`;
    pushLogReal(alerta.nivel, alerta.mensagem);
    nivelAnterior = alerta.nivel;
  }

  // som reativado com o alerta ainda ativo: o banner volta ao estado "não reconhecido"
  if(alerta.nivel === 'nao_identificado' && alerta.som.estado !== 'silenciado' && alertBanner.classList.contains('acked')){
    alertBanner.classList.remove('acked');
    ackBtn.textContent = 'RECONHECER';
    alertTxt.innerHTML = `${alerta.mensagem}<small>Câmera confirma presença sem tag LoRa correspondente</small>`;
    if(eventLog.length){ eventLog[0].acked = false; renderLog(); }
  }

  const maxSilenciado = alerta.nivel === 'nao_identificado' && alerta.som.estado === 'silenciado';
  if(maxSilenciado && !alertBanner.classList.contains('acked')){
    alertBanner.classList.add('acked');
    ackBtn.style.display = '';
    ackBtn.textContent = 'REATIVAR SOM';
    const t = new Date().toLocaleTimeString('pt-BR');
    alertTxt.innerHTML = `NÃO IDENTIFICADO — reconhecido pelo operador<small>Buzzer silenciado às ${t} · LED permanece ativo até a situação mudar</small>`;
    if(eventLog.length){ eventLog[0].acked = true; renderLog(); }
  }

  atualizarAudio(alerta.som);
  alertBanner.classList.toggle('show', alerta.nivel === 'nao_identificado');
}

function pushLogReal(nivel, mensagem){
  eventLog.unshift({ time:new Date(), sev:severityMap[nivel]||'n', text:mensagem, acked:false });
  renderLog();
}
