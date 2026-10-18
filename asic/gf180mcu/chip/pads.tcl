# pads.tcl - LibreLane PAD_CFG for z80_chip: the pad ring of the Z80 with
# every pad on the side and in the order of the original die, its spacing
# scaled from the Visual Z80 die scan (z80_chip.v has the pinout). Based
# on LibreLane's scripts/openroad/common/pad_cfg.tcl (Apache-2.0), with
# fixed pad positions instead of evenly spread ones.

source $::env(SCRIPTS_DIR)/openroad/common/set_global_connections.tcl
set_global_connections

puts "\[INFO\] Generating the Z80 pad ring…"
set block [ord::get_db_block]

make_io_sites \
    -horizontal_site $::env(PAD_SITE_NAME) \
    -vertical_site $::env(PAD_SITE_NAME) \
    -corner_site $::env(PAD_CORNER_SITE_NAME) \
    -offset $::env(PAD_EDGE_SPACING)

# instance, pad row, start along the row (um, from the die origin): the
# original pad centres mapped onto the row between the corners, pushed
# apart only where two would overlap
set pads {
    {pad_A5       IO_SOUTH   453}
    {pad_A6       IO_SOUTH   532}
    {pad_A7       IO_SOUTH   625}
    {pad_A8       IO_SOUTH   706}
    {pad_A9       IO_SOUTH   799}
    {pad_A10      IO_SOUTH   894}
    {pad_A11      IO_SOUTH   987}
    {pad_A12      IO_SOUTH  1075}
    {pad_A13      IO_SOUTH  1175}
    {pad_A14      IO_SOUTH  1254}
    {pad_A15      IO_SOUTH  1347}
    {pad_CLK      IO_SOUTH  1434}
    {pad_D4       IO_EAST    442}
    {pad_D3       IO_EAST    548}
    {pad_D5       IO_EAST    644}
    {pad_D6       IO_EAST    835}
    {pad_VCC      IO_EAST    978}
    {pad_D2       IO_EAST   1121}
    {pad_D7       IO_EAST   1243}
    {pad_D0       IO_EAST   1468}
    {pad_D1       IO_EAST   1576}
    {pad_WAIT_n   IO_NORTH   452}
    {pad_BUSAK_n  IO_NORTH   629}
    {pad_WR_n     IO_NORTH   838}
    {pad_RD_n     IO_NORTH   913}
    {pad_IORQ_n   IO_NORTH   988}
    {pad_MREQ_n   IO_NORTH  1077}
    {pad_HALT_n   IO_NORTH  1169}
    {pad_NMI_n    IO_NORTH  1356}
    {pad_INT_n    IO_NORTH  1431}
    {pad_A4       IO_WEST    502}
    {pad_A3       IO_WEST    589}
    {pad_A2       IO_WEST    689}
    {pad_A1       IO_WEST    779}
    {pad_A0       IO_WEST    878}
    {pad_GND      IO_WEST   1001}
    {pad_RFSH_n   IO_WEST   1124}
    {pad_M1_n     IO_WEST   1223}
    {pad_RESET_n  IO_WEST   1512}
    {pad_BUSRQ_n  IO_WEST   1587}
}

foreach pad $pads {
    lassign $pad inst_name row pos
    if { [set inst [$block findInst $inst_name]] == "NULL" } {
        puts stderr "\[ERROR\] No instance $inst_name found."
        exit 1
    }
    place_pad -row $row -location $pos $inst_name -master [[$inst getMaster] getName]
}

puts "\[INFO\] Placing corner cells…"
place_corners $::env(PAD_CORNER)

puts "\[INFO\] Placing filler cells…"
foreach row {IO_NORTH IO_SOUTH IO_WEST IO_EAST} {
    place_io_fill -row $row {*}$::env(PAD_FILLERS)
}

puts "\[INFO\] Connecting ring signals…"
connect_by_abutment

if { [info exists ::env(PAD_BONDPAD_NAME)] } {
    puts "\[INFO\] Placing bondpads…"
    foreach pad $pads {
        set inst_name [lindex $pad 0]
        set master_name [[[$block findInst $inst_name] getMaster] getName]
        dict for {master_regex offset} $::env(PAD_BONDPAD_OFFSETS) {
            if {[regexp $master_regex $master_name]} {
                place_bondpad -bond $::env(PAD_BONDPAD_NAME) $inst_name \
                    -offset [list [lindex $offset 0] [lindex $offset 1]]
            }
        }
    }
}

if { [info exists ::env(PAD_PLACE_IO_TERMINALS)] } {
    puts "\[INFO\] Placing I/O terminals…"
    foreach pad $pads {
        set inst_name [lindex $pad 0]
        set master_name [[[$block findInst $inst_name] getMaster] getName]
        foreach master_pin $::env(PAD_PLACE_IO_TERMINALS) {
            lassign [split $master_pin /] check_master pin_name
            if {$master_name == $check_master} {
                place_io_terminals $inst_name/$pin_name
                break
            }
        }
    }
}

puts "\[INFO\] Removing I/O rows…"
remove_io_rows
