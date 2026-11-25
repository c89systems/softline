const chat = document.getElementById('chat');
const composer = document.getElementById('composer-area');
const welcome = document.getElementById('welcome');
const reduced = window.matchMedia('(prefers-reduced-motion: reduce)');
const scroll = document.getElementById('scroll');
const scrollFrame = document.getElementById('scroll-frame');

function updateScrollEdge() {
  const scrolled = scroll.scrollTop > 1 ? '1' : '0';
  if (scrolled !== scrollFrame.dataset.scrolled) scrollFrame.dataset.scrolled = scrolled;
}
scroll.addEventListener('scroll', updateScrollEdge, { passive: true });
updateScrollEdge();

let feedFrame = null, feedTop, feedTime;
function stopFeedMotion() {
  if (feedFrame !== null) window.cancelAnimationFrame(feedFrame);
  feedFrame = null;
}
function advanceFeed(time) {
  feedFrame = null;
  const top = scroll.scrollTop;
  if (top !== feedTop) return;
  const bottom = Math.max(0, scroll.scrollHeight - scroll.clientHeight);
  const remaining = bottom - top;
  if (reduced.matches || remaining <= 1 || remaining > scroll.clientHeight) {
    if (bottom !== top) scroll.scrollTop = bottom;
    return;
  }
  // Ease toward the latest height so each new line can extend the same motion.
  const step = Math.max(1, remaining * (1 - Math.exp(-Math.max(0, time - feedTime) / 38)));
  scroll.scrollTop = top + step;
  feedTop = scroll.scrollTop;
  feedTime = time;
  feedFrame = window.requestAnimationFrame(advanceFeed);
}
scroll.addEventListener('softline:feed-follow', event => {
  if (event.detail || reduced.matches) { stopFeedMotion(); return; }
  event.preventDefault();
  if (feedFrame !== null) return;
  feedTop = scroll.scrollTop;
  feedTime = performance.now();
  feedFrame = window.requestAnimationFrame(advanceFeed);
});
scroll.addEventListener('softline:feed-scroll', event => {
  if (feedFrame === null) return;
  if (scroll.scrollTop === feedTop) event.preventDefault();
  else stopFeedMotion();
});

let welcomeExit;
function cancelWelcomeExit() {
  if (!welcomeExit) return;
  const animation = welcomeExit;
  welcomeExit = null;
  delete welcome.dataset.leaving;
  welcome.inert = false;
  chat.insertBefore(welcome, composer);
  animation.cancel();
}
chat.addEventListener('softline:welcome-reset', cancelWelcomeExit);
reduced.addEventListener('change', () => {
  if (reduced.matches) cancelWelcomeExit();
});

chat.addEventListener('softline:composer-dock', () => {
  if (reduced.matches) return;
  const duration = 540;
  const top = composer.getBoundingClientRect().top;
  const bounds = welcome.getBoundingClientRect();
  if (bounds.height && bounds.bottom > 0) {
    const position = { left: `${bounds.left}px`, top: `${bounds.top}px`, width: `${bounds.width}px` };
    welcome.dataset.leaving = '';
    welcome.inert = true;
    // Let the banner travel past the chat pane and toolbar to the viewport edge.
    document.body.append(welcome);
    const exit = welcome.animate([
      { ...position, translate: '0 0', easing: 'cubic-bezier(.18, .65, .35, 1)' },
      { ...position, translate: '0 12px', offset: .27, easing: 'cubic-bezier(.55, 0, .88, .46)' },
      { ...position, translate: `0 -${Math.ceil(bounds.bottom + 4)}px` }
    ], { duration, fill: 'both' });
    welcomeExit = exit;
    const complete = () => { if (welcomeExit === exit) cancelWelcomeExit(); };
    exit.finished.then(complete, complete);
  }
  // Measure the docked position after its layout changes take effect.
  queueMicrotask(() => {
    const distance = top - composer.getBoundingClientRect().top;
    if (!distance || reduced.matches) return;
    composer.animate([{ translate: `0 ${distance}px` }, { translate: '0 0' }], {
      duration,
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
