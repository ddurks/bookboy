// Smoke test for the wasm typesetting core (node).
const fs = require('fs');
const path = require('path');
const createBookboyCore = require('./bookboy_core.js');

const ROOT = path.join(__dirname, '..');
const XC = path.join(ROOT, 'build/xcharter/xcharter/opentype');

const K = { BODY: 0, TITLE: 1, SUBTITLE: 2, BYLINE: 3, PART: 4, PARTSUB: 5,
            DISPLAY: 6 };

createBookboyCore().then((M) => {
  M.FS.mkdir('/fonts');
  for (const f of ['XCharter-Roman.otf', 'XCharter-Italic.otf',
                   'XCharter-Bold.otf'])
    M.FS.writeFile('/fonts/' + f, fs.readFileSync(path.join(XC, f)));
  M.FS.writeFile('/hyph_en_US.dic',
                 fs.readFileSync(path.join(ROOT, 'tools/data/hyph_en_US.dic')));

  const c = (name, ret, args, vals) => M.ccall(name, ret, args, vals);
  c('bw_reset', null, [], []);
  const bi = c('bw_book_begin', 'number', ['string'], ['The Time Machine']);
  c('bw_chapter_break', null, [], []);
  c('bw_para_begin', null, ['number', 'number'], [K.TITLE, -1]);
  const feed = (s, italic = 0) =>
    c('bw_para_run', null, ['number', 'string', 'number'],
      [italic, s, Buffer.byteLength(s, 'utf8')]);
  feed('CHAPTER ONE');
  c('bw_para_end', null, [], []);
  const LOREM = 'The Time Traveller (for so it will be convenient to speak ' +
    'of him) was expounding a recondite matter to us. His pale grey eyes ' +
    'shone and twinkled, and his usually pale face was flushed and ' +
    'animated — the fire burned brightly, and the soft radiance of the ' +
    'incandescent lights in the lilies of silver caught the bubbles that ' +
    'flashed and passed in our glasses. ';
  for (let i = 0; i < 40; i++) {
    c('bw_para_begin', null, ['number', 'number'], [K.BODY, -1]);
    feed(LOREM);
    if (i % 3 === 0) feed('Quite remarkable, really.', 1);
    c('bw_para_end', null, [], []);
  }
  const ended = c('bw_book_end', 'number', [], []);
  console.log('book index:', bi, 'ended:', ended);

  const nglyphs = c('bw_prepare', 'number', ['number', 'number'], [14, 72]);
  console.log('glyphs:', nglyphs);
  const npages = c('bw_layout', 'number',
                   ['number', 'number', 'number', 'number'], [0, 0, 0, 0]);
  console.log('pages:', npages);
  if (npages <= 0) process.exit(1);

  const rgba = M._malloc(240 * 160 * 4);
  const ok = c('bw_render_page', 'number', ['number', 'number', 'number'],
               [0, 1, rgba]);
  const px = M.HEAPU8.subarray(rgba, rgba + 240 * 160 * 4);
  let ink = 0;
  for (let i = 0; i < px.length; i += 4)
    if (px[i] < 0x80) ink++;
  console.log('render ok:', ok, 'dark pixels:', ink);

  const total = c('bw_finalize', 'number', [], []);
  console.log('finalize total:', total);
  const magics = ['GFN2', 'GBK1', 'BLB2'];
  for (let part = 0; part < 3; part++) {
    const sz = c('bw_part_size', 'number', ['number'], [part]);
    const ptr = c('bw_part_data', 'number', ['number'], [part]);
    const head = Buffer.from(M.HEAPU8.subarray(ptr, ptr + 4)).toString('latin1');
    const pass = head === magics[part];
    console.log(`part ${part}: ${sz} B, magic ${head} ${pass ? 'OK' : 'FAIL'}`);
    if (!pass) process.exit(1);
  }
  const miniSz = c('bw_part_size', 'number', ['number'], [3]);
  console.log('mini:', miniSz, 'B');
  console.log('SMOKE TEST PASSED');
});
