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

for (const id of ['terminal-panel', 'transcripts']) {
  const pane = document.getElementById(id);
  let movement;
  pane.addEventListener('softline:split-visibility', event => {
    const { axis, divider, finish } = event.detail;
    const dimension = axis === 'x' ? 'width' : 'height';
    const elements = [pane, divider];
    const sizes = elements.map(element => element.getBoundingClientRect()[dimension]);
    // Measure first so a close or reopen can pick up while the split is moving.
    pane.getAnimations().find(animation => animation.animationName === 'split-open')?.cancel();
    movement?.forEach(animation => animation.cancel());
    movement = null;
    if (reduced.matches || (finish && !sizes[0])) return;

    const targets = finish ? ['0px', '0px'] : ['var(--pane-size)', 'var(--pane-divider-size)'];
    const next = elements.map((element, index) => element.animate([
      { flexBasis: `${sizes[index]}px` }, { flexBasis: targets[index] }
    ], {
      id: finish ? 'split-close' : 'split-reopen',
      duration: 540, easing: 'cubic-bezier(.42, 0, .20, 1)', fill: 'both'
    }));
    movement = next;
    if (finish) event.preventDefault();
    const complete = () => {
      if (movement !== next) return;
      movement = null;
      finish?.();
      next.forEach(animation => animation.cancel());
    };
    Promise.all(next.map(animation => animation.finished)).then(complete, complete);
  });
  reduced.addEventListener('change', () => {
    if (reduced.matches) movement?.forEach(animation => animation.finish());
  });
}
