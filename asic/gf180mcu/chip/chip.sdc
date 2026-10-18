# chip.sdc - timing constraints of z80_chip: the clock enters through the
# CLK pad (CLOCK_NET, the pad's core-side output), every other pin is
# constrained against it. Based on the wafer.space gf180mcu project
# template's chip_top.sdc (Apache-2.0).

current_design $::env(DESIGN_NAME)
set_units -time ns

set clock_port [lindex $::env(CLOCK_PORT) 0]
create_clock [get_pins [lindex $::env(CLOCK_NET) 0]] -name $clock_port -period $::env(CLOCK_PERIOD)
set clocks [get_clocks $clock_port]

set io_delay [expr $::env(CLOCK_PERIOD) * $::env(IO_DELAY_CONSTRAINT) / 100]
puts "\[INFO] I/O delay: $io_delay ns"

set_max_fanout $::env(MAX_FANOUT_CONSTRAINT) [current_design]
if { [info exists ::env(MAX_TRANSITION_CONSTRAINT)] } {
    set_max_transition $::env(MAX_TRANSITION_CONSTRAINT) [current_design]
}
if { [info exists ::env(MAX_CAPACITANCE_CONSTRAINT)] } {
    set_max_capacitance $::env(MAX_CAPACITANCE_CONSTRAINT) [current_design]
}

# data bus: in and out
set bidir_ports [get_ports {D[*]}]
set_input_delay -min 0 -clock $clocks $bidir_ports
set_input_delay -max $io_delay -clock $clocks $bidir_ports
set_output_delay $io_delay -clock $clocks $bidir_ports

# control inputs
set input_ports [get_ports {RESET_n WAIT_n INT_n NMI_n BUSRQ_n}]
set_input_delay -min 0 -clock $clocks $input_ports
set_input_delay -max $io_delay -clock $clocks $input_ports

# address bus and control outputs
set output_ports [get_ports {A[*] M1_n MREQ_n IORQ_n RD_n WR_n RFSH_n HALT_n BUSAK_n}]
set_output_delay $io_delay -clock $clocks $output_ports

set_load [expr $::env(OUTPUT_CAP_LOAD) / 1000.0] [all_outputs]
set_clock_uncertainty $::env(CLOCK_UNCERTAINTY_CONSTRAINT) $clocks
set_clock_transition $::env(CLOCK_TRANSITION_CONSTRAINT) $clocks
set_timing_derate -early [expr 1 - $::env(TIME_DERATING_CONSTRAINT) / 100.0]
set_timing_derate -late [expr 1 + $::env(TIME_DERATING_CONSTRAINT) / 100.0]
