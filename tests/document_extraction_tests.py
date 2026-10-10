import importlib.util
from pathlib import Path
import sys
import tempfile
import zipfile
import shutil

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("extract_document", Path(sys.argv[1]))
extractor = importlib.util.module_from_spec(spec)
spec.loader.exec_module(extractor)

with tempfile.TemporaryDirectory(prefix="localagent-extract-") as temp:
    root = Path(temp)
    docx = root / "sample.docx"
    with zipfile.ZipFile(docx, "w") as archive:
        archive.writestr("word/document.xml", '''<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body><w:p><w:r><w:t>Проект САПФИР</w:t></w:r></w:p></w:body></w:document>''')
    assert "САПФИР" in extractor.extract(docx)[0]["text"]

    text_stream = b"BT /F1 12 Tf 72 720 Td (Project SAPPHIRE uses port 43219.) Tj ET"
    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        b"<< /Length " + str(len(text_stream)).encode() + b" >>\nstream\n" + text_stream + b"\nendstream",
    ]
    pdf = bytearray(b"%PDF-1.4\n")
    offsets = [0]
    for number, obj in enumerate(objects, 1):
        offsets.append(len(pdf))
        pdf += str(number).encode() + b" 0 obj\n" + obj + b"\nendobj\n"
    xref = len(pdf)
    pdf += b"xref\n0 6\n0000000000 65535 f \n"
    for offset in offsets[1:]:
        pdf += f"{offset:010d} 00000 n \n".encode()
    pdf += b"trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n" + str(xref).encode() + b"\n%%EOF\n"
    (root / "sample.pdf").write_bytes(pdf)
    parts = extractor.extract(root / "sample.pdf")
    assert parts[0]["page"] == 1 and "43219" in parts[0]["text"]
    (root / "broken.pdf").write_text("not a pdf", encoding="utf-8")
    try:
        extractor.extract(root / "broken.pdf")
    except Exception:
        pass
    else:
        raise AssertionError("Broken PDF was accepted")
    if len(sys.argv) > 2:
        destination = Path(sys.argv[2])
        destination.mkdir(parents=True, exist_ok=True)
        for name in ("sample.docx", "sample.pdf"):
            shutil.copyfile(root / name, destination / name)

print("PASS: DOCX text, PDF text and page provenance, malformed PDF rejection")
