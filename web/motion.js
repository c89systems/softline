const chat = document.getElementById('chat');
const composer = document.getElementById('composer-area');
const reduced = window.matchMedia('(prefers-reduced-motion: reduce)');

chat.addEventListener('softline:submit', () => {
  if (reduced.matches || !chat.classList.contains('welcome-state')) return;
  const top = composer.getBoundingClientRect().top;
  // Measure the docked composer after the submit handler clears the draft.
  queueMicrotask(() => {
    const distance = top - composer.getBoundingClientRect().top;
    if (!distance || reduced.matches) return;
    composer.animate([{ translate: `0 ${distance}px` }, { translate: '0 0' }], {
      duration: 440,
      easing: 'cubic-bezier(.42, 0, .20, 1)'
    });
  });
});
