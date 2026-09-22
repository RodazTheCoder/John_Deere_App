// Banner de alerta e historico de eventos
// must exist before first applyScenario() call at the bottom of this script
const alertTxt = document.getElementById('alertTxt');
const ackBtn = document.getElementById('ackBtn');

const eventLog = [];
const logList = document.getElementById('logList');
const logBadge = document.getElementById('logBadge');
const historyMeta = document.getElementById('historyMeta');

function renderLog(){
  logBadge.textContent = eventLog.length;
  document.getElementById('logBadgeM').textContent = eventLog.length;
  historyMeta.textContent = eventLog.length + (eventLog.length===1 ? ' evento registrado nesta sessão' : ' eventos registrados nesta sessão');
  logList.innerHTML = eventLog.map(e=>`
    <div class="log-entry">
      <span class="log-dot ${e.sev}"></span>
      <div class="log-body">
        <div class="log-text">${e.text}</div>
        <div class="log-time">${e.time.toLocaleTimeString('pt-BR')}</div>
      </div>
      ${e.acked ? '<span class="log-ackpill">RECONHECIDO</span>' : ''}
    </div>
  `).join('');
}
