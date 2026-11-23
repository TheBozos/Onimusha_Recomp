"""oni_overlay_externals.py <externals.txt> <out.txt>: the externals the overlay modules are generated with.

The resident program's stubs and overrides apply to the overlays' calls too; its native instruction sites ("native
0xADDR") are resident instructions, not the overlays' own (bozo-recomp refuses a site outside the image's code).
"""
import pathlib
import sys

lines = pathlib.Path(sys.argv[1]).read_text().splitlines()
pathlib.Path(sys.argv[2]).write_text(''.join(line + '\n' for line in lines if not line.startswith('native ')))
