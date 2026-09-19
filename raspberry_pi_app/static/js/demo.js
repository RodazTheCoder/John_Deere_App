// Modo demo, entidade falsa e escala dos limiares
let currentScenarioKey = 'safe';

// Botões de demo mandam uma entidade FALSA pro mesmo endpoint que o ESP32
// vai usar (POST /api/entidade) — não é um mock só no front-end, é o
// backend real (logica_alerta.py) recebendo um dado simulado no lugar do
// LoRa. Serve de plano B pra demonstração caso o GPS/ESP32 não funcione ao
// vivo. A câmera continua sempre real: pra ver "confirmado" ou "MÁXIMO",
// é preciso fisicamente estar (ou não) na frente da câmera de verdade.
const ENTIDADE_DEMO_ID = "demo-pessoa";

function aleatorioEntre(min, max){
  return min + Math.random() * (max - min);
}

// Distância E ângulo sorteados dentro da faixa de cada nível a cada clique
// (não um valor fixo sempre igual) — mostra a lógica reagindo a dados
// variados de verdade, não só repetindo o mesmo número.
async function definirEntidadeDemo(distanciaMin, distanciaMax){
  const distanciaM = aleatorioEntre(distanciaMin, distanciaMax);
  const anguloDeg = aleatorioEntre(0, 360);
  await fetch('/api/entidade', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({id: ENTIDADE_DEMO_ID, tipo: 'pessoa', distancia_m: distanciaM, angulo_deg: anguloDeg}),
  });
  pollStatusReal();
}

async function removerEntidadeDemo(){
  await fetch('/api/entidade/' + ENTIDADE_DEMO_ID, {method: 'DELETE'});
  pollStatusReal();
}

// Faixas dos botões de demo acompanham a escala atual (ver /api/escala) --
// clicar "Crítico" com escala 10m injeta um valor bem menor do que com a
// escala padrão de 100m, sempre dentro da faixa certa pra aquele nível.
let escalaVerdeAtual = 100;
let escalaAmareloAtual = 50;

const demoAcoes = {
  safe: () => definirEntidadeDemo(escalaVerdeAtual + 1, escalaVerdeAtual * 2.2),
  amber: () => definirEntidadeDemo(escalaAmareloAtual + 1, escalaVerdeAtual - Math.max(1, escalaVerdeAtual * 0.02)),
  critico: () => definirEntidadeDemo(Math.max(1, escalaAmareloAtual * 0.1), Math.max(2, escalaAmareloAtual - 1)),
  unidentified: () => removerEntidadeDemo(),
};

function applyScenario(key){
  currentScenarioKey = key;
  document.querySelectorAll('.demo-btn[data-s]').forEach(b=>b.classList.toggle('on', b.dataset.s===key));
  const acao = demoAcoes[key];
  if (acao) acao();
}

document.querySelectorAll('.demo-btn[data-s]').forEach(btn=>{
  btn.addEventListener('click', ()=>applyScenario(btn.dataset.s));
});

// Modo Demo: interruptor explícito -- ligado, só a entidade falsa dos
// botões conta (dado real do LoRa é ignorado); desligado, só entidade real
// conta (botões ficam visualmente inertes). Nunca mistura os dois -- ver
// POST /api/modo-demo em app.py. Evita o problema de uma tag desconectada
// "brigar" com um clique de demo.
const modoDemoBtn = document.getElementById('modoDemoBtn');
const demoBotoes = document.getElementById('demoBotoes');
let modoDemoAtual = false;

function renderModoDemo(ativo){
  modoDemoAtual = ativo;
  modoDemoBtn.textContent = ativo ? 'MODO DEMO: LIGADO' : 'MODO DEMO: DESLIGADO';
  modoDemoBtn.classList.toggle('on', ativo);
  document.getElementById('demoChip').style.display = ativo ? '' : 'none';
  demoBotoes.style.opacity = ativo ? '1' : '.35';
  demoBotoes.style.pointerEvents = ativo ? '' : 'none';
}

modoDemoBtn.addEventListener('click', async ()=>{
  await fetch('/api/modo-demo', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({ativo: !modoDemoAtual}),
  });
  pollStatusReal();
});

// Escala: reconfigura os limiares verde/amarelo em tempo real (mantendo a
// proporção 1:2), pra testar em ambientes menores sem precisar de 100m de
// verdade -- vale tanto pra dado real quanto pros botões de demo. Você
// digita qualquer valor pro limiar verde, o amarelo é sempre metade dele.
const escalaInput = document.getElementById('escalaInput');

async function aplicarEscala(verdeM){
  await fetch('/api/escala', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({verde_m: verdeM}),
  });
  pollStatusReal();
}

document.getElementById('escalaAplicarBtn').addEventListener('click', ()=>{
  const valor = Number(escalaInput.value);
  if (valor > 0) aplicarEscala(valor);
});
document.getElementById('escalaPadraoBtn').addEventListener('click', ()=>{
  escalaInput.value = '';
  aplicarEscala(null);
});
escalaInput.addEventListener('keydown', (e)=>{
  if (e.key === 'Enter') document.getElementById('escalaAplicarBtn').click();
});

function renderEscala(verdeM, amareloM){
  escalaVerdeAtual = verdeM;
  escalaAmareloAtual = amareloM;
  // não sobrescreve enquanto o campo está em foco (usuário digitando)
  if (document.activeElement !== escalaInput) {
    escalaInput.value = verdeM === 100 ? '' : verdeM;
  }
  document.querySelector('#rowG .sem-sub').textContent = `> ${verdeM} m`;
  document.querySelector('#rowA .sem-sub').textContent = `${amareloM} – ${verdeM} m`;
  btnAmber.textContent = `Atenção (${amareloM}–${verdeM}m)`;
  btnCritico.textContent = `Crítico (<${amareloM}m) — teste dentro/fora da câmera`;
  rTextSubMap.seguro = `> ${verdeM} m`;
  rTextSubMap.atencao = `${amareloM} – ${verdeM} m`;
  rTextSubFallback = `< ${amareloM} m`;
}
