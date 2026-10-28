# RZ80.ReleaseAnchors's script: after global placement, the anchors
# RZ80.DieFloorplan fixed (FIRM) become ordinary placed cells again, so the
# timing repairs may size them and detailed placement may shift them the
# few sites a bigger cell needs. The taps (LOCKED) stay as they are.
from reader import click, click_odb  # noqa: E402


@click.command()
@click_odb
def main(reader):
    n = 0
    for inst in reader.block.getInsts():
        if inst.getPlacementStatus() == "FIRM":
            inst.setPlacementStatus("PLACED")
            n += 1
    print(f"[INFO RZ80] released {n} anchors")


if __name__ == "__main__":
    main()
