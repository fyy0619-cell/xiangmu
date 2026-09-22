# -*- coding: utf-8 -*-
"""Dependency-free HTML -> DOCX (OOXML) converter for the project manual.

Handles h1/h2/h3, p, ul/li, pre, table (th/td), blockquote and page breaks.
Chinese renders via the Microsoft YaHei east-asian font. No external packages.
"""
import html
import os
import zipfile
from html.parser import HTMLParser

SRC = os.path.join(os.path.dirname(__file__), "manual.html")
OUT = os.path.join(os.path.dirname(__file__), "manual.docx")
FONT = "Microsoft YaHei"


def esc(t):
    return (t.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"))


class Doc(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.blocks = []          # list of ('h1'|'h2'|'h3'|'p'|'li'|'pre'|'pagebreak'|'table', payload)
        self.buf = []
        self.capture = None       # current simple-block tag
        self.in_cell = False
        self.cell = []
        self.row = None
        self.table = None
        self.cell_header = False

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag == "div" and "pagebreak" in a.get("class", ""):
            self.blocks.append(("pagebreak", None))
        elif tag in ("h1", "h2", "h3", "p", "li", "pre", "blockquote"):
            self.capture = tag
            self.buf = []
        elif tag == "table":
            self.table = []
        elif tag == "tr":
            self.row = []
        elif tag in ("th", "td"):
            self.in_cell = True
            self.cell = []
            self.cell_header = (tag == "th")
        elif tag == "br" and self.capture == "pre":
            self.buf.append("\n")

    def handle_endtag(self, tag):
        if tag in ("h1", "h2", "h3", "p", "li", "pre", "blockquote"):
            text = "".join(self.buf)
            if tag == "pre":
                text = text.strip("\n")
            else:
                text = " ".join(text.split())
            key = "p" if tag == "blockquote" else tag
            if text or tag == "pre":
                self.blocks.append((key, text))
            self.capture = None
            self.buf = []
        elif tag in ("th", "td"):
            self.in_cell = False
            self.row.append((" ".join("".join(self.cell).split()), self.cell_header))
        elif tag == "tr":
            if self.row:
                self.table.append(self.row)
            self.row = None
        elif tag == "table":
            self.blocks.append(("table", self.table))
            self.table = None

    def handle_data(self, data):
        if self.in_cell:
            self.cell.append(data)
        elif self.capture:
            self.buf.append(data)


def run(text, sz, bold=False, color=None, mono=False):
    rpr = ["<w:rPr>"]
    font = "Consolas" if mono else FONT
    rpr.append('<w:rFonts w:ascii="%s" w:eastAsia="%s" w:hAnsi="%s"/>' % (font, FONT, font))
    if bold:
        rpr.append("<w:b/>")
    if color:
        rpr.append('<w:color w:val="%s"/>' % color)
    rpr.append('<w:sz w:val="%d"/>' % sz)
    rpr.append("</w:rPr>")
    return "<w:r>%s<w:t xml:space=\"preserve\">%s</w:t></w:r>" % ("".join(rpr), esc(text))


def para(runs_xml, shade=None, spacing_after=120):
    ppr = ['<w:spacing w:after="%d"/>' % spacing_after]
    if shade:
        ppr.append('<w:shd w:val="clear" w:fill="%s"/>' % shade)
    return "<w:p><w:pPr>%s</w:pPr>%s</w:p>" % ("".join(ppr), runs_xml)


def heading(text, level):
    sz = {1: 52, 2: 32, 3: 26}[level]
    color = "0B3D91" if level <= 2 else "1A4FA0"
    return para(run(text, sz, bold=True, color=color), spacing_after=160)


def pagebreak():
    return '<w:p><w:r><w:br w:type="page"/></w:r></w:p>'


def table_xml(rows):
    borders = ('<w:tblBorders>'
               + "".join('<w:%s w:val="single" w:sz="4" w:space="0" w:color="9BB0D0"/>' % b
                         for b in ("top", "left", "bottom", "right", "insideH", "insideV"))
               + '</w:tblBorders>')
    tblpr = ('<w:tblPr><w:tblW w:w="5000" w:type="pct"/>%s'
             '<w:tblLayout w:type="autofit"/></w:tblPr>' % borders)
    ncol = max(len(r) for r in rows)
    grid = "<w:tblGrid>" + "<w:gridCol/>" * ncol + "</w:tblGrid>"
    out = ["<w:tbl>", tblpr, grid]
    for r in rows:
        out.append("<w:tr>")
        for text, is_h in r:
            shade = "E8EEF8" if is_h else None
            tcpr = "<w:tcPr>"
            if shade:
                tcpr += '<w:shd w:val="clear" w:fill="%s"/>' % shade
            tcpr += "</w:tcPr>"
            color = "0B3D91" if is_h else None
            cell_p = para(run(text, 20, bold=is_h, color=color), spacing_after=40)
            out.append("<w:tc>%s%s</w:tc>" % (tcpr, cell_p))
        out.append("</w:tr>")
    out.append("</w:tbl>")
    # a spacer paragraph after table
    out.append("<w:p/>")
    return "".join(out)


def build_body(blocks):
    body = []
    for kind, payload in blocks:
        if kind in ("h1", "h2", "h3"):
            body.append(heading(payload, int(kind[1])))
        elif kind == "p":
            body.append(para(run(payload, 22)))
        elif kind == "li":
            body.append(para(run("• " + payload, 22)))
        elif kind == "pre":
            for line in payload.split("\n"):
                body.append(para(run(line or " ", 19, mono=True), shade="F2F4F8", spacing_after=0))
            body.append("<w:p/>")
        elif kind == "table":
            body.append(table_xml(payload))
        elif kind == "pagebreak":
            body.append(pagebreak())
    return "".join(body)


DOCUMENT = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
            '<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">'
            '<w:body>%s'
            '<w:sectPr><w:pgSz w:w="11906" w:h="16838"/>'
            '<w:pgMar w:top="1134" w:right="1134" w:bottom="1134" w:left="1134"/>'
            '</w:sectPr></w:body></w:document>')

CONTENT_TYPES = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
                 '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
                 '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
                 '<Default Extension="xml" ContentType="application/xml"/>'
                 '<Override PartName="/word/document.xml" '
                 'ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>'
                 '</Types>')

RELS = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
        '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
        '<Relationship Id="rId1" '
        'Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" '
        'Target="word/document.xml"/></Relationships>')


def main():
    with open(SRC, "r", encoding="utf-8") as f:
        htmltext = f.read()
    p = Doc()
    p.feed(htmltext)
    document = DOCUMENT % build_body(p.blocks)
    with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml", CONTENT_TYPES)
        z.writestr("_rels/.rels", RELS)
        z.writestr("word/document.xml", document)
    print("DOCX written:", OUT, os.path.getsize(OUT), "bytes")


if __name__ == "__main__":
    main()
