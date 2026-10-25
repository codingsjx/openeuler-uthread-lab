"""Package the actual generated results and their build log."""
from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED

root = Path(__file__).resolve().parents[1]/'results'
root.mkdir(exist_ok=True)
destination = root/'stage2-results.zip'
with ZipFile(destination,'w',ZIP_DEFLATED) as bundle:
    for p in sorted(root.rglob('*')):
        if p.is_file() and p.suffix != '.zip':
            bundle.write(p,p.relative_to(root).as_posix())
print(destination)
