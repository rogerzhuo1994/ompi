#!/bin/bash

RUNPREFIX="`which mpirun` --report-bindings --mca pml cm --mca mtl portals4 -H bold-node012,bold-node013,bold-node014,bold-node015"

# 50 points: Correctness of transfer under no interfering traffic
# Two ranks, small message (1 datagram worth), synchronization in between
$RUNPREFIX -n 2 ./grade  -z small -y yes -s 0 -l 0 -b 1

# Two ranks, large message, synchronization in between
$RUNPREFIX -n 2 ./grade  -z large -y yes -s 0 -l 0 -b 1

# Four ranks, large message, only one sender
$RUNPREFIX -n 4 ./grade  -z large -y no -s 0 -l 0 -b 1

# Four ranks, large message, rotating sender
$RUNPREFIX -n 4 ./grade  -z large -y no -s mix -l 0 -b 1

# Four ranks, small message, one late receiver
$RUNPREFIX -n 4 ./grade  -z small -y no -s mix -l 1 -b 1

# Four ranks, small message, three late receivers
$RUNPREFIX -n 4 ./grade  -z small -y no -s mix -l 3 -b 1

# Four ranks, large messages, three late receivers
$RUNPREFIX -n 4 ./grade  -z large -y no -s mix -l 3 -b 1

# Two simultaneous broadcasts on different non-overlapping subcommunicators, small messages
$RUNPREFIX -n 4 ./grade  -z small -y no -s 0 -l 0 -b 2

# Two simultaneous broadcasts on different non-overlapping subcommunicators, large messages
$RUNPREFIX -n 4 ./grade  -z large -y no -s 0 -l 0 -b 2

# Simultaneous broadcasts on overlapping subcommunicators, mix of small and large messages, so some late receivers
$RUNPREFIX -n 4 ./grade  -z mix -y no -s 0 -l 0 -b 2

# 
# 10 points: Speed under no interfering traffic. 1 point for 1 Gbps, 2 for 2 Gbps, ... , 10 for 10 Gbps
# 10 GB message, 3 receivers
# 
$RUNPREFIX -n 4 ./grade  -z huge -y no -s 0 -l 0 -b 1


for flags in "-u -b 1000M -l 8k" "-l 1.8k"
do
	TCP_IPERF_S_P=$(mktemp -u)
	TCP_IPERF_C_P=$(mktemp -u)
	ssh -f -M -S $TCP_IPERF_S_P  bold-node013 "iperf $uflag -s "
	ssh -f -M -S $TCP_IPERF_C_P  bold-node016 "iperf $uflag -c 192.168.50.124 -t 1000 "
	$RUNPREFIX -n 4 ./grade  -z huge -y no -s 0 -l 0 -b 1

	ssh -S $TCP_IPERF_S_P -O exit bold-node013
	ssh -S $TCP_IPERF_C_P -O exit bold-node016
	ssh bold-node016 "pkill iperf"
	ssh bold-node013 "pkill iperf"
done

# 10 points: Correctness of transfer under 1 TCP flow @ max 1 Gbps interference
# Node 5 sends traffic to the last receiver
# Node 5 sends 1 Gbps to each receiver
# 
# 10 points: Correctness of transfer under 1 UDP flow @ max 1 Gbps interference
# 
# 10 points: Speed under 1 TCP flow interference, 10 for 9 Gbps
# 
# 10 points: Speed under 1 UDP flow interference, 10 for 9 Gbps



