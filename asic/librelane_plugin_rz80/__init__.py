# RZ80's LibreLane plugin: the step that lays z80_top out after the Z80
# die (RZ80.DieFloorplan). LibreLane imports any `librelane_plugin_*`
# module on sys.path; asic/<pdk>/Makefile puts asic/ on PYTHONPATH, and the
# core's config inserts the step with
#   meta: substituting_steps: {"-OpenROAD.GlobalPlacement": RZ80.DieFloorplan,
#                               "+OpenROAD.GlobalPlacement": RZ80.ReleaseAnchors}
# i.e. after the pins are placed and before global placement, which then
# places every other cell around the fixed ones; released right after, so
# the repairs that follow may size them.
import os
from typing import List

from librelane.config import Variable
from librelane.logging import info
from librelane.steps import OdbpyStep, Step

HERE = os.path.dirname(os.path.abspath(__file__))


@Step.factory.register()
class DieFloorplan(OdbpyStep):
    """
    Places z80_top's anchor cells where the Z80 die has their counterparts
    (arrangement.py, after Ken Shirriff's die analysis) and fixes them
    (FIRM): the register file's 224 flip-flops in the die's 14
    columns of 16-bit pairs, the address latch and the ALU's latches beside
    it, the decoder's AND and OR planes with the instruction register at
    their end, each pin's flip-flops at its pin. Global placement places
    the rest around them; RZ80.ReleaseAnchors frees them right after. The
    anchors are written to anchors.json in the step directory, which
    asic/floorplan_check.py compares with the final layout.
    """

    id = "RZ80.DieFloorplan"
    name = "Die floorplan"

    config_vars = OdbpyStep.config_vars + [
        Variable(
            "RZ80_DIE_FLOORPLAN",
            bool,
            "Place and fix the anchor cells as the Z80 die arranges their counterparts.",
            default=True,
        ),
    ]

    def get_script_path(self) -> str:
        return os.path.join(HERE, "die_floorplan.py")

    def get_command(self) -> List[str]:
        assert self.config_path is not None, "get_command called before start()"
        return super().get_command() + [
            "--step-config",
            self.config_path,
            "--anchors",
            os.path.join(self.step_dir, "anchors.json"),
        ]

    def run(self, state_in, **kwargs):
        if not self.config["RZ80_DIE_FLOORPLAN"]:
            info(f"'RZ80_DIE_FLOORPLAN' is off. Skipping '{self.id}'…")
            return {}, {}
        return super().run(state_in, **kwargs)


@Step.factory.register()
class ReleaseAnchors(OdbpyStep):
    """
    After global placement: RZ80.DieFloorplan's anchors (FIRM) become placed
    cells again, so the timing repairs may size them and detailed placement
    may shift them the few sites a bigger cell needs -- a fixed cell that
    grows in place would overlap its neighbour. Global placement has already
    placed everything around them; asic/floorplan_check.py measures how far
    any anchor moved after.
    """

    id = "RZ80.ReleaseAnchors"
    name = "Release the die floorplan's anchors"

    def get_script_path(self) -> str:
        return os.path.join(HERE, "release_anchors.py")
