// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Raúl Milla (DominicZe)
//
// Builds the design documentation (docs/design/*.md) into one PDF:
// cover page, table of contents, every chapter in order, Mermaid diagrams
// rendered as vector graphics, page numbers in the footer.
//
// Needs Node 18+ and a local Chrome or Chromium. From the repository root:
//
//   cd tools/design-pdf
//   npm install
//   node build.mjs --version 1.0.0 [--chrome /path/to/chrome] [--out file.pdf]
//
// How it works: the Markdown is turned into one HTML page with marked, the
// page loads mermaid.js from node_modules to draw the diagrams, and headless
// Chrome prints it to PDF. Nothing is downloaded at build time.

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { marked } from 'marked';
import puppeteer from 'puppeteer-core';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '..', '..');
const designDir = path.join(root, 'docs', 'design');

function option(name, fallback) {
  const i = process.argv.indexOf(`--${name}`);
  return i > 0 ? process.argv[i + 1] : fallback;
}

const version = option('version', '1.0.0');
const out = path.resolve(option('out', path.join(root, `linuxcnc-wireless-display-design-v${version}.pdf`)));
const chrome = option('chrome', process.env.CHROME_PATH || findChrome());

function findChrome() {
  const candidates = [
    '/usr/bin/google-chrome', '/usr/bin/chromium', '/usr/bin/chromium-browser',
    'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe',
    'C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe',
    '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
  ];
  return candidates.find((p) => fs.existsSync(p));
}

// Heading anchors the same way GitHub makes them, so the links between
// chapters ("03-host.md#networkpy") keep working inside the single PDF.
function slug(text) {
  return text.toLowerCase().trim()
    .replace(/<[^>]+>/g, '')
    .replace(/[^\w\- ]+/g, '')
    .replace(/ /g, '-');
}

const chapters = fs.readdirSync(designDir)
  .filter((f) => /^\d\d-.*\.md$/.test(f))
  .sort();

const toc = [];
let body = '';
for (const file of chapters) {
  const id = file.replace(/\.md$/, '');
  let md = fs.readFileSync(path.join(designDir, file), 'utf8');
  // Links to other chapters become links inside the PDF.
  md = md.replace(/\]\((\d\d-[a-z0-9-]+)\.md(#[^)]+)?\)/g,
                  (_, chap, hash) => `](#${hash ? chap + '--' + hash.slice(1) : chap})`);
  // Links to the user guides one level up point at GitHub.
  md = md.replace(/\]\(\.\.\/([^)]+)\)/g,
                  '](https://github.com/DominicZe-DZ/linuxcnc-wireless-display/blob/main/docs/$1)');

  const renderer = new marked.Renderer();
  renderer.heading = (text, level) => {
    const anchor = level === 1 ? id : `${id}--${slug(text)}`;
    if (level === 1) toc.push({ id, title: text });
    return `<h${level} id="${anchor}">${text}</h${level}>\n`;
  };
  renderer.code = (code, lang) => lang === 'mermaid'
    ? `<pre class="mermaid">${code.replace(/</g, '&lt;')}</pre>\n`
    : `<pre><code>${code.replace(/&/g, '&amp;').replace(/</g, '&lt;')}</code></pre>\n`;
  body += `<section class="chapter">${marked.parse(md, { renderer })}</section>\n`;
}

const mermaidJs = pathToFileURL(path.join(here, 'node_modules', 'mermaid', 'dist', 'mermaid.min.js')).href;
const today = new Date().toISOString().slice(0, 10);

