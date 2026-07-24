/* bookboy advance web converter.
 * The browser does EPUB (zip + DOMParser); the wasm core does typesetting.
 * No uploads: everything runs locally.
 */
'use strict';

const ROM_LIMIT = 32 * 1024 * 1024;
const COVER_BYTES = 240 * 160 * 2;
const K = {
  BODY: 0,
  TITLE: 1,
  SUBTITLE: 2,
  BYLINE: 3,
  PART: 4,
  PARTSUB: 5,
  DISPLAY: 6,
};
const CLASS_KIND = {
  fmh: K.TITLE,
  cst: K.SUBTITLE,
  ctag: K.BYLINE,
  pt: K.PART,
  fmsh: K.PARTSUB,
  center: K.DISPLAY,
  ct: K.TITLE,
};

let core = null; // wasm module
let books = []; // {name,title,chapters,coverRGB,coverW,coverH,npages,blobSize,failed}
let selected = 0; // preview book index
let curPage = 0;
let assetsCache = null; // {stub, title}
let busy = false;

const $ = (id) => document.getElementById(id);
const cc = (...a) => core.ccall(...a);

/* ---------------- zip reading (built-ins only) ---------------- */
async function unzip(buf) {
  const dv = new DataView(buf);
  const u8 = new Uint8Array(buf);
  // find EOCD
  let eocd = -1;
  for (
    let i = buf.byteLength - 22;
    i >= Math.max(0, buf.byteLength - 66000);
    i--
  )
    if (dv.getUint32(i, true) === 0x06054b50) {
      eocd = i;
      break;
    }
  if (eocd < 0) throw new Error('not a zip');
  const count = dv.getUint16(eocd + 10, true);
  let off = dv.getUint32(eocd + 16, true);
  const entries = new Map();
  const td = new TextDecoder();
  for (let i = 0; i < count; i++) {
    if (dv.getUint32(off, true) !== 0x02014b50) break;
    const method = dv.getUint16(off + 10, true);
    const csize = dv.getUint32(off + 20, true);
    const nlen = dv.getUint16(off + 28, true);
    const elen = dv.getUint16(off + 30, true);
    const clen = dv.getUint16(off + 32, true);
    const lho = dv.getUint32(off + 42, true);
    const name = td.decode(u8.subarray(off + 46, off + 46 + nlen));
    entries.set(name, { method, csize, lho });
    off += 46 + nlen + elen + clen;
  }
  return {
    names: [...entries.keys()],
    async read(name) {
      const e = entries.get(name);
      if (!e) return null;
      const nlen = dv.getUint16(e.lho + 26, true);
      const elen = dv.getUint16(e.lho + 28, true);
      const start = e.lho + 30 + nlen + elen;
      const slice = u8.subarray(start, start + e.csize);
      if (e.method === 0) return slice.slice();
      const ds = new DecompressionStream('deflate-raw');
      const resp = new Response(new Blob([slice]).stream().pipeThrough(ds));
      return new Uint8Array(await resp.arrayBuffer());
    },
  };
}

/* ---------------- epub parsing ---------------- */
function parseXml(bytes, mime) {
  const text = new TextDecoder().decode(bytes);
  const doc = new DOMParser().parseFromString(text, mime);
  return doc.querySelector('parsererror') ? null : doc;
}

function dirOf(p) {
  const i = p.lastIndexOf('/');
  return i < 0 ? '' : p.slice(0, i + 1);
}
function joinPath(base, href) {
  const parts = (base + decodeURIComponent(href)).split('/');
  const out = [];
  for (const p of parts) {
    if (p === '..') out.pop();
    else if (p !== '.' && p !== '') out.push(p);
  }
  return out.join('/');
}

