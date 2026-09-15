#!/usr/bin/env python3
"""Build a standalone, offline HTML handbook from authored Markdown."""
import html
import os
from pathlib import Path
import re
from urllib.parse import unquote, urlsplit
import markdown

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "docs/index.html"
files = [ROOT / "README.md", *sorted((ROOT / "docs").glob("[0-9]*.md")),
         ROOT / "docs/adr/README.md", ROOT / "specs/README.md", ROOT / "docs/sources.md"]


def key(path):
    return str(path.relative_to(ROOT)).replace("/", "-").replace(".", "-")


nav, articles = [], []
for source in files:
    raw = source.read_text()
    title = raw.splitlines()[0].removeprefix("# ")
    prefix = key(source)
    md = markdown.Markdown(extensions=["extra", "toc"], extension_configs={"toc": {"permalink": False}})
    body = md.convert(raw)
    body = re.sub(r'id="([^"]+)"', lambda m: f'id="{prefix}-{m[1]}"', body)

    def link(match):
        target = html.unescape(match[1])
        if target.startswith("#"):
            return f'href="#{prefix}-{target[1:]}"'
        parts = urlsplit(target)
        if parts.scheme:
            return match[0]
        resolved = (source.parent / unquote(parts.path)).resolve()
        if resolved in files:
            anchor = key(resolved) + ("-" + parts.fragment if parts.fragment else "")
            return f'href="#{anchor}"'
        return 'href="' + html.escape(os.path.relpath(resolved, OUTPUT.parent), quote=True) + '"'

    body = re.sub(r'href="([^"]+)"', link, body)
    body = body.replace("<table>", '<div class="table-scroll" tabindex="0"><table>').replace("</table>", "</table></div>")
    body = re.sub(r'<pre><code class="language-mermaid">.*?</code></pre>',
        '<figure aria-label="Архитектура: интерфейс и модули передают команды владельцу проекта; подготовленный план поступает в аудиопоток, сохранение работает отдельно.">'
        '<div class="flow"><span>Интерфейс / модули</span><b>→</b><span>Команды</span><b>→</b><span>Модель проекта</span>'
        '<b>→</b><span>Render plan</span><b>→</b><span>Аудиоустройство</span></div>'
        '<figcaption>Сохранение, чтение медиа и анализ работают вне realtime callback. Стрелки обозначают логическое взаимодействие через описанные очереди.</figcaption></figure>', body, flags=re.S)
    nav.append(f'<a href="#{prefix}">{html.escape(title)}</a>')
    articles.append(f'<article id="{prefix}">{body}</article>')

css = """
:root{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;color:#222;background:#fff;font-size:16px;line-height:1.65}
*{box-sizing:border-box}body{margin:0}aside{position:fixed;inset:0 auto 0 0;width:265px;padding:28px 22px;overflow:auto;border-right:1px solid #ddd;background:#f6f6f6}
aside strong{display:block;font-size:21px;margin-bottom:18px}nav a{display:block;color:#444;padding:6px 0;text-decoration:none;font-size:13px;line-height:1.4}nav a[hidden]{display:none}nav a:hover,nav a:focus{color:#000;text-decoration:underline}
main{margin-left:265px;padding:24px 5vw 80px;max-width:1600px}article{padding:28px 0 52px;border-bottom:1px solid #ccc;scroll-margin-top:16px}h1{font-size:32px;line-height:1.2;letter-spacing:-.6px;margin:0 0 24px}h2{font-size:22px;margin:34px 0 14px}h3{font-size:18px;margin-top:28px}p,li{max-width:950px}a{color:#184e72;text-underline-offset:3px}pre{white-space:pre-wrap;overflow-wrap:anywhere;padding:18px;background:#f4f4f4;border:1px solid #ddd;font-size:13px;line-height:1.55}code{font-size:.87em;overflow-wrap:anywhere}.table-scroll{overflow-x:auto;margin:20px 0}table{border-collapse:collapse;width:100%;font-size:13px;line-height:1.5;min-width:660px}th,td{text-align:left;vertical-align:top;padding:12px;border-bottom:1px solid #ddd}th{background:#eee;font-weight:600}tr:nth-child(even) td{background:#fafafa}.footnote{font-size:12px;color:#555}figure{margin:25px 0;padding:20px 0;border-block:1px solid #ccc}.flow{display:flex;align-items:center;flex-wrap:wrap;gap:12px;font-size:13px}.flow span{padding:9px 12px;border:1px solid #aaa}.flow b{font-weight:400}figcaption{font-size:12px;color:#555;margin-top:14px}input{width:100%;padding:9px;border:1px solid #bbb;background:white;margin-bottom:18px;font:inherit;font-size:13px}.note{font-size:12px;color:#666}
@media(max-width:900px){aside{position:relative;width:auto;border-right:0;border-bottom:1px solid #ddd;max-height:300px}main{margin:0;padding:24px}h1{font-size:27px}}
@media print{aside{display:none}main{margin:0;padding:0;max-width:none}article{break-before:page;border:0}article:first-child{break-before:auto}h1,h2,h3{break-after:avoid}table{min-width:0;font-size:9px}pre{font-size:10px}a{color:#222}th,td{padding:6px}.table-scroll{overflow:visible}}
"""
page = ('<!doctype html><html lang="ru"><head><meta charset="utf-8">'
        '<meta name="viewport" content="width=device-width,initial-scale=1">'
        '<title>My DAW — документация проекта</title><style>' + css + '</style></head><body>'
        '<aside><strong>My DAW</strong><label for="filter" class="note">Найти раздел</label>'
        '<input id="filter" type="search" placeholder="Стек, аудио, атлас…">'
        '<nav aria-label="Разделы документации">' + ''.join(nav) + '</nav>'
        '<p class="note">Для поиска по тексту — ⌘F.<br>Документация, не работающая DAW.</p></aside>'
        '<main>' + ''.join(articles) + '</main>'
        '<script>document.getElementById("filter").addEventListener("input",e=>{'
        'const q=e.target.value.toLocaleLowerCase();document.querySelectorAll("nav a").forEach(a=>{'
        'a.hidden=!a.textContent.toLocaleLowerCase().includes(q);});});</script></body></html>')
OUTPUT.write_text(page)
print(f"Built {OUTPUT.relative_to(ROOT)} from {len(files)} documents ({len(page):,} characters)")
