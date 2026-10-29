# pdn_tree.tcl - the chip's power grid for PDN_STYLE=tree (chip/config.tree.yaml):
# pdn.tcl's grid, core ring and ring-to-pads, without its macro grid -- the
# macro's trunks are fed by RZ80.PowerTreeChip instead, after this.
rename define_pdn_grid rz80_define_pdn_grid
proc define_pdn_grid {args} {
    if {[lsearch -exact $args -macro] >= 0} { return }
    rz80_define_pdn_grid {*}$args
}
rename add_pdn_connect rz80_add_pdn_connect
proc add_pdn_connect {args} {
    set i [lsearch -exact $args -grid]
    if {$i >= 0 && [lindex $args [expr {$i + 1}]] eq "macro"} { return }
    rz80_add_pdn_connect {*}$args
}
source [file join [file dirname [info script]] pdn.tcl]