/* DOM -> the same paragraph state machine as the C parser */
function extractParas(doc) {
  const paras = [];
  let cur = null,
    italic = 0;

  const flush = (keep) => {
    if (!cur) return;
    if (cur.runs.some((r) => r.text.trim() !== '')) paras.push(cur);
    cur = keep ? { kind: cur.kind, hint: -1, runs: [] } : null;
  };
  const open = (kind, el) => {
    const cls = (el.getAttribute('class') || '').trim();
    if (kind === K.BODY && cls in CLASS_KIND) kind = CLASS_KIND[cls];
    let hint = -1;
    if (kind === K.BODY) {
      if (cls.endsWith('tx1')) hint = 1;
      else if (cls.endsWith('tx')) hint = 0;
    }
    cur = { kind, hint, runs: [] };
    italic = 0;
  };

  const walk = (node) => {
    if (node.nodeType === Node.TEXT_NODE) {
      if (cur && node.data)
        cur.runs.push({ italic: italic > 0, text: node.data });
      return;
    }
    if (node.nodeType !== Node.ELEMENT_NODE) return;
    const tag = node.localName.toLowerCase();
    if (tag === 'p' || tag === 'div') {
      open(K.BODY, node);
      for (const ch of node.childNodes) walk(ch);
      flush(false);
    } else if (/^h[1-6]$/.test(tag)) {
      open(tag <= 'h2' ? K.TITLE : tag === 'h3' ? K.SUBTITLE : K.DISPLAY, node);
      for (const ch of node.childNodes) walk(ch);
      flush(false);
    } else if (tag === 'em' || tag === 'i' || tag === 'cite') {
      if (cur) italic++;
      for (const ch of node.childNodes) walk(ch);
      if (italic) italic--;
    } else if (tag === 'br') {
      if (cur) flush(true);
    } else if (tag === 'script' || tag === 'style' || tag === 'head') {
      /* skip */
    } else {
      for (const ch of node.childNodes) walk(ch);
    }
  };
  doc.normalize();
  walk(doc.body || doc.documentElement);
  return paras;
}

async function parseEpub(file) {
  const zip = await unzip(await file.arrayBuffer());
  const containerBytes = await zip.read('META-INF/container.xml');
  let opfPath = 'content.opf';
  if (containerBytes) {
    const cdoc = parseXml(containerBytes, 'text/xml');
    const rf = cdoc && cdoc.querySelector('rootfile');
    if (rf) opfPath = rf.getAttribute('full-path');
  }
  const opfBytes = await zip.read(opfPath);
  if (!opfBytes) throw new Error('no OPF');
  const opf = parseXml(opfBytes, 'text/xml');
  if (!opf) throw new Error('bad OPF');
  const base = dirOf(opfPath);

  const items = new Map();
  for (const it of opf.querySelectorAll('manifest > item'))
    items.set(it.getAttribute('id'), {
      href: it.getAttribute('href') || '',
      type: it.getAttribute('media-type') || '',
    });

  const titleEl =
    opf.getElementsByTagName('dc:title')[0] || opf.querySelector('title');
  let title = titleEl ? titleEl.textContent.replace(/\s+/g, ' ').trim() : '';
  if (!title) title = file.name.replace(/\.epub$/i, '').split(' -- ')[0];

  // cover image
  let coverHref = null;
  const covMeta = opf.querySelector('meta[name="cover"]');
  if (covMeta && items.has(covMeta.getAttribute('content')))
    coverHref = items.get(covMeta.getAttribute('content')).href;
  if (!coverHref)
    for (const [id, it] of items)
      if (
        /\.(jpe?g|png)$/i.test(it.href) &&
        (/cover/i.test(it.href) || /cover/i.test(id))
      ) {
        coverHref = it.href;
        break;
      }

  const chapters = [];
  for (const ref of opf.querySelectorAll('spine > itemref')) {
    if (ref.getAttribute('linear') === 'no') continue;
    const it = items.get(ref.getAttribute('idref'));
    if (!it) continue;
    const isDoc =
      /html|oeb1-document/.test(it.type) ||
      /\.(x?html?|xml|xhtm)$/i.test(it.href);
    if (!isDoc) continue;
    const bytes = await zip.read(joinPath(base, it.href));
    if (!bytes) continue;
    const doc =
      parseXml(bytes, 'application/xhtml+xml') ||
      new DOMParser().parseFromString(
        new TextDecoder().decode(bytes),
        'text/html',
      );
    const paras = extractParas(doc);
    if (paras.length) chapters.push(paras);
  }

  let coverRGB = null,
    coverW = 0,
    coverH = 0;
  if (coverHref) {
    try {
      const cbytes = await zip.read(joinPath(base, coverHref));
      const bmp = await createImageBitmap(new Blob([cbytes]));
      const cv = document.createElement('canvas');
      cv.width = bmp.width;
      cv.height = bmp.height;
      const ctx = cv.getContext('2d');
      ctx.fillStyle = '#fff';
      ctx.fillRect(0, 0, cv.width, cv.height);
      ctx.drawImage(bmp, 0, 0);
      const img = ctx.getImageData(0, 0, cv.width, cv.height).data;
      coverRGB = new Uint8Array(bmp.width * bmp.height * 3);
      for (let i = 0, j = 0; i < img.length; i += 4, j += 3) {
        coverRGB[j] = img[i];
        coverRGB[j + 1] = img[i + 1];
        coverRGB[j + 2] = img[i + 2];
      }
      coverW = bmp.width;
      coverH = bmp.height;
    } catch (e) {
      /* no cover, fine */
    }
  }
  const nchars = chapters
    .flat()
    .reduce((a, p) => a + p.runs.reduce((b, r) => b + r.text.length, 0), 0);
  if (nchars < 4000) throw new Error('no readable text found');
  return { name: file.name, title, chapters, coverRGB, coverW, coverH };
}

