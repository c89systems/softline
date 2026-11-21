const chat = document.getElementById('chat');
const composer = document.getElementById('composer-area');
const reduced = window.matchMedia('(prefers-reduced-motion: reduce)');

chat.addEventListener('softline:composer-dock', () => {
  if (reduced.matches || !chat.classList.contains('welcome-state')) return;
  const top = composer.getBoundingClientRect().top;
  // Measure the docked position after its layout changes take effect.
  queueMicrotask(() => {
    const distance = top - composer.getBoundingClientRect().top;
    if (!distance || reduced.matches) return;
    composer.animate([{ translate: `0 ${distance}px` }, { translate: '0 0' }], {
      duration: 540,
      easing: 'cubic-bezier(.42, 0, .20, 1)'
    });
  });
});
