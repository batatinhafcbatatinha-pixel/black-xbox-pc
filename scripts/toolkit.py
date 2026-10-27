"""Where the xboxrecomp toolkit is: the submodule when it is checked out, else the author's neighbouring checkout."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLKIT = ROOT / 'xboxrecomp'
if not (TOOLKIT / 'tools' / 'recomp').is_dir():
    TOOLKIT = ROOT.parent / 'xboxrecomp'