/* ---------------- wasm orchestration ---------------- */
async function initCore() {
  if (core) return;
  setStatus('loading typesetting engine…');
  core = await createBookboyCore();
  core.FS.mkdir('/fonts');
  for (const f of [
    'XCharter-Roman.otf',
    'XCharter-Italic.otf',
    'XCharter-Bold.otf',
  ])
    core.FS.writeFile(
      '/fonts/' + f,
      new Uint8Array(await (await fetch('assets/' + f)).arrayBuffer()),
    );
  core.FS.writeFile(
    '/hyph_en_US.dic',
    new Uint8Array(await (await fetch('assets/hyph_en_US.dic')).arrayBuffer()),
  );
}

function feedBook(b) {
  const bi = cc('bw_book_begin', 'number', ['string'], [b.title]);
  for (const paras of b.chapters) {
    cc('bw_chapter_break', null, [], []);
    for (const p of paras) {
      cc('bw_para_begin', null, ['number', 'number'], [p.kind, p.hint]);
      for (const r of p.runs) {
        const bytes = new TextEncoder().encode(r.text);
        const ptr = core._malloc(bytes.length || 1);
        core.HEAPU8.set(bytes, ptr);
        cc(
          'bw_para_run',
          null,
          ['number', 'number', 'number'],
          [r.italic ? 1 : 0, ptr, bytes.length],
        );
        core._free(ptr);
      }
      cc('bw_para_end', null, [], []);
    }
  }
  return cc('bw_book_end', 'number', [], []);
}

async function retypeset() {
  if (!books.length || busy) return;
  busy = true;
  try {
    await initCore();
    setStatus('typesetting…');
    setBuild(''); // the built ROM (if any) is now stale
    if (emuLoaded) {
      // the booted emulator predates this change
      const h = $('emuhint');
      if (h)
        h.textContent =
          'this shows an earlier build — refresh the page to preview your latest library';
    }
    await new Promise((r) => setTimeout(r, 20)); // let UI paint
    cc('bw_reset', null, [], []);
    for (const b of books) feedBook(b);
    const px = parseInt($('fontsize').value, 10);
    cc('bw_prepare', 'number', ['number', 'number'], [px, 72]);
    const wantCovers = $('covers').checked;
    books.forEach((b, i) => {
      let coverPtr = 0;
      if (wantCovers && b.coverRGB) {
        coverPtr = core._malloc(b.coverRGB.length);
        core.HEAPU8.set(b.coverRGB, coverPtr);
      }
      b.npages = cc(
        'bw_layout',
        'number',
        ['number', 'number', 'number', 'number'],
        [i, coverPtr, b.coverW, b.coverH],
      );
      if (coverPtr) core._free(coverPtr);
      b.blobSize = cc('bw_book_blob_size', 'number', ['number'], [i]);
    });
    curPage = Math.min(curPage, (books[selected]?.npages || 1) - 1);
    renderShelf();
    renderPreview();
    setStatus('');
  } catch (e) {
    setStatus('error: ' + e.message);
  } finally {
    busy = false;
    updateBar(); // re-enable the build button now that busy is clear
  }
}

/* ---------------- UI ---------------- */
function setStatus(msg) {
  $('status').textContent = msg;
}
function setBuild(msg) {
  // build result, shown under the stack
  $('buildmsg').textContent = msg;
}

function totalBytes() {
  const per = books.reduce(
    (a, b) =>
      a +
      (b.blobSize ? (b.blobSize + 3) & ~3 : 0) +
      ($('covers').checked && b.coverRGB ? COVER_BYTES + 10000 : 0) +
      96,
    0,
  );
  return per + 400 * 1024; // stub+font+mini+title+slack
}

function fmtMB(n) {
  return (n / 1024 / 1024).toFixed(1) + ' MB';
}

