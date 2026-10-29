# pdn_tree.tcl - z80_top's power network for PDN_STYLE=tree (config.tree.yaml):
# pdngen draws only the standard cells' Metal1 followpin rails; the tree
# itself -- trunks, fingers, twigs, the power pins -- is RZ80.PowerTree's
# (asic/librelane_plugin_rz80/power_tree.py), right after this.
source $::env(SCRIPTS_DIR)/openroad/common/set_global_connections.tcl
set_global_connections

set_voltage_domain -name CORE -power $::env(VDD_NET) -ground $::env(GND_NET)

define_pdn_grid \
    -name stdcell_grid \
    -starts_with POWER \
    -voltage_domain CORE

add_pdn_stripe \
    -grid stdcell_grid \
    -layer $::env(PDN_RAIL_LAYER) \
    -width $::env(PDN_RAIL_WIDTH) \
    -followpins
