# -*- coding: utf-8 -*-
"""Dependency-free thesis HTML -> DOCX with embedded figures.

Supports h1-h4, p, ul/li, pre, table, blockquote, page breaks, cover/toc
divs, and <figure><img><figcaption> with real PNG embedding (OOXML media +
relationships + inline drawing). Chinese via Microsoft YaHei. No packages.
"""
import os
import struct
import zipfile
from html.parser import HTMLParser

HERE = os.path.dirname(__file__)
SRC = os.path.join(HERE, "thesis.html")
OUT = os.path.join(HERE, "thesis.docx")
FONT = "Microsoft YaHei"
EMU_PER_IN = 914400
IMG_MAX_W_EMU = 5486400          # 6 inch content width


def esc(t):
    return t.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def png_size(path):
    with open(path, "rb") as f:
        head = f.read(24)
    w, h = struct.unpack(">II", head[16:24])
    return w, h


class Doc(HTMLParser):
    LEAF = {"h1", "h2", "h3", "h4", "p", "li", "pre", "blockquote"}
    DIVCLS = {"big": "title", "sub": "subtitle", "abstract-title": "h2c",
              "kw": "p", "l1": "toc1", "l2": "toc2"}

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.blocks = []
        self.buf = []
        self.capture = None
        self.in_cell = False
        self.cell = []
        self.row = None
        self.table = None
        self.cell_header = False

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        cls = a.get("class", "")
        if tag == "div":
            if "pagebreak" in cls:
                self.blocks.append(("pagebreak", None))
            else:
                for k, kind in self.DIVCLS.items():
                    if k in cls.split():
                        self.capture = ("div", kind)
                        self.buf = []
                        break
        elif tag in self.LEAF:
            self.capture = ("leaf", tag)
            self.buf = []
        elif tag == "img":
            src = a.get("src", "")
            self.blocks.append(("img", os.path.join(HERE, src.replace("/", os.sep))))
        elif tag == "figcaption":
            self.capture = ("leaf", "caption")
            self.buf = []
        elif tag == "table":
            self.table = []
        elif tag == "tr":
            self.row = []
        elif tag in ("th", "td"):
            self.in_cell = True
            self.cell = []
            self.cell_header = (tag == "th")
        elif tag == "br" and self.capture and self.capture[1] == "pre":
            self.buf.append("\n")

    def handle_endtag(self, tag):
        if tag in self.LEAF or tag == "figcaption" or (tag == "div" and self.capture and self.capture[0] == "div"):
            if not self.capture:
                return
            kind = self.capture[1]
            text = "".join(self.buf)
            if kind == "pre":
                text = text.strip("\n")
            else:
                text = " ".join(text.split())
            if kind == "blockquote":
                kind = "p"
            if tag == "figcaption":
                kind = "caption"
            if text or kind == "pre":
                self.blocks.append((kind, text))
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
    font = "Consolas" if mono else FONT
    rpr = ['<w:rPr>', '<w:rFonts w:ascii="%s" w:eastAsia="%s" w:hAnsi="%s"/>' % (font, FONT, font)]
    if bold:
        rpr.append("<w:b/>")
    if color:
        rpr.append('<w:color w:val="%s"/>' % color)
    rpr.append('<w:sz w:val="%d"/></w:rPr>' % sz)
    return '<w:r>%s<w:t xml:space="preserve">%s</w:t></w:r>' % ("".join(rpr), esc(text))


def para(runs_xml, jc=None, shade=None, after=120):
    ppr = ['<w:spacing w:after="%d"/>' % after]
    if jc:
        ppr.append('<w:jc w:val="%s"/>' % jc)
    if shade:
        ppr.append('<w:shd w:val="clear" w:fill="%s"/>' % shade)
    return "<w:p><w:pPr>%s</w:pPr>%s</w:p>" % ("".join(ppr), runs_xml)


def heading(text, level):
    sz = {1: 40, 2: 32, 3: 27, 4: 24}[level]
    color = "0B3D91" if level <= 2 else "123A7A"
    return para(run(text, sz, bold=True, color=color), after=140)


def table_xml(rows):
    borders = "<w:tblBorders>" + "".join(
        '<w:%s w:val="single" w:sz="4" w:space="0" w:color="9BB0D0"/>' % b
        for b in ("top", "left", "bottom", "right", "insideH", "insideV")) + "</w:tblBorders>"
    tblpr = '<w:tblPr><w:tblW w:w="5000" w:type="pct"/>%s<w:tblLayout w:type="autofit"/></w:tblPr>' % borders
    ncol = max(len(r) for r in rows)
    grid = "<w:tblGrid>" + "<w:gridCol/>" * ncol + "</w:tblGrid>"
    out = ["<w:tbl>", tblpr, grid]
    for r in rows:
        out.append("<w:tr>")
        for text, is_h in r:
            tcpr = "<w:tcPr>" + ('<w:shd w:val="clear" w:fill="E8EEF8"/>' if is_h else "") + "</w:tcPr>"
            color = "0B3D91" if is_h else None
            out.append("<w:tc>%s%s</w:tc>" % (tcpr, para(run(text, 19, bold=is_h, color=color), after=40)))
        out.append("</w:tr>")
    out.append("</w:tbl><w:p/>")
    return "".join(out)


