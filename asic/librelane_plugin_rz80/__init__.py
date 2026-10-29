# RZ80's LibreLane plugin: the step that lays z80_top out after the Z80
# die (RZ80.DieFloorplan). LibreLane imports any `librelane_plugin_*`
# module on sys.path; asic/<pdk>/Makefile puts asic/ on PYTHONPATH, and the
# core's config inserts the step with
#   meta: substituting_steps: {"-OpenROAD.GlobalPlacement": RZ80.DieFloorplan,
#                               "+OpenROAD.GlobalPlacement": RZ80.ReleaseAnchors}
# i.e. after the pins are placed and before global placement, which then
# places every other cell around the fixed ones; released right after, so
# the repairs that follow may size them.
import json
import os
from decimal import Decimal
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


TREE_VARS = [
    Variable("RZ80_TREE_TRUNK_WIDTH", Decimal, "Width of the VDD (east) and VSS (west) trunks, Metal4.", default=10, units="µm"),
    Variable("RZ80_TREE_FINGER_WIDTH", Decimal, "Width of the fingers, Metal5.", default=5, units="µm"),
    Variable("RZ80_TREE_FINGER_GAP", Decimal, "Gap between a finger's end and the other net's trunk.", default=5, units="µm"),
    Variable("RZ80_TREE_BAND_ROWS", int, "Placement rows per finger (fingers alternate VDD and VSS).", default=8),
    Variable("RZ80_TREE_TWIG_PITCH", Decimal, "Pitch of the twigs along a finger, Metal4.", default=60, units="µm"),
]


@Step.factory.register()
class PowerTree(OdbpyStep):
    """
    The "old-school" power network of z80_top (``PDN_STYLE=tree``): after
    pdngen has drawn only the Metal1 followpin rails (``pdn_tree.tcl``), a
    VSS trunk down the west edge and a VDD trunk down the east edge
    (Metal4, the block's power pins), Metal5 fingers off each trunk in turn,
    and short Metal4 twigs tying each finger to the rails of its net
    nearest to it -- two interdigitated combs, as the NMOS die feeds +5 V
    from its east edge and GND from its west.
    """

    id = "RZ80.PowerTree"
    name = "Power tree"
    config_vars = OdbpyStep.config_vars + TREE_VARS

    def get_script_path(self) -> str:
        return os.path.join(HERE, "power_tree.py")

    def get_command(self) -> List[str]:
        assert self.config_path is not None, "get_command called before start()"
        return super().get_command() + ["--step-config", self.config_path, "--mode", "core",
                                        "--metrics-out", os.path.join(self.step_dir, "rz80_pdn.json")]

    def run(self, state_in, **kwargs):
        # pdngen's own grid check ran before the trunks, the power pins,
        # existed: this step's check replaces its count
        views, metrics = super().run(state_in, **kwargs)
        with open(os.path.join(self.step_dir, "rz80_pdn.json")) as f:
            metrics.update(json.load(f))
        return views, metrics


CHIP_VARS = [
    Variable("RZ80_TREE_MACRO", str, "The core macro's instance.", default="u_top"),
    Variable("RZ80_TREE_MACRO_HALO", Decimal, "Room kept around the macro by the Metal5 obstruction.", default=2, units="µm"),
    Variable("RZ80_TREE_STRAP_WIDTH", Decimal, "Width of the Metal5 straps feeding the macro's trunks.", default=5, units="µm"),
    Variable("RZ80_TREE_STRAP_CLEARANCE", Decimal, "Clearance of a strap from the macro's own Metal5.", default=1, units="µm"),
]


@Step.factory.register()
class PowerTreeChipObstruct(OdbpyStep):
    """
    Before the chip's pdngen (``PDN_STYLE=tree``): Metal5 obstructed over the
    core macro, whose fingers are there, so the chip's grid stays off it.
    """

    id = "RZ80.PowerTreeChipObstruct"
    name = "Power tree: keep the chip's grid off the macro"
    config_vars = OdbpyStep.config_vars + CHIP_VARS

    def get_script_path(self) -> str:
        return os.path.join(HERE, "power_tree.py")

    def get_command(self) -> List[str]:
        assert self.config_path is not None, "get_command called before start()"
        return super().get_command() + ["--step-config", self.config_path, "--mode", "chip-obstruct"]


@Step.factory.register()
class PowerTreeChip(OdbpyStep):
    """
    After the chip's pdngen (``PDN_STYLE=tree``): the obstruction off, and
    the macro's trunks fed by Metal5 straps over their ends -- the VDD trunk
    from the core ring's east side, the VSS trunk from its west side, the
    VCC pad's side and the GND pad's side.
    """

    id = "RZ80.PowerTreeChip"
    name = "Power tree: feed the macro's trunks"
    config_vars = OdbpyStep.config_vars + CHIP_VARS

    def get_script_path(self) -> str:
        return os.path.join(HERE, "power_tree.py")

    def get_command(self) -> List[str]:
        assert self.config_path is not None, "get_command called before start()"
        return super().get_command() + ["--step-config", self.config_path, "--mode", "chip-connect",
                                        "--metrics-out", os.path.join(self.step_dir, "rz80_pdn.json")]

    def run(self, state_in, **kwargs):
        views, metrics = super().run(state_in, **kwargs)
        with open(os.path.join(self.step_dir, "rz80_pdn.json")) as f:
            metrics.update(json.load(f))
        return views, metrics
