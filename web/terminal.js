/* SPDX-License-Identifier: GPL-3.0-or-later */
(() => {
  'use strict';
  const MAX_INPUT_BYTES = 64 * 1024;
  const MAX_OUTPUT_BYTES = 32 * 1024;
  const MAX_CLIPBOARD_BYTES = 64 * 1024;
  const MAX_FONT_FAMILY_CHARS = 256;
  const MIN_FONT_SIZE = 6;
  const MAX_FONT_SIZE = 48;
  const MIN_SCROLLBACK = 0;
  const MAX_SCROLLBACK = 20000;
  const DEFAULT_SETTINGS = {
    fontFamily: 'Cascadia Mono, Consolas, monospace', fontSize: 13,
    scrollback: 5000
  };
  const DEFAULT_THEME = {
    background: '#1e1e1e', foreground: '#d4d4d4', cursor: '#d4d4d4',
    cursorAccent: '#1e1e1e', selectionBackground: '#264f78'
  };
  const bridge = window.chrome?.webview;
  const status = document.getElementById('status');
  if (!bridge) { status.textContent = 'Open this page through the NppTerminal plugin.'; return; }
  const terminal = new Terminal({
    ...DEFAULT_SETTINGS, allowProposedApi: false, disableStdin: true,
    theme: { ...DEFAULT_THEME }
  });
  const fit = new FitAddon.FitAddon();
  terminal.loadAddon(fit);
  terminal.open(document.getElementById('terminal'));
  // Never honor shell-originated OSC 52 clipboard writes.
  terminal.parser.registerOscHandler(52, () => true);
  let generation = null;
  let state = 'NoSession';
  let nextInputId = 1;
  let inFlightInput = null;
  let pendingInput = [];
  let pendingBytes = 0;
  let paused = false;
  let stateMessage = '';
  const encoder = new TextEncoder();
  const post = message => bridge.postMessage({ ...message, generation });
  const validString = (value, maximum) => typeof value === 'string' &&
    value.length > 0 && value.length <= maximum && !/[\u0000-\u001f\u007f]/.test(value);
  const validColor = value => typeof value === 'string' && value.length <= 128 &&
    /^(?:#[0-9a-f]{3,8}|rgba?\([^\r\n]{1,112}\)|hsla?\([^\r\n]{1,112}\)|transparent)$/i.test(value);
  const validTheme = value => {
    if (!value || typeof value !== 'object' || Array.isArray(value)) return false;
    const keys = ['background', 'foreground', 'cursor', 'cursorAccent', 'selectionBackground'];
    return keys.some(key => Object.prototype.hasOwnProperty.call(value, key)) &&
      keys.every(key => value[key] === undefined || validColor(value[key]));
  };
  const applyTheme = value => {
    if (!validTheme(value)) return false;
    const theme = { ...DEFAULT_THEME, ...value };
    terminal.options.theme = theme;
    const root = document.documentElement;
    if (root?.style) {
      root.style.setProperty('--terminal-background', theme.background);
      root.style.setProperty('--terminal-foreground', theme.foreground);
      root.style.setProperty('--terminal-status-background', theme.background);
    }
    return true;
  };
  const applySettings = value => {
    if (!value || typeof value !== 'object' || Array.isArray(value)) return false;
    const next = {};
    if (value.fontFamily !== undefined) {
      if (!validString(value.fontFamily, MAX_FONT_FAMILY_CHARS) || /[{};<>`]/.test(value.fontFamily)) return false;
      next.fontFamily = value.fontFamily;
    }
    if (value.fontSize !== undefined) {
      if (!Number.isSafeInteger(value.fontSize) || value.fontSize < MIN_FONT_SIZE || value.fontSize > MAX_FONT_SIZE) return false;
      next.fontSize = value.fontSize;
    }
    if (value.scrollback !== undefined) {
      if (!Number.isSafeInteger(value.scrollback) || value.scrollback < MIN_SCROLLBACK || value.scrollback > MAX_SCROLLBACK) return false;
      next.scrollback = value.scrollback;
    }
    if (value.theme !== undefined && !validTheme(value.theme)) return false;
    Object.assign(terminal.options, next);
    if (value.theme !== undefined) applyTheme(value.theme);
    fitNow();
    return true;
  };
  const sendClipboardCopy = () => {
    const selection = typeof terminal.getSelection === 'function' ? terminal.getSelection() : '';
    if (!selection) return;
    if (encoder.encode(selection).length > MAX_CLIPBOARD_BYTES) {
      status.textContent = 'Selection is too large (maximum 64 KiB).';
      return;
    }
    post({ type: 'clipboardCopy', data: selection });
  };
  const requestClipboardPaste = () => post({ type: 'clipboardRead' });
  const updateInputFlow = () => {
    if (!paused && (pendingBytes >= 48 * 1024 || pendingInput.length >= 192)) paused = true;
    if (paused && pendingBytes < 16 * 1024 && pendingInput.length < 64) paused = false;
    terminal.options.disableStdin = state !== 'Running' || paused;
    status.textContent = paused ? 'Input paused while the shell catches up…' : stateMessage;
  };
  const flushInput = () => {
    if (inFlightInput || !pendingInput.length || state !== 'Running') return;
    const item = pendingInput.shift();
    pendingBytes -= item.bytes;
    inFlightInput = { ...item, id: nextInputId++ };
    post({ type: item.type, data: item.data, id: inFlightInput.id });
    updateInputFlow();
  };
  const enqueueInput = (type, data, bytes) => {
    if (state !== 'Running' || generation === null) return;
    if (bytes === 0) return;
    if (bytes > MAX_INPUT_BYTES) {
      status.textContent = 'Paste is too large (maximum 64 KiB).';
      return;
    }
    if (pendingBytes + bytes > MAX_INPUT_BYTES || pendingInput.length >= 256) {
      status.textContent = 'Input queue is full. Wait for the shell, then try again.';
      return;
    }
    const tail = pendingInput.at(-1);
    if (tail && tail.type === type) { tail.data += data; tail.bytes += bytes; }
    else pendingInput.push({ type, data, bytes });
    pendingBytes += bytes;
    flushInput();
    updateInputFlow();
  };
  const grid = () => ({ cols: terminal.cols, rows: terminal.rows });
  const fitNow = () => {
    const host = document.getElementById('terminal');
    if (!host.clientWidth || !host.clientHeight) return;
    fit.fit();
    if (generation !== null) post({ type: 'resize', ...grid() });
  };
  new ResizeObserver(fitNow).observe(document.getElementById('terminal'));
  terminal.onData(data => {
    enqueueInput('input', data, new TextEncoder().encode(data).length);
  });
  terminal.onBinary(data => {
    enqueueInput('binary', data, data.length);
  });
  if (typeof terminal.attachCustomKeyEventHandler === 'function') {
    terminal.attachCustomKeyEventHandler(event => {
      if (event.type !== 'keydown' || !event.ctrlKey || !event.shiftKey || event.altKey) return true;
      if (event.key.toLowerCase() === 'c') { sendClipboardCopy(); return false; }
      if (event.key.toLowerCase() === 'v') { requestClipboardPaste(); return false; }
      return true;
    });
  }
  bridge.addEventListener('message', event => {
    const message = event.data;
    if (!message || typeof message !== 'object' || Array.isArray(message)) return;
    if (message.type === 'init') {
      if (!Number.isSafeInteger(message.generation) || message.generation <= 0) return;
      generation = message.generation;
      state = 'Starting';
      stateMessage = 'Starting terminal…';
      nextInputId = 1;
      inFlightInput = null;
      pendingInput = [];
      pendingBytes = 0;
      paused = false;
      terminal.options.disableStdin = true;
      terminal.reset();
      if (message.settings !== undefined) applySettings(message.settings);
      if (message.theme !== undefined) applyTheme(message.theme);
      fitNow();
      post({ type: 'ready', ...grid() });
      return;
    }
    if (generation === null || message.generation !== generation) return;
    if (message.type === 'output') {
      if (!Number.isSafeInteger(message.id) || message.id < 0 || typeof message.data !== 'string' || message.data.length > Math.ceil(MAX_OUTPUT_BYTES / 3) * 4) return;
      let bytes;
      try { bytes = Uint8Array.from(atob(message.data), character => character.charCodeAt(0)); }
      catch { return; }
      if (bytes.length > MAX_OUTPUT_BYTES) return;
      const outputGeneration = generation;
      terminal.write(bytes, () => {
        if (generation === outputGeneration) post({ type: 'ack', id: message.id });
      });
    } else if (message.type === 'inputAck') {
      if (!inFlightInput || message.id !== inFlightInput.id) return;
      inFlightInput = null;
      flushInput();
      updateInputFlow();
    } else if (message.type === 'clipboard') {
      if (typeof message.data !== 'string' || encoder.encode(message.data).length > MAX_CLIPBOARD_BYTES) return;
      if (typeof terminal.paste === 'function') terminal.paste(message.data);
    } else if (message.type === 'settings') {
      if (message.settings === undefined || !applySettings(message.settings)) return;
    } else if (message.type === 'theme') {
      if (!applyTheme(message.theme)) return;
    } else if (message.type === 'focus') {
      terminal.focus();
    } else if (message.type === 'state') {
      if (!['NoSession', 'Starting', 'Running', 'Stopping', 'Exited', 'Error'].includes(message.state)) return;
      state = message.state;
      stateMessage = typeof message.message === 'string' ? message.message.slice(0, 2048) : state;
      updateInputFlow();
      if (state === 'Running') terminal.focus();
    } else if (message.type === 'clear') {
      terminal.clear();
    }
  });
})();