function renderShelf() {
  const shelf = $('shelf');
  shelf.innerHTML = '';
  books.forEach((b, i) => {
    const row = document.createElement('div');
    row.className = 'book' + (i === selected ? ' sel' : '');
    row.innerHTML =
      `<span class="btitle"></span>` +
      `<span class="bmeta">${b.npages ? b.npages + ' pages' : '…'}` +
      `${b.blobSize ? ' · ' + fmtMB(b.blobSize) : ''}</span>` +
      `<button class="del" title="remove">×</button>`;
    row.querySelector('.btitle').textContent = b.title;
    row.onclick = () => {
      selected = i;
      curPage = 0;
      renderShelf();
      renderPreview();
    };
    row.querySelector('.del').onclick = (e) => {
      e.stopPropagation();
      books.splice(i, 1);
      selected = Math.min(selected, books.length - 1);
      retypeset().then(updateBar);
      renderShelf();
      updateBar();
    };
    shelf.appendChild(row);
  });
  $('shelfwrap').style.display = books.length ? '' : 'none';
  $('buildrow').style.display = books.length ? '' : 'none';
  updateBar();
}

function updateBar() {
  const t = totalBytes();
  const pct = Math.min(100, (t * 100) / ROM_LIMIT);
  $('barfill').style.width = pct + '%';
  $('barfill').classList.toggle('over', t > ROM_LIMIT);
  $('barlabel').textContent =
    `${fmtMB(t)} of 32 MB` + (t > ROM_LIMIT ? ' — too big, remove a book' : '');
  $('build').disabled = t > ROM_LIMIT || !books.length || busy;
}

function renderPreview() {
  const b = books[selected];
  const wrap = $('previewwrap');
  if (!b || !b.npages || b.npages <= 0) {
    wrap.style.display = 'none';
    return;
  }
  wrap.style.display = '';
  const cv = $('gba');
  const ctx = cv.getContext('2d');
  const ptr = core._malloc(240 * 160 * 4);
  const ok = cc(
    'bw_render_page',
    'number',
    ['number', 'number', 'number'],
    [selected, curPage, ptr],
  );
  if (ok) {
    const img = new ImageData(
      new Uint8ClampedArray(core.HEAPU8.buffer, ptr, 240 * 160 * 4).slice(),
      240,
      160,
    );
    ctx.putImageData(img, 0, 0);
  }
  core._free(ptr);
  $('pageno').textContent = `${curPage + 1} / ${b.npages}`;
}

/* ---------------- ROM assembly (JS rompack) ---------------- */
async function fetchAssets() {
  if (assetsCache) return assetsCache;
  const [stub, title, art] = await Promise.all([
    fetch('assets/stub.gba').then((r) => r.arrayBuffer()),
    fetch('assets/title.bin').then((r) => r.arrayBuffer()),
    fetch('assets/art.bin').then((r) => r.arrayBuffer()),
  ]);
  assetsCache = {
    stub: new Uint8Array(stub),
    title: new Uint8Array(title),
    art: new Uint8Array(art),
  };
  return assetsCache;
}

function part(which) {
  const size = cc('bw_part_size', 'number', ['number'], [which]);
  const ptr = cc('bw_part_data', 'number', ['number'], [which]);
  return core.HEAPU8.slice(ptr, ptr + size);
}

/* Finalize the wasm layout and concatenate stub + data table + blobs into a
 * complete .gba — the same JS "rompack". Returns {rom, romSize}. */
async function assembleRom() {
  const total = cc('bw_finalize', 'number', [], []);
  if (total < 0) throw new Error('layout incomplete');
  const { stub, title, art } = await fetchAssets();
  const blobs = [part(0), part(1), part(2), part(3), title, art];

  const tableAt = (stub.length + 255) & ~255;
  const align4 = (n) => (n + 3) & ~3;
  const offs = [];
  let cur = 64;
  for (const b of blobs) {
    offs.push(cur);
    cur += align4(b.length);
  }
  const romSize = tableAt + cur;
  if (romSize > ROM_LIMIT) throw new Error('ROM too large');

  const rom = new Uint8Array(romSize);
  rom.set(stub, 0);
  const dv = new DataView(rom.buffer);
  const M = 'BKBYDAT1';
  for (let i = 0; i < 8; i++) rom[tableAt + i] = M.charCodeAt(i);
  dv.setUint32(tableAt + 8, 1, true);
  for (let i = 0; i < 5; i++) dv.setUint32(tableAt + 12 + 4 * i, offs[i], true);
  dv.setUint32(tableAt + 36, offs[5], true); // art blob
  dv.setUint32(tableAt + 32, 1, true); // default palette: paperback (1-based)
  for (let i = 0; i < blobs.length; i++) rom.set(blobs[i], tableAt + offs[i]);
  return { rom, romSize };
}

function romFilename() {
  return books.length === 1
    ? books[0].title
        .replace(/[^\w ]+/g, '')
        .trim()
        .replace(/ +/g, '_')
        .toLowerCase()
        .slice(0, 32) + '.gba'
    : 'bookboy_library.gba';
}

