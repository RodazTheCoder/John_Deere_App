// Abas do PC
// ---------- TABS ----------
document.querySelectorAll('.tab-btn').forEach(btn=>{
  btn.addEventListener('click', ()=>{
    document.querySelectorAll('.tab-btn').forEach(b=>b.classList.toggle('active', b===btn));
    const v = btn.dataset.view;
    document.getElementById('viewPanel').style.display = v==='panel' ? 'flex' : 'none';
    document.getElementById('viewHistory').style.display = v==='history' ? 'block' : 'none';
    document.getElementById('viewEntidades').style.display = v==='entidades' ? 'block' : 'none';
  });
});
