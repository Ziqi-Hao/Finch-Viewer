#!/usr/bin/env python3
"""Local GPU-accelerated tractography editor — thin entry point.

The implementation lives in the `trkedit` package; the project rules live in
../CLAUDE.md.  Run from the `trkedit` conda env (Ubuntu-22.04 WSL):

    python python/local_editor.py \
        --volume SUBG08_tissue_FA_aggressive.nii.gz \
        --trk SUBG08_OR_full.trk \
        --out SUBG08_OR_edited.trk

--trk is optional (a file dialog opens if omitted); load others at runtime with 'l'.

Controls: drag the yellow box (white = inside box -> will be edited); then
d=delete  k=keep  p=preview  t=stats  +/-=density  n=set#  l=load  u=undo
r=reset  s=save  h=toggle volume  q=quit.  Rotate the view by dragging the empty
background.
"""
import argparse
import os
import sys

# allow `python python/local_editor.py` from any cwd
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from trkedit.editor import Editor


def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--volume", "--fa", dest="volume", required=True,
                   help="background scalar volume NIfTI (--fa is a deprecated alias)")
    p.add_argument("--trk", default=None,
                   help="input .trk (optional; a file dialog opens if omitted)")
    p.add_argument("--out", required=True, help="output .trk for surviving subset")
    p.add_argument("--display-n", type=int, default=12_000,
                   help="max streamlines drawn interactively (default 12000)")
    p.add_argument("--disp-step", type=int, default=2,
                   help="draw every k-th point (default 2; visual only, editing "
                        "and saving always use full resolution)")
    p.add_argument("--selector", choices=["grid", "linear"], default="grid",
                   help="box-selection backend (default grid)")
    p.add_argument("--seed", type=int, default=0, help="display subsample seed")
    Editor(p.parse_args()).run()


if __name__ == "__main__":
    main()
