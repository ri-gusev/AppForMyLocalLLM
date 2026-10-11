#include "web_chat.h"

namespace local_api {
const char* webChatPage() {
    return R"WEBCHAT(<!doctype html>
<html lang="ru">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="color-scheme" content="dark">
<title>Qwen · Local chat</title>
<style>
:root{color-scheme:dark;font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;background:#161719;color:#ececef;--muted:#93969e;--line:#303236;--accent:#b7edcf}
*{box-sizing:border-box}body{margin:0}button,input,textarea{font:inherit}button{cursor:pointer}button:disabled{cursor:default;opacity:.4}button:focus-visible,input:focus-visible,textarea:focus-visible{outline:2px solid var(--accent);outline-offset:3px}
.app{height:100vh;height:100dvh;display:flex;flex-direction:column;max-width:1100px;margin:auto}
header{display:flex;align-items:center;gap:12px;padding:16px 24px;border-bottom:1px solid var(--line);padding-top:max(16px,env(safe-area-inset-top));flex-shrink:0}.brand{flex:1}.brand strong{font-size:16px;font-weight:600}.brand small{display:block;color:var(--muted);font-size:12px;margin-top:3px}.dot{display:inline-block;width:7px;height:7px;border-radius:50%;background:var(--accent);margin-right:6px}
.quiet{border:1px solid var(--line);background:transparent;color:#c9cbd1;border-radius:10px;min-height:42px;padding:8px 12px;font-size:13px}.quiet:hover{background:#252629}
main{flex:1;min-height:0;overflow:auto;overscroll-behavior:contain;scroll-behavior:smooth;padding:24px}.thread{max-width:760px;margin:auto}.welcome{text-align:center;padding:clamp(35px,12vh,130px) 8px 30px}.mark{display:grid;place-items:center;width:56px;height:56px;border:1px solid #424b45;border-radius:18px;color:var(--accent);font-size:25px;margin:0 auto 22px;background:#202923}.welcome h1{font-size:clamp(24px,5vw,32px);font-weight:600;letter-spacing:-.8px;margin:0 0 14px}.welcome p{color:var(--muted);line-height:1.6;max-width:410px;margin:auto;font-size:14px}
.message{margin-bottom:28px;line-height:1.65;font-size:15px;overflow-wrap:anywhere}.message .label{color:var(--muted);font-size:12px;margin-bottom:7px}.message .text{white-space:pre-wrap}.message.user{margin-left:auto;max-width:85%;width:fit-content;background:#2b2d31;border-radius:20px 20px 5px 20px;padding:12px 18px}.message.user .label{display:none}.message.assistant{padding:0 3px}.message.assistant .label{color:var(--accent)}.message.failed .label{color:#f4b5b5;display:block}.waiting{color:var(--muted);display:flex;gap:8px;align-items:center}.pulse{width:8px;height:8px;background:var(--accent);border-radius:50%;animation:pulse 1.3s infinite}@keyframes pulse{50%{opacity:.3}}@media(prefers-reduced-motion:reduce){.pulse{animation:none}main{scroll-behavior:auto}}
footer{padding:8px 24px max(14px,env(safe-area-inset-bottom));flex-shrink:0}.composer-wrap{max-width:760px;margin:auto}.error{font-size:13px;color:#f4b5b5;line-height:1.5;margin:0 0 10px}.error:empty{display:none}.composer{border:1px solid #424449;border-radius:22px;background:#24262a;display:flex;align-items:flex-end;gap:10px;padding:12px}.composer:focus-within{border-color:#6c7d72}textarea{resize:none;flex:1;min-width:0;max-height:160px;min-height:28px;border:0;outline:none!important;background:transparent;color:#ececef;padding:4px 6px;line-height:1.5;font-size:16px}.send{border:0;border-radius:50%;height:38px;width:38px;flex-shrink:0;display:grid;place-items:center;background:var(--accent);color:#17281f;font-size:24px;font-weight:600}.meta{display:flex;justify-content:space-between;gap:12px;color:var(--muted);font-size:11px;margin:9px 8px 0;line-height:1.4}.over{color:#f4b5b5}
.auth-overlay{position:fixed;inset:0;background:#101113eb;display:flex;align-items:center;justify-content:center;padding:24px;z-index:2;overflow:auto}.auth-overlay[hidden]{display:none}.auth-card{width:100%;max-width:410px;background:#202225;border:1px solid #383b40;padding:28px;border-radius:24px;box-shadow:0 24px 90px #0005}.auth-card h2{font-size:23px;letter-spacing:-.5px;margin:0 0 12px}.auth-card p{font-size:14px;color:var(--muted);line-height:1.6;margin:0 0 22px}.auth-card label{display:block;font-size:13px;margin-bottom:9px}.auth-card input{width:100%;border:1px solid #494c51;background:#17181b;border-radius:12px;padding:13px;color:#ececef;font-size:16px}.connect{background:var(--accent);color:#17281f;font-weight:600;border:0;border-radius:12px;width:100%;min-height:46px;margin-top:16px}.auth-card .error{margin-top:12px}.auth-card small{display:block;color:var(--muted);font-size:12px;line-height:1.5;margin-top:18px}
@media(max-width:600px){header{padding-left:16px;padding-right:16px;gap:8px}header .quiet{padding:8px 10px}main{padding:24px 16px}footer{padding-left:12px;padding-right:12px}.meta{font-size:10px}.auth-card{padding:24px}.message.user{max-width:92%}}
</style>
</head>
<body>
<div class="app" id="app">
<header><div class="brand"><strong>Qwen3 <span style="color:var(--muted);font-weight:400">8B</span></strong><small><span class="dot"></span>Локальная модель</small></div><button class="quiet" id="clear" type="button">Новый чат</button><button class="quiet" id="change-key" type="button">API-ключ</button></header>
<main id="scroll"><div class="thread" id="thread" role="log" aria-live="polite" aria-label="История сообщений"><div class="welcome" id="welcome"><div class="mark" aria-hidden="true">Q</div><h1>О чём поговорим?</h1><p>Ваш чат с локальной Qwen3 8B.<br>История живёт в этой вкладке. Каждый запрос к модели обрабатывается отдельно.</p></div></div></main>
<footer><div class="composer-wrap"><p class="error" id="error" role="alert"></p><form class="composer" id="chat-form"><textarea id="prompt" rows="1" placeholder="Напишите сообщение…" aria-label="Сообщение" disabled></textarea><button class="send" id="send" type="submit" aria-label="Отправить сообщение" disabled>↑</button></form><div class="meta"><span id="status" role="status">Введите API-ключ для начала</span><span id="counter">0 / 4000 байт</span></div></div></footer>
</div>
<div class="auth-overlay" id="auth-overlay"><section class="auth-card" role="dialog" aria-modal="true" aria-labelledby="auth-title"><div class="mark" aria-hidden="true">Q</div><h2 id="auth-title">Подключиться к Qwen</h2><p>Введите Bearer API-ключ, заданный при запуске вашего сервера.</p><form id="auth-form" autocomplete="off"><label for="api-key">API-ключ</label><input id="api-key" type="password" autocomplete="off" autocapitalize="none" autocorrect="off" spellcheck="false" required maxlength="4096" placeholder="Ваш секретный ключ"><p class="error" id="auth-error" role="alert"></p><button class="connect" type="submit">Открыть чат</button></form><small>Ключ хранится только в памяти страницы. После обновления вкладки его нужно ввести снова.</small></section></div>
<script>
(() => {
  'use strict';
  const el = id => document.getElementById(id);
  const prompt = el('prompt'), send = el('send'), encoder = new TextEncoder();
  const welcome = el('welcome').cloneNode(true);
  let apiKey = '', busy = false, cooldownUntil = 0;
  const bytes = text => encoder.encode(text).length;
  function update() {
    const count = bytes(prompt.value), wait = Math.max(0, Math.ceil((cooldownUntil - Date.now()) / 1000));
    el('counter').textContent = count + ' / 4000 байт';
    el('counter').classList.toggle('over', count > 4000);
    send.disabled = !apiKey || busy || wait > 0 || !prompt.value.trim() || count > 4000;
    prompt.disabled = !apiKey;
    el('clear').disabled = busy;
    el('change-key').disabled = busy;
    el('status').textContent = busy ? 'Qwen готовит ответ…' : wait ? 'Повторная отправка через ' + wait + ' с' : apiKey ? (window.matchMedia('(pointer: coarse)').matches ? 'Enter — новая строка · ↑ — отправить' : 'Enter — отправить · Shift+Enter — новая строка') : 'Введите API-ключ для начала';
  }
  function resize() { prompt.style.height = 'auto'; prompt.style.height = Math.min(prompt.scrollHeight, 160) + 'px'; }
  function scrollDown() { el('scroll').scrollTop = el('scroll').scrollHeight; }
  function showAuth(message = '') {
    el('auth-error').textContent = message;
    el('auth-overlay').hidden = false;
    el('app').inert = true;
    el('api-key').value = '';
    el('api-key').focus();
  }
  function message(role, text) {
    el('welcome')?.remove();
    const item = document.createElement('article');
    item.className = 'message ' + role;
    const label = document.createElement('div'); label.className = 'label';
    label.textContent = role === 'user' ? 'Вы' : 'Qwen3 8B';
    const content = document.createElement('div'); content.className = 'text'; content.textContent = text;
    item.append(label, content); el('thread').append(item); scrollDown(); return item;
  }
  el('auth-form').addEventListener('submit', event => {
    event.preventDefault();
    const key = el('api-key').value;
    if (!key || key.length > 4096 || /[^\x21-\x7e]/.test(key)) {
      el('auth-error').textContent = 'Введите ключ из печатных ASCII-символов без пробелов.'; return;
    }
    apiKey = key;
    el('api-key').value = '';
    el('auth-overlay').hidden = true; el('app').inert = false;
    el('error').textContent = ''; update(); prompt.focus();
  });
  el('change-key').addEventListener('click', () => { apiKey = ''; update(); showAuth(); });
  el('clear').addEventListener('click', () => {
    el('thread').replaceChildren(welcome.cloneNode(true)); el('error').textContent = ''; prompt.value = ''; resize(); update(); prompt.focus();
  });
  prompt.addEventListener('input', () => {
    resize(); update();
    if (bytes(prompt.value) > 4000) el('error').textContent = 'Сообщение превышает 4000 UTF-8-байт. Сократите текст перед отправкой.';
    else if (el('error').textContent.startsWith('Сообщение превышает')) el('error').textContent = '';
  });
  prompt.addEventListener('keydown', event => {
    if (event.key === 'Enter' && !event.shiftKey && !event.isComposing && !window.matchMedia('(pointer: coarse)').matches) {
      event.preventDefault(); el('chat-form').requestSubmit();
    }
  });
  el('chat-form').addEventListener('submit', async event => {
    event.preventDefault();
    if (busy || !apiKey || cooldownUntil > Date.now()) return;
    const text = prompt.value;
    if (!text.trim()) return;
    const body = JSON.stringify({prompt: text});
    if (bytes(text) > 4000 || bytes(body) > 8192 || text.includes('\0')) {
      el('error').textContent = 'Сообщение слишком большое или содержит недопустимые символы. Лимит: 4000 UTF-8-байт, JSON — 8192 байта.'; return;
    }
    el('error').textContent = ''; busy = true;
    const user = message('user', text);
    const pending = document.createElement('div'); pending.className = 'message waiting';
    const dot = document.createElement('span'); dot.className = 'pulse'; dot.setAttribute('aria-hidden', 'true');
    pending.append(dot, document.createTextNode('Qwen думает…')); el('thread').append(pending);
    prompt.value = ''; resize(); update(); scrollDown();
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 240000);
    try {
      const response = await fetch('/chat', {
        method: 'POST', headers: {'Authorization': 'Bearer ' + apiKey, 'Content-Type': 'application/json'},
        body, signal: controller.signal, credentials: 'omit', cache: 'no-store', redirect: 'error'
      });
      const data = await response.json().catch(() => null);
      if (!response.ok) {
        if (response.status === 401) {
          apiKey = ''; showAuth('Ключ не принят сервером. Введите правильный API-ключ.');
          throw new Error('Нет доступа: проверьте API-ключ (401).');
        }
        if (response.status === 429) {
          const retry = Number(response.headers.get('Retry-After'));
          cooldownUntil = Date.now() + (Number.isFinite(retry) && retry > 0 ? Math.min(retry, 300) : 60) * 1000;
          throw new Error(data?.error?.code === 'generation_busy' ? 'Модель занята другим запросом. Попробуйте чуть позже (429).' : 'Лимит: 5 запросов за 60 секунд на весь сервер. Подождите перед повторной отправкой (429).');
        }
        if (response.status === 413) throw new Error('Превышен лимит: сообщение до 4000 UTF-8-байт, тело запроса до 8192 байт (413).');
        if (response.status === 502) throw new Error('Модель недоступна. Проверьте Ollama и наличие qwen3:8b (502).');
        if (response.status === 504) throw new Error('Модель не успела ответить. Попробуйте позже (504).');
        throw new Error('Сервер отклонил запрос (HTTP ' + response.status + '). Попробуйте позже.');
      }
      if (!data || typeof data.response !== 'string') throw new Error('Сервер вернул ответ в неожиданном формате.');
      pending.remove(); message('assistant', data.response);
    } catch (error) {
      user.classList.add('failed'); user.querySelector('.label').textContent = 'Ответ не получен';
      el('error').textContent = error.name === 'AbortError' ? 'Ожидание превысило 4 минуты. Запрос на сервере может ещё выполняться.' : error instanceof TypeError ? 'Не удалось связаться с сервером. Проверьте подключение и Cloudflare Tunnel.' : error.message;
      if (!prompt.value) prompt.value = text;
    } finally {
      clearTimeout(timeout); pending.remove(); busy = false; resize(); update(); scrollDown();
    }
  });
  // Track the iPhone visual viewport when its keyboard opens.
  if (window.visualViewport) {
    const fit = () => { el('app').style.height = window.visualViewport.height + 'px'; };
    window.visualViewport.addEventListener('resize', fit); fit();
  }
  setInterval(update, 1000);
  window.addEventListener('pagehide', () => { apiKey = ''; el('api-key').value = ''; });
  window.addEventListener('pageshow', () => { if (!apiKey) { update(); showAuth(); } });
  update(); showAuth();
})();
</script>
</body>
</html>)WEBCHAT";
}
}