const html = `<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<title>LinuxCNC Wireless Display - Design documentation v${version}</title>
<style>
  @page { size: A4; margin: 20mm 18mm 20mm 18mm; }
  body { font-family: "Segoe UI", "DejaVu Sans", Arial, sans-serif; font-size: 10pt;
         line-height: 1.45; color: #1d2327; }
  h1 { font-size: 20pt; border-bottom: 2px solid #bb1e10; padding-bottom: 4px; margin-top: 0; }
  h2 { font-size: 14pt; margin-top: 18pt; color: #22313a; }
  h3 { font-size: 11.5pt; margin-top: 14pt; }
  .chapter { page-break-before: always; }
  table { border-collapse: collapse; width: 100%; margin: 8pt 0; font-size: 8.8pt;
          page-break-inside: auto; }
  tr { page-break-inside: avoid; }
  th, td { border: 1px solid #c9d0d3; padding: 3pt 5pt; vertical-align: top; text-align: left; }
  th { background: #eef1f2; }
  code { font-family: Consolas, "DejaVu Sans Mono", monospace; font-size: 8.6pt;
         background: #f3f5f6; padding: 0 2px; }
  pre { background: #f6f8f9; border: 1px solid #dde3e5; padding: 6pt; font-size: 8pt;
        white-space: pre-wrap; page-break-inside: avoid; }
  pre code { background: none; padding: 0; }
  pre.mermaid { background: none; border: none; text-align: center; }
  pre.mermaid svg { max-width: 100%; height: auto; }
  blockquote { border-left: 3px solid #bb1e10; margin-left: 0; padding-left: 10pt; color: #44515a; }
  a { color: #0a5f80; text-decoration: none; }
  .cover { height: 245mm; display: flex; flex-direction: column; justify-content: center; }
  .cover h1 { font-size: 30pt; border: none; margin-bottom: 4pt; }
  .cover .sub { font-size: 15pt; color: #44515a; margin-bottom: 40pt; }
  .cover table { width: auto; font-size: 10.5pt; }
  .cover td { border: none; padding: 2pt 14pt 2pt 0; }
  .toc { page-break-before: always; }
  .toc ol { font-size: 12pt; line-height: 2; }
</style></head><body>
<section class="cover">
  <h1>LinuxCNC Wireless Display</h1>
  <div class="sub">Design documentation</div>
  <table>
    <tr><td>Version</td><td>${version}</td></tr>
    <tr><td>Date</td><td>${today}</td></tr>
    <tr><td>Author</td><td>Raúl Milla (DominicZe)</td></tr>
    <tr><td>Web</td><td>https://dominicze.com</td></tr>
    <tr><td>Source</td><td>https://github.com/DominicZe-DZ/linuxcnc-wireless-display</td></tr>
    <tr><td>License</td><td>MIT (software), CC BY 4.0 (enclosure)</td></tr>
  </table>
</section>
<section class="toc"><h1>Contents</h1><ol>
${toc.map((c) => `<li><a href="#${c.id}">${c.title}</a></li>`).join('\n')}
</ol></section>
${body}
<script src="${mermaidJs}"></script>
<script>
  mermaid.initialize({ startOnLoad: false, theme: 'neutral', securityLevel: 'loose',
                       flowchart: { htmlLabels: true }, fontFamily: 'Arial' });
  mermaid.run({ querySelector: 'pre.mermaid' })
    .then(() => { window.diagramsDone = true; })
    .catch((e) => { window.diagramsError = String(e); window.diagramsDone = true; });
</script>
</body></html>`;

if (!chrome) {
  console.error('Chrome/Chromium not found, pass --chrome <path> or set CHROME_PATH');
  process.exit(1);
}
const htmlFile = path.join(here, 'design.html');
fs.writeFileSync(htmlFile, html);

const browser = await puppeteer.launch({
  executablePath: chrome, headless: true,
  args: ['--allow-file-access-from-files', '--no-sandbox'],
});
try {
  const page = await browser.newPage();
  await page.goto(pathToFileURL(htmlFile).href, { waitUntil: 'load' });
  await page.waitForFunction('window.diagramsDone === true', { timeout: 60000 });
  const error = await page.evaluate('window.diagramsError');
  if (error) throw new Error(`Mermaid failed: ${error}`);
  await page.pdf({
    path: out, format: 'A4', printBackground: true,
    displayHeaderFooter: true,
    headerTemplate: '<span></span>',
    footerTemplate: `<div style="font-size:7pt;width:100%;padding:0 18mm;color:#6b777c;
      display:flex;justify-content:space-between;">
      <span>LinuxCNC Wireless Display - design documentation v${version}</span>
      <span><span class="pageNumber"></span> / <span class="totalPages"></span></span></div>`,
    margin: { top: '20mm', bottom: '20mm', left: '18mm', right: '18mm' },
  });
  console.log(`written ${out}`);
} finally {
  await browser.close();
  fs.rmSync(htmlFile, { force: true });
}
