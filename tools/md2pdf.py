#!/usr/bin/env python3
"""Convert USER_GUIDE.md to styled PDF via Chrome headless."""
import sys, os, re, subprocess, markdown

HTML_TEMPLATE = r"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<title>{title}</title>
<style>
  @page {{ size: A4; margin: 18mm 16mm; }}
  body {{
    font-family: "Microsoft YaHei","微软雅黑","Segoe UI","Noto Sans CJK SC",sans-serif;
    font-size: 10.5pt; line-height: 1.65; color: #222;
  }}
  h1 {{
    font-size: 20pt; color: #1a4f8b; border-bottom: 3px solid #1a4f8b;
    padding-bottom: 8px; margin-top: 0; page-break-after: avoid;
  }}
  h2 {{
    font-size: 15pt; color: #1a4f8b; border-bottom: 1px solid #b0c4de;
    padding-bottom: 4px; margin-top: 1.6em; page-break-after: avoid;
  }}
  h3 {{ font-size: 12.5pt; color: #2a5f9e; margin-top: 1.3em; page-break-after: avoid; }}
  h4 {{ font-size: 11pt; color: #333; page-break-after: avoid; }}
  p {{ margin: 0.5em 0; }}
  a {{ color: #1a4f8b; text-decoration: none; word-break: break-all; }}
  strong {{ color: #c0392b; }}
  table {{
    border-collapse: collapse; width: 100%; margin: 0.8em 0;
    font-size: 9.5pt; page-break-inside: avoid;
  }}
  th {{
    background: #1a4f8b; color: #fff; font-weight: 600;
    padding: 6px 10px; text-align: left; border: 1px solid #1a4f8b;
  }}
  td {{ padding: 5px 10px; border: 1px solid #ccc; vertical-align: top; }}
  tr:nth-child(even) td {{ background: #f5f8fc; }}
  code {{
    font-family: "Consolas","Courier New",monospace; background: #eef2f7;
    color: #c7254e; padding: 1px 5px; border-radius: 3px; font-size: 9pt;
  }}
  pre {{
    background: #f6f8fa; border: 1px solid #d0d7de; border-left: 4px solid #1a4f8b;
    border-radius: 4px; padding: 10px 14px; overflow-x: auto;
    page-break-inside: avoid; font-size: 8.8pt; line-height: 1.5; white-space: pre;
  }}
  pre code {{ background: transparent; color: #24292e; padding: 0; font-size: 8.8pt; }}
  blockquote {{
    border-left: 4px solid #f0ad4e; background: #fcf8e3;
    margin: 0.8em 0; padding: 8px 16px; color: #555; page-break-inside: avoid;
  }}
  blockquote p {{ margin: 0.3em 0; }}
  ul, ol {{ padding-left: 1.8em; margin: 0.4em 0; }}
  li {{ margin: 0.2em 0; }}
  hr {{ border: none; border-top: 1px solid #ddd; margin: 1.5em 0; }}
  /* 图片: 居中 + 限宽 + 不跨页割裂 */
  img {{
    display: block;
    max-width: 95%;
    margin: 10px auto;
    page-break-inside: avoid;
  }}
  /* 图注 (独立图片行的 alt 文字) */
  figure {{
    margin: 12px 0; text-align: center; page-break-inside: avoid;
  }}
  figcaption {{
    font-size: 9pt; color: #888; font-style: italic; margin-top: 4px;
  }}
</style>
</head>
<body>
{body}
</body>
</html>"""

def md_to_html(md_text, title):
    md = markdown.Markdown(extensions=["tables","fenced_code","toc","sane_lists"])
    body = md.convert(md_text)
    # 把独占一行的 <p><img ...></p> 包装成 <figure> + <figcaption>(alt)</figure>,
    # 这样独立图片行会带居中说明文字; 行内图片不受影响 (不在独立 <p> 中)
    body = re.sub(
        r'<p><img alt="([^"]*)" src="([^"]+)"\s*/?></p>',
        r'<figure><img alt="\1" src="\2"/>\n<figcaption>\1</figcaption></figure>',
        body,
    )
    return HTML_TEMPLATE.format(title=title, body=body)

def find_chrome():
    for c in [r"C:\Program Files\Google\Chrome\Application\chrome.exe",
              r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
              r"C:\Program Files\Microsoft\Edge\Application\msedge.exe"]:
        if os.path.exists(c): return c
    return None

def main():
    if len(sys.argv) != 3:
        print("Usage: python md2pdf.py <input.md> <output.pdf>"); sys.exit(1)
    md_path, pdf_path = sys.argv[1], os.path.abspath(sys.argv[2])
    with open(md_path, "r", encoding="utf-8") as f: md_text = f.read()
    title = os.path.splitext(os.path.basename(pdf_path))[0]
    html = md_to_html(md_text, title)
    # 关键: 临时 HTML 写到 md 文件同目录, 这样图片的相对路径 (./img/xxx.png)
    # 才能被 Chrome 正确解析 (Chrome 按文件位置解析相对 URL)
    md_dir = os.path.dirname(os.path.abspath(md_path))
    tmp_path = os.path.join(md_dir, '_user_guide_tmp.html')
    with open(tmp_path, "w", encoding="utf-8") as f: f.write(html)
    try:
        chrome = find_chrome()
        if not chrome: print("ERROR: Chrome/Edge not found", file=sys.stderr); sys.exit(2)
        print(f"Using: {chrome}")
        cmd = [chrome,"--headless=new","--disable-gpu","--no-sandbox",
               "--no-pdf-header-footer",f"--print-to-pdf={pdf_path}",tmp_path]
        subprocess.run(cmd, capture_output=True, timeout=60)
        if os.path.exists(pdf_path):
            print(f"OK: {pdf_path} ({os.path.getsize(pdf_path)//1024} KB)")
        else:
            print("ERROR: PDF generation failed", file=sys.stderr); sys.exit(3)
    finally:
        os.unlink(tmp_path)

if __name__ == "__main__":
    main()