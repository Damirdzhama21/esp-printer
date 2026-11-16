/**
 * Сервер печати для платы ESP32-S3 у принтера.
 *
 * Плата раз в 4 с спрашивает GET /api/pechat/esp/vzyat и печатает то, что получила.
 * Сервер держит очередь, превращает PDF в PWG raster (ghostscript) и ждёт отчёт.
 *
 * Как положить задание:
 *   1) curl -X POST --data-binary @file.pdf -H "x-admin-token: $ADMIN_TOKEN" http://сервер:8080/api/print
 *   2) или, если задан TG_BOT_TOKEN, просто отправить PDF своему Telegram-боту.
 *
 * Без зависимостей: Node.js 18+ и ghostscript (gs) в системе.
 */
import http from 'node:http';
import { execFile } from 'node:child_process';
import { mkdir, writeFile, readFile, readdir, rm, stat } from 'node:fs/promises';
import { createReadStream } from 'node:fs';
import { join } from 'node:path';
import { timingSafeEqual } from 'node:crypto';

const PORT = Number(process.env.PORT || 8080);
const PRINTER_TOKEN = process.env.PRINTER_TOKEN || '';   // вводится на странице настройки платы
const ADMIN_TOKEN = process.env.ADMIN_TOKEN || '';       // для POST /api/print
const TG_BOT_TOKEN = process.env.TG_BOT_TOKEN || '';     // необязательно: печать PDF из Telegram
const TG_ALLOWED = (process.env.TG_ALLOWED_IDS || '').split(',').map((s) => s.trim()).filter(Boolean);
const PAPER = process.env.PAPER || 'a4';
const DATA = process.env.DATA_DIR || './data';
const LEASE_MS = 10 * 60 * 1000;   // плата взяла и молчит 10 мин — задание снова в очередь
const MAX_TRIES = 3;

if (PRINTER_TOKEN.length < 16) {
  console.log('❌ задайте PRINTER_TOKEN (не короче 16 символов)');
  process.exit(1);
}

/* ---------- очередь: каждое задание = папка data/<id>/ с in.pdf и job.json ---------- */
const jobs = new Map();   // id → { id, name, status: 'wait'|'printing'|'done'|'error', tries, leasedAt, error, chat }
let nextId = 1;

async function saveJob(j) {
  await writeFile(join(DATA, String(j.id), 'job.json'), JSON.stringify(j));
}

async function loadJobs() {
  await mkdir(DATA, { recursive: true });
  for (const d of await readdir(DATA)) {
    try {
      const j = JSON.parse(await readFile(join(DATA, d, 'job.json'), 'utf8'));
      if (j.status === 'printing') j.status = 'wait';   // сервер перезапустился посреди печати
      jobs.set(j.id, j);
      nextId = Math.max(nextId, j.id + 1);
    } catch { /* битая папка — пропускаем */ }
  }
  console.log(`✅ очередь загружена: ${[...jobs.values()].filter((j) => j.status === 'wait').length} ждут печати`);
}

async function addJob(pdf, name, chat = null) {
  if (pdf.subarray(0, 5).toString() !== '%PDF-') throw new Error('это не PDF');
  const j = { id: nextId++, name: name || 'документ', status: 'wait', tries: 0, leasedAt: 0, error: '', chat };
  await mkdir(join(DATA, String(j.id)), { recursive: true });
  await writeFile(join(DATA, String(j.id), 'in.pdf'), pdf);
  await saveJob(j);
  jobs.set(j.id, j);
  console.log(`📩 задание ${j.id}: «${j.name}»`);
  return j;
}

function nextJob() {
  const now = Date.now();
  for (const j of [...jobs.values()].sort((a, b) => a.id - b.id)) {
    if (j.status === 'printing' && now - j.leasedAt > LEASE_MS) j.status = 'wait';   // отчёт не пришёл
    if (j.status === 'wait') return j;
  }
  return null;
}

/* ---------- PDF → PWG raster ---------- */
function gs(args) {
  return new Promise((ok, ne) => execFile('gs', args, { timeout: 120000 }, (e, _o, err) =>
    (e ? ne(new Error(`gs: ${(err || e.message).toString().trim().slice(0, 200)}`)) : ok())));
}

async function toPwg(j) {
  const dir = join(DATA, String(j.id));
  const out = join(dir, 'out.pwg');
  await gs(['-q', '-dBATCH', '-dNOPAUSE', '-dSAFER', '-sDEVICE=pwgraster', '-r600',
    '-dcupsColorSpace=18', '-dcupsBitsPerColor=8', `-sPAPERSIZE=${PAPER}`, '-dFIXEDMEDIA', '-dPDFFitPage',
    `-sOutputFile=${out}`, join(dir, 'in.pdf')]);
  const { size } = await stat(out);
  if (size < 1800) throw new Error('gs: пустой растр');
  return { file: out, size };
}

/* ---------- Telegram (необязательно) ---------- */
async function tg(method, body) {
  if (!TG_BOT_TOKEN) return null;
  try {
    const r = await fetch(`https://api.telegram.org/bot${TG_BOT_TOKEN}/${method}`, {
      method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify(body),
    });
    return await r.json();
  } catch (e) {
    console.log(`⚠️ Telegram ${method}: ${e.message}`);
    return null;
  }
}