def drawing_xml(rid, did, cx, cy, name):
    return ('<w:r><w:drawing><wp:inline distT="0" distB="0" distL="0" distR="0">'
            '<wp:extent cx="%d" cy="%d"/><wp:docPr id="%d" name="%s"/>'
            '<a:graphic><a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/picture">'
            '<pic:pic><pic:nvPicPr><pic:cNvPr id="%d" name="%s"/><pic:cNvPicPr/></pic:nvPicPr>'
            '<pic:blipFill><a:blip r:embed="rId%d"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>'
            '<pic:spPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="%d" cy="%d"/></a:xfrm>'
            '<a:prstGeom prst="rect"><a:avLst/></a:prstGeom></pic:spPr></pic:pic>'
            '</a:graphicData></a:graphic></wp:inline></w:drawing></w:r>'
            % (cx, cy, did, name, did, name, rid, cx, cy))


def build(blocks):
    body = []
    media = []          # (rid, arcname, path)
    rid = 10
    did = 100
    for kind, payload in blocks:
        if kind in ("h1", "h2", "h3", "h4"):
            body.append(heading(payload, int(kind[1])))
        elif kind == "title":
            body.append(para(run(payload, 48, bold=True, color="0B3D91"), jc="center", after=200))
        elif kind == "subtitle":
            body.append(para(run(payload, 24, color="555555"), jc="center", after=200))
        elif kind == "h2c":
            body.append(para(run(payload, 30, bold=True, color="0B3D91"), jc="center", after=140))
        elif kind == "toc1":
            body.append(para(run(payload, 22, bold=True), after=60))
        elif kind == "toc2":
            body.append(para(run("    " + payload, 20, color="333333"), after=60))
        elif kind == "p":
            body.append(para(run(payload, 21), jc="both", after=120))
        elif kind == "caption":
            body.append(para(run(payload, 19, color="444444"), jc="center", after=160))
        elif kind == "li":
            body.append(para(run("• " + payload, 21), after=60))
        elif kind == "pre":
            for line in payload.split("\n"):
                body.append(para(run(line or " ", 18, mono=True), shade="F2F4F8", after=0))
            body.append("<w:p/>")
        elif kind == "table":
            body.append(table_xml(payload))
        elif kind == "pagebreak":
            body.append('<w:p><w:r><w:br w:type="page"/></w:r></w:p>')
        elif kind == "img":
            if not os.path.exists(payload):
                continue
            w, h = png_size(payload)
            cx = IMG_MAX_W_EMU
            cy = int(cx * h / w)
            rid += 1
            did += 1
            arc = "media/" + os.path.basename(payload)
            media.append((rid, arc, payload))
            body.append(para(drawing_xml(rid, did, cx, cy, os.path.basename(payload)), jc="center", after=40))
    return "".join(body), media


DOC_OPEN = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
            '<w:document '
            'xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main" '
            'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships" '
            'xmlns:wp="http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing" '
            'xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" '
            'xmlns:pic="http://schemas.openxmlformats.org/drawingml/2006/picture">'
            '<w:body>')
DOC_CLOSE = ('<w:sectPr>'
             '<w:footerReference w:type="default" r:id="rId3"/>'
             '<w:pgSz w:w="11906" w:h="16838"/>'
             '<w:pgMar w:top="1247" w:right="1134" w:bottom="1418" w:left="1134" '
             'w:header="720" w:footer="680" w:gutter="0"/>'
             '</w:sectPr></w:body></w:document>')

_FT_RPR = ('<w:rPr><w:rFonts w:ascii="Microsoft YaHei" w:eastAsia="Microsoft YaHei" '
           'w:hAnsi="Microsoft YaHei"/><w:sz w:val="18"/><w:color w:val="666666"/></w:rPr>')
FOOTER = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
          '<w:ftr xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">'
          '<w:p><w:pPr><w:jc w:val="center"/></w:pPr>'
          '<w:r>%s<w:t xml:space="preserve">第 </w:t></w:r>'
          '<w:fldSimple w:instr=" PAGE "><w:r>%s<w:t>1</w:t></w:r></w:fldSimple>'
          '<w:r>%s<w:t xml:space="preserve"> 页 / 共 </w:t></w:r>'
          '<w:fldSimple w:instr=" NUMPAGES "><w:r>%s<w:t>1</w:t></w:r></w:fldSimple>'
          '<w:r>%s<w:t xml:space="preserve"> 页</w:t></w:r>'
          '</w:p></w:ftr>' % (_FT_RPR, _FT_RPR, _FT_RPR, _FT_RPR, _FT_RPR))


def main():
    with open(SRC, "r", encoding="utf-8") as f:
        p = Doc()
        p.feed(f.read())
    body, media = build(p.blocks)
    document = DOC_OPEN + body + DOC_CLOSE

    ctypes = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
              '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
              '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
              '<Default Extension="xml" ContentType="application/xml"/>'
              '<Default Extension="png" ContentType="image/png"/>'
              '<Override PartName="/word/document.xml" '
              'ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>'
              '<Override PartName="/word/footer1.xml" '
              'ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.footer+xml"/>'
              '</Types>')
    rels = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
            '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
            '<Relationship Id="rId1" '
            'Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" '
            'Target="word/document.xml"/></Relationships>')
    drels = ['<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
             '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
             '<Relationship Id="rId3" '
             'Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/footer" '
             'Target="footer1.xml"/>']
    for rid, arc, _ in media:
        drels.append('<Relationship Id="rId%d" '
                     'Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/image" '
                     'Target="%s"/>' % (rid, arc))
    drels.append("</Relationships>")

    with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml", ctypes)
        z.writestr("_rels/.rels", rels)
        z.writestr("word/document.xml", document)
        z.writestr("word/footer1.xml", FOOTER)
        z.writestr("word/_rels/document.xml.rels", "".join(drels))
        for _, arc, path in media:
            z.write(path, "word/" + arc)
    print("thesis.docx written:", os.path.getsize(OUT), "bytes;", len(media), "figures embedded")


if __name__ == "__main__":
    main()
