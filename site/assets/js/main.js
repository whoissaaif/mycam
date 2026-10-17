/* MyCam site — behaviour.
 * Spec: docs/mycam-website.md §5 (Motion), §6 (Easter eggs), §7 (Loading)
 *
 * Rules this file follows:
 *  - Nothing here is required to read the page. If GSAP or Lenis fail to
 *    load, content stays visible and scrolling stays native.
 *  - prefers-reduced-motion turns effects off and keeps the layout.
 *  - Ambient loops only run while their section is on screen.
 *  - At most three smooth effects visible per screen.
 */
(function () {
  'use strict';

  var root = document.documentElement;
  var reduced = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  var hasGSAP = typeof window.gsap !== 'undefined';
  var hasST = hasGSAP && typeof window.ScrollTrigger !== 'undefined';
  var EASE = 'cubic-bezier(0.625, 0.05, 0, 1)';

  if (!hasGSAP) root.classList.add('gsap-failed');

  /* ---- 1. Loading is focusing (§7) ----------------------------------- */
  (function loader() {
    var el = document.getElementById('loader');
    if (!el) return;

    var seen = false;
    try { seen = sessionStorage.getItem('mycam-seen') === '1'; } catch (e) {}

    function finish() {
      document.body.classList.remove('is-focusing');
      document.body.classList.add('is-sharp');
      el.classList.add('is-done');
      window.setTimeout(function () { el.remove(); }, 400);
      try { sessionStorage.setItem('mycam-seen', '1'); } catch (e) {}
    }

    // Skipped entirely on repeat visits and with reduced motion (§7).
    if (seen || reduced) {
      el.remove();
      document.body.classList.add('is-sharp');
      return;
    }

    document.body.classList.add('is-focusing');

    var pct = document.getElementById('loader-pct');
    var start = performance.now();
    var DUR = 1200;          // under the 1.5s ceiling (§7)

    (function tick(now) {
      var t = Math.min(1, ((now || performance.now()) - start) / DUR);
      if (pct) pct.textContent = Math.round(t * 100);
      if (t < 1) { requestAnimationFrame(tick); return; }
      // Ring locks, then the page snaps from blurry to sharp.
      el.classList.add('is-locked');
      window.setTimeout(finish, 220);
    })();
  })();

  /* ---- 2. Smooth scroll + scroll-driven reveals (§5) ------------------ */
  var lenis = null;

  if (!reduced && typeof window.Lenis !== 'undefined') {
    lenis = new window.Lenis({
      duration: 1.05,
      easing: function (t) { return 1 - Math.pow(1 - t, 3); },
      smoothWheel: true
    });

    if (hasST) {
      lenis.on('scroll', window.ScrollTrigger.update);
      window.gsap.ticker.add(function (time) { lenis.raf(time * 1000); });
      window.gsap.ticker.lagSmoothing(0);
    } else {
      (function raf(time) { lenis.raf(time); requestAnimationFrame(raf); })(0);
    }

    // In-page links must still work with Lenis in charge.
    document.addEventListener('click', function (e) {
      var a = e.target.closest ? e.target.closest('a[href^="#"]') : null;
      if (!a) return;
      var id = a.getAttribute('href');
      if (!id || id === '#') return;
      var target = document.querySelector(id);
      if (!target) return;
      e.preventDefault();
      lenis.scrollTo(target, { offset: -80 });
    });
  }

  if (hasST && !reduced) {
    window.gsap.registerPlugin(window.ScrollTrigger);

    // §11 gotcha: never pre-set a CSS transform in % and then animate
    // yPercent. Use gsap.set + to with the same unit.
    window.gsap.utils.toArray('.reveal').forEach(function (el, i) {
      window.gsap.set(el, { opacity: 0, y: 18 });
      window.gsap.to(el, {
        opacity: 1,
        y: 0,
        duration: 0.5,
        delay: (i % 3) * 0.06,
        ease: 'power3.out',
        scrollTrigger: { trigger: el, start: 'top 85%', once: true }
      });
    });
  } else {
    // No GSAP, or reduced motion: show everything.
    var hidden = document.querySelectorAll('.reveal');
    for (var h = 0; h < hidden.length; h++) {
      hidden[h].style.opacity = '1';
      hidden[h].style.transform = 'none';
    }
  }

  /* ---- 2b. The demo loop is motion too ------------------------------- */
  (function demoVideo() {
    var v = document.getElementById('demo-video');
    if (!v) return;

    // Reduced motion: hold the poster frame instead of looping.
    if (reduced) {
      v.removeAttribute('autoplay');
      v.pause();
      v.setAttribute('controls', '');
      return;
    }

    // Only play while it is on screen (§5: ambient loops).
    if (!('IntersectionObserver' in window)) return;

    new IntersectionObserver(function (entries) {
      entries.forEach(function (entry) {
        if (entry.isIntersecting) { v.play().catch(function () {}); }
        else { v.pause(); }
      });
    }, { threshold: 0.2 }).observe(v);
  })();

  /* ---- 3. Header state ----------------------------------------------- */
  (function header() {
    var head = document.getElementById('site-head');
    if (!head) return;
    var onScroll = function () {
      head.classList.toggle('is-stuck', window.scrollY > 12);
    };
    onScroll();
    window.addEventListener('scroll', onScroll, { passive: true });
  })();

  /* ---- 4. REC timecode ----------------------------------------------- */
  (function timecode() {
    var el = document.getElementById('timecode');
    if (!el) return;
    var t0 = Date.now();
    var pad = function (n) { return n < 10 ? '0' + n : '' + n; };

    window.setInterval(function () {
      var s = Math.floor((Date.now() - t0) / 1000);
      el.textContent = pad(Math.floor(s / 3600)) + ':' +
                       pad(Math.floor(s / 60) % 60) + ':' + pad(s % 60);
    }, 1000);
  })();

  /* ---- 5. Autofocus cursor (§5) -------------------------------------- */
  (function cursor() {
    var el = document.getElementById('focus-cursor');
    if (!el || reduced) return;
    if (!window.matchMedia('(hover: hover)').matches) return;

    var x = window.innerWidth / 2, y = window.innerHeight / 2;
    var cx = x, cy = y, cw = 34, ch = 34, tw = 34, th = 34;
    var locked = null;

    window.addEventListener('pointermove', function (e) {
      x = e.clientX; y = e.clientY;
      el.style.opacity = '1';

      // Lock onto the nearest interactive element under the pointer.
      var hit = e.target.closest
        ? e.target.closest('a, button, summary, .compare-handle, .tab')
        : null;

      if (hit !== locked) {
        locked = hit;
        el.classList.toggle('is-locked', !!hit);
      }
    }, { passive: true });

    document.addEventListener('pointerleave', function () { el.style.opacity = '0'; });

    (function frame() {
      if (locked) {
        var r = locked.getBoundingClientRect();
        // Snap the box around the element, with a little breathing room.
        tw = Math.min(r.width + 18, window.innerWidth * 0.9);
        th = r.height + 14;
        x = r.left + r.width / 2;
        y = r.top + r.height / 2;
      } else {
        tw = 34; th = 34;
      }

      cx += (x - cx) * 0.22;
      cy += (y - cy) * 0.22;
      cw += (tw - cw) * 0.22;
      ch += (th - ch) * 0.22;

      el.style.transform = 'translate(' + cx + 'px,' + cy + 'px)';
      el.style.width = cw + 'px';
      el.style.height = ch + 'px';
      el.style.margin = (-ch / 2) + 'px 0 0 ' + (-cw / 2) + 'px';

      requestAnimationFrame(frame);
    })();
  })();

  /* ---- 6. Before / after slider -------------------------------------- */
  (function compare() {
    var box = document.getElementById('compare-widget');
    var before = document.getElementById('compare-before');
    var handle = document.getElementById('compare-handle');
    if (!box || !before || !handle) return;

    var pos = 50;
    var dragging = false;

    function paint() {
      before.style.clipPath = 'inset(0 ' + (100 - pos) + '% 0 0)';
      handle.style.left = pos + '%';
      handle.setAttribute('aria-valuenow', Math.round(pos));
    }

    function setFromEvent(e) {
      var r = box.getBoundingClientRect();
      pos = Math.max(0, Math.min(100, ((e.clientX - r.left) / r.width) * 100));
      paint();
    }

    handle.addEventListener('pointerdown', function (e) {
      dragging = true;
      handle.setPointerCapture(e.pointerId);
      e.preventDefault();
    });

    handle.addEventListener('pointermove', function (e) {
      if (dragging) setFromEvent(e);
    });

    handle.addEventListener('pointerup', function () { dragging = false; });
    handle.addEventListener('pointercancel', function () { dragging = false; });

    // Clicking anywhere in the frame jumps the divider there.
    box.addEventListener('pointerdown', function (e) {
      if (e.target === handle || handle.contains(e.target)) return;
      setFromEvent(e);
    });

    // Keyboard: the slider is a real control, not a mouse toy.
    handle.addEventListener('keydown', function (e) {
      var step = e.shiftKey ? 10 : 2;
      if (e.key === 'ArrowLeft') { pos = Math.max(0, pos - step); }
      else if (e.key === 'ArrowRight') { pos = Math.min(100, pos + step); }
      else if (e.key === 'Home') { pos = 0; }
      else if (e.key === 'End') { pos = 100; }
      else { return; }
      e.preventDefault();
      paint();
    });

    paint();
  })();

  /* ---- 7. "See the app": tabs + state cycle (§9) ---------------------- */
  (function appDemo() {
    var stage = document.getElementById('app-stage');
    var demo = document.getElementById('app-demo');
    if (!stage || !demo) return;

    var readout = document.getElementById('state-readout');
    var next = document.getElementById('state-next');
    var tabs = demo.querySelectorAll('.tab');
    var frames = demo.querySelectorAll('.frame');

    // The app's own words (§9). Per state, per frame.
    var COPY = {
      idle: {
        label: 'Not connected',
        pc: 'Not connected',
        android: { status: 'Not connected', hint: 'Plug this phone into a PC running MyCam.' },
        iphone: { status: 'Not connected', hint: 'Scan the code on your PC to connect.' }
      },
      live: {
        label: 'Streaming',
        pc: 'Streaming',
        android: { status: 'Streaming', hint: 'The camera turns on only while an app on the PC is using it.' },
        iphone: { status: 'Streaming', hint: 'Resolution, frame rate, zoom and torch are chosen on the PC.' }
      },
      paused: {
        label: 'Paused',
        pc: 'Paused',
        android: { status: 'Paused', hint: 'Turns the camera off until you resume, here or on the PC.' },
        iphone: { status: 'Paused', hint: 'The camera is off. Apps see a "Camera paused" picture.' }
      }
    };

    var ORDER = ['idle', 'live', 'paused'];
    var at = 0;
    var timer = null;

    function render() {
      var state = ORDER[at];
      var copy = COPY[state];
      stage.dataset.state = state;
      if (readout) readout.textContent = copy.label;

      // PC window
      var pcStatus = demo.querySelectorAll('#frame-pc [data-status]');
      for (var i = 0; i < pcStatus.length; i++) pcStatus[i].textContent = copy.pc;

      ['android', 'iphone'].forEach(function (name) {
        var frame = document.getElementById('frame-' + name);
        if (!frame) return;
        var s = frame.querySelector('[data-status]');
        var hint = frame.querySelector('[data-hint]');
        if (s) s.textContent = copy[name].status;
        if (hint) hint.textContent = copy[name].hint;
      });
    }

    function step() { at = (at + 1) % ORDER.length; render(); }

    function startCycle() {
      if (timer || reduced) return;
      timer = window.setInterval(step, 3600);
    }

    function stopCycle() {
      if (!timer) return;
      window.clearInterval(timer);
      timer = null;
    }

    if (next) {
      next.addEventListener('click', function () {
        stopCycle();          // taking manual control stops the carousel
        step();
      });
    }

    for (var t = 0; t < tabs.length; t++) {
      tabs[t].addEventListener('click', function (e) {
        var want = e.currentTarget.dataset.frame;

        for (var i = 0; i < tabs.length; i++) {
          var on = tabs[i].dataset.frame === want;
          tabs[i].classList.toggle('is-active', on);
          tabs[i].setAttribute('aria-selected', on ? 'true' : 'false');
        }

        for (var f = 0; f < frames.length; f++) {
          var show = frames[f].id === 'frame-' + want;
          frames[f].classList.toggle('is-active', show);
          if (show) frames[f].removeAttribute('hidden');
          else frames[f].setAttribute('hidden', '');
        }
      });
    }

    // Ambient loops only while on screen (§5).
    if ('IntersectionObserver' in window) {
      new IntersectionObserver(function (entries) {
        entries.forEach(function (entry) {
          stage.classList.toggle('is-rolling', entry.isIntersecting && !reduced);
          if (entry.isIntersecting) startCycle(); else stopCycle();
        });
      }, { threshold: 0.3 }).observe(stage);
    } else {
      stage.classList.add('is-rolling');
      startCycle();
    }

    render();
  })();

  /* ---- 8. App marquee ------------------------------------------------- */
  (function marquee() {
    var track = document.getElementById('marquee-track');
    if (!track || reduced) return;

    // Duplicate the row so the loop has no gap.
    track.innerHTML += track.innerHTML;

    var x = 0;
    var half = track.scrollWidth / 2;
    var running = false;

    if ('IntersectionObserver' in window) {
      new IntersectionObserver(function (entries) {
        running = entries[0].isIntersecting;
      }, { threshold: 0 }).observe(track);
    } else { running = true; }

    (function frame() {
      if (running) {
        x -= 0.45;
        if (half && -x >= half) x += half;
        track.style.transform = 'translateX(' + x + 'px)';
      }
      requestAnimationFrame(frame);
    })();
  })();

  /* ---- 9. Mobile visitors can't install a Windows app (§8) ----------- */
  (function handoff() {
    var box = document.getElementById('pc-handoff');
    var copy = document.getElementById('copy-link');
    if (!box) return;

    var isPhone = window.matchMedia('(max-width: 899px), (hover: none)').matches;
    var isWindows = /Windows NT/i.test(navigator.userAgent);
    if (isPhone || !isWindows) box.removeAttribute('hidden');

    if (!copy) return;
    copy.addEventListener('click', function () {
      var url = window.location.href.split('#')[0];
      var done = function () {
        copy.textContent = 'Link copied';
        window.setTimeout(function () { copy.textContent = 'Copy link for your PC'; }, 2200);
      };
      if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(url).then(done, done);
      } else { done(); }
    });
  })();

  /* ---- 10. Easter eggs (§6) ------------------------------------------ */
  (function eggs() {
    // A note for anyone who opens the console.
    try {
      console.log(
        '%cMyCam%c  open source, MIT. https://github.com/whoissaaif/mycam\n' +
        'Found something wrong on this page? Issues are welcome.',
        'background:#df2b1f;color:#fff;padding:2px 6px;font-weight:700',
        'color:#9aa0a9'
      );
    } catch (e) {}

    // Click the brand mark: the viewfinder takes a photo. Silent by default
    // (§6) — no sound without a deliberate gesture toward it.
    var mark = document.querySelector('.brand-mark');
    if (mark && !reduced) {
      mark.addEventListener('click', function (e) {
        e.preventDefault();
        var flash = document.createElement('div');
        flash.style.cssText = 'position:fixed;inset:0;background:#fff;z-index:400;' +
          'pointer-events:none;opacity:0;transition:opacity 90ms ' + EASE;
        document.body.appendChild(flash);
        requestAnimationFrame(function () {
          flash.style.opacity = '0.9';
          window.setTimeout(function () {
            flash.style.opacity = '0';
            window.setTimeout(function () { flash.remove(); }, 260);
          }, 90);
        });
      });
    }

    // Konami code flips the site to the app's XP skin for a few seconds.
    var SEQ = ['ArrowUp', 'ArrowUp', 'ArrowDown', 'ArrowDown', 'ArrowLeft',
               'ArrowRight', 'ArrowLeft', 'ArrowRight', 'b', 'a'];
    var seen = 0;

    window.addEventListener('keydown', function (e) {
      var want = SEQ[seen];
      var key = e.key.length === 1 ? e.key.toLowerCase() : e.key;
      seen = (key === want) ? seen + 1 : (key === SEQ[0] ? 1 : 0);

      if (seen < SEQ.length) return;
      seen = 0;
      root.classList.add('xp-mode');
      window.setTimeout(function () { root.classList.remove('xp-mode'); }, 4500);
    });
  })();
})();