async function tgLoop() {
  let offset = 0;
  console.log('✅ Telegram-бот слушает');
  for (;;) {
    const r = await tg('getUpdates', { offset, timeout: 50, allowed_updates: ['message'] });
    if (!r?.ok) { await new Promise((s) => setTimeout(s, 5000)); continue; }
    for (const u of r.result) {
      offset = u.update_id + 1;
      const m = u.message;
      if (!m) continue;
      const chat = m.chat.id;
      if (!TG_ALLOWED.includes(String(m.from?.id))) {   // пусто = никому: впишите свой id из ответа бота
        await tg('sendMessage', { chat_id: chat, text: `Нет доступа. Ваш id: ${m.from?.id}` });
        continue;
      }
      const doc = m.document;
      if (!doc || doc.mime_type !== 'application/pdf') {
        await tg('sendMessage', { chat_id: chat, text: 'Пришлите PDF, я его напечатаю.' });
        continue;
      }
      try {
        const f = await tg('getFile', { file_id: doc.file_id });
        const res = await fetch(`https://api.telegram.org/file/bot${TG_BOT_TOKEN}/${f.result.file_path}`);
        const j = await addJob(Buffer.from(await res.arrayBuffer()), doc.file_name, chat);
        await tg('sendMessage', { chat_id: chat, text: `🖨 В очереди: ${j.name} (№${j.id})` });
      } catch (e) {
        console.log(`❌ Telegram-файл: ${e.message}`);
        await tg('sendMessage', { chat_id: chat, text: `Не получилось: ${e.message}` });
      }
    }
  }
}

/* ---------- HTTP ---------- */
function same(a, b) {
  const x = Buffer.from(String(a || '')), y = Buffer.from(b);
  return x.length === y.length && timingSafeEqual(x, y);
}

function json(res, code, obj) {
  res.writeHead(code, { 'content-type': 'application/json; charset=utf-8', 'cache-control': 'no-store' });
  res.end(JSON.stringify(obj));
}

function body(req, max) {
  return new Promise((ok, ne) => {
    const parts = [];
    let n = 0;
    req.on('data', (c) => { n += c.length; if (n > max) { ne(new Error('слишком большой')); req.destroy(); } else parts.push(c); });
    req.on('end', () => ok(Buffer.concat(parts)));
    req.on('error', ne);
  });
}

let lastBoardState = '';

const server = http.createServer(async (req, res) => {
  const path = new URL(req.url, 'http://x').pathname;
  try {
    /* --- положить PDF в очередь --- */
    if (path === '/api/print' && req.method === 'POST') {
      if (!ADMIN_TOKEN || !same(req.headers['x-admin-token'], ADMIN_TOKEN)) return json(res, 403, { error: 'нет доступа' });
      const j = await addJob(await body(req, 30 * 1024 * 1024), String(req.headers['x-name'] || ''));
      return json(res, 200, { id: j.id });
    }

    /* --- всё, что ниже, спрашивает плата --- */
    if (!path.startsWith('/api/pechat/esp/')) return json(res, 404, { error: 'нет такого адреса' });
    if (!same(req.headers['x-pechat-token'], PRINTER_TOKEN)) return json(res, 403, { error: 'неверный токен' });

    // плата в каждом запросе сообщает своё состояние: прошивка, принтер, тонер, ошибки
    const st = String(req.headers['x-plata'] || '').slice(0, 300);
    if (st && st !== lastBoardState) { lastBoardState = st; console.log(`📟 плата: ${st}`); }

    if (path === '/api/pechat/esp/pulse' && req.method === 'GET') return json(res, 200, { ok: true });

    if (path === '/api/pechat/esp/vzyat' && req.method === 'GET') {
      const j = nextJob();
      if (!j) { res.writeHead(204, { 'cache-control': 'no-store' }); return res.end(); }
      let r;
      try {
        r = await toPwg(j);
      } catch (e) {
        j.status = 'error'; j.error = e.message; await saveJob(j);
        console.log(`❌ задание ${j.id}: ${e.message}`);
        res.writeHead(204, { 'cache-control': 'no-store' }); return res.end();
      }
      j.status = 'printing'; j.leasedAt = Date.now(); j.tries += 1; await saveJob(j);
      console.log(`🖨 отдал плате задание ${j.id}, ${Math.round(r.size / 1024)} КБ`);
      res.writeHead(200, {
        'content-type': 'application/octet-stream', 'content-length': r.size,
        'x-zadanie-id': String(j.id), 'cache-control': 'no-store',
      });
      createReadStream(r.file).on('error', () => res.destroy()).pipe(res);
      return;
    }

    if (path === '/api/pechat/esp/gotovo' && req.method === 'POST') {
      const z = JSON.parse((await body(req, 4096)).toString() || '{}');
      const j = jobs.get(Number(z.id));
      if (!j || j.status !== 'printing') return json(res, 404, { error: 'нет такого задания' });
      if (z.ok) {
        j.status = 'done';
        console.log(`✅ напечатано задание ${j.id}`);
        if (j.chat) tg('sendMessage', { chat_id: j.chat, text: `✅ Напечатано: ${j.name}` });
      } else {
        j.error = String(z.oshibka || '').slice(0, 300);
        j.status = j.tries < MAX_TRIES ? 'wait' : 'error';
        console.log(`❌ задание ${j.id}, попытка ${j.tries}: ${j.error}`);
        if (j.status === 'error' && j.chat) tg('sendMessage', { chat_id: j.chat, text: `❌ Не напечаталось: ${j.name}\n${j.error}` });
      }
      await saveJob(j);
      await rm(join(DATA, String(j.id), 'out.pwg'), { force: true });
      return json(res, 200, { ok: true });
    }

    json(res, 404, { error: 'нет такого метода' });
  } catch (e) {
    console.log(`❌ ${path}: ${e.message}`);
    if (!res.headersSent) json(res, 500, { error: 'ошибка сервера' });
  }
});

await loadJobs();
server.listen(PORT, () => console.log(`✅ сервер печати на порту ${PORT}`));
if (TG_BOT_TOKEN) tgLoop();