async function buildRom() {
  if (busy || !books.length) return;
  setBuild('building ROM…');
  try {
    const { rom, romSize } = await assembleRom();
    const a = document.createElement('a');
    const url = URL.createObjectURL(
      new Blob([rom], { type: 'application/octet-stream' }),
    );
    a.href = url;
    a.download = romFilename();
    a.click();
    setTimeout(() => URL.revokeObjectURL(url), 10000);
    setBuild(
      `done — ${fmtMB(romSize)} ROM with ${books.length} book` +
        (books.length > 1 ? 's' : ''),
    );
  } catch (e) {
    setBuild('error building ROM: ' + e.message);
  }
}

/* ---------------- lazy emulator ("preview on a Game Boy") ---------------- */
let emuLoaded = false;

function loadEmulatorJS(gameUrl) {
  return new Promise((resolve, reject) => {
    window.EJS_player = '#emu';
    window.EJS_core = 'gba';
    window.EJS_gameID = 'bookboy'; // quiets EJS's "gameId not set" warning
    window.EJS_gameUrl = gameUrl;
    window.EJS_pathtodata = 'https://cdn.emulatorjs.org/stable/data/';
    window.EJS_startOnLoaded = true;
    window.EJS_volume = 0;
    window.EJS_Buttons = {
      cheat: false,
      saveState: false,
      loadState: false,
      netplay: false,
      gamepad: false,
    };
    const s = document.createElement('script');
    s.src = 'https://cdn.emulatorjs.org/stable/data/loader.js';
    s.onload = resolve;
    s.onerror = () => reject(new Error('could not load the emulator'));
    document.body.appendChild(s);
  });
}

/* EmulatorJS loads once per page (EJS_Runtime is non-configurable), so the
 * emulator is a one-shot snapshot booted below the live still preview. */
async function launchEmulator() {
  if (busy || !books.length || emuLoaded) return;
  emuLoaded = true;
  const btn = $('playbtn');
  btn.disabled = true;
  btn.textContent = 'loading emulator…';
  try {
    const { rom } = await assembleRom();
    const url = URL.createObjectURL(
      new Blob([rom], { type: 'application/octet-stream' }),
    );
    $('emuwrap').style.display = ''; // shown below the still preview
    await loadEmulatorJS(url); // fetches the mGBA core from CDN
    btn.textContent = '▶ running below';
  } catch (e) {
    setBuild('emulator: ' + e.message);
    $('emuwrap').style.display = 'none';
    emuLoaded = false;
    btn.disabled = false;
    btn.textContent = '▶ run on GBA emulator';
  }
}

/* ---------------- events ---------------- */
async function addFiles(files) {
  const eps = [...files].filter((f) => /\.epub$/i.test(f.name));
  if (!eps.length) return;
  if (typeof DecompressionStream === 'undefined') {
    setStatus(
      'your browser is too old to unzip EPUBs — try a recent Chrome, Edge, or Safari 16.4+',
    );
    return;
  }
  await initCore();
  for (const f of eps) {
    setStatus(`reading ${f.name}…`);
    try {
      const b = await parseEpub(f);
      books.push(b);
      selected = books.length - 1;
      curPage = 0;
    } catch (e) {
      setStatus(`couldn't read ${f.name}: ${e.message}`);
      await new Promise((r) => setTimeout(r, 1500));
    }
  }
  renderShelf();
  await retypeset();
}

window.addEventListener('DOMContentLoaded', () => {
  const dz = $('drop');
  dz.onclick = () => $('file').click();
  dz.onkeydown = (e) => {
    // keyboard access (role=button)
    if (e.key === 'Enter' || e.key === ' ') {
      e.preventDefault();
      $('file').click();
    }
  };
  $('file').onchange = (e) => addFiles(e.target.files);
  dz.ondragover = (e) => {
    e.preventDefault();
    dz.classList.add('hot');
  };
  dz.ondragleave = () => dz.classList.remove('hot');
  dz.ondrop = (e) => {
    e.preventDefault();
    dz.classList.remove('hot');
    addFiles(e.dataTransfer.files);
  };
  $('prev').onclick = () => {
    if (curPage > 0) {
      curPage--;
      renderPreview();
    }
  };
  $('next').onclick = () => {
    if (books[selected] && curPage + 1 < books[selected].npages) {
      curPage++;
      renderPreview();
    }
  };
  $('fontsize').onchange = retypeset;
  $('covers').onchange = () => {
    retypeset();
  };
  $('build').onclick = buildRom;
  $('playbtn').onclick = launchEmulator;
});
