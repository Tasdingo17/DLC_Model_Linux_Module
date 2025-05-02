# DLC Model for Linux kernel

## Introduction
Delay-loss correlation qdisc module for linux tc utility.

## Prerequisites

I don't know (sorry): mb only linux-headers package, mb whole kernel development kit.
Developed and tested in Ubuntu 20.04, kernel 5.4.0-212-generic

Download custom iproute2 for calculating and passing parameters: https://github.com/Tasdingo17/iproute2_dlc

## Build

1) Build modified iproute2 for tc: 
```
[in iproute2 repo]
./configure
make
```
Note: to run build with use `tc/tc qdisc ...` or replace system tc somehow

2) Build dlc_model: (in dlc repo)
```
[in dlc repo]
make
```

3) Install dlc module:
```
sudo insmod sch_dlc_qdisc.ko
```
Ensure module successfully installed: `lsmod | grep sch_dlc_qdisc` (or `dmesg | tail` for kernel logs)

4) (Optional) remove module after work:
```
sudo rmmod sch_dlc_qdisc
```

## Usage example

**Start**: 
```
sudo iproute2_dlc/tc/tc qdisc add dev veth0 root dlc limit 10000 delay 10ms 2ms loss 1% mu 30% mean_burst_len 3 mean_good_burst_len 15 rate 50mbit
```

**Testbed**: check `Makefile.testbed` for testbed setup and work check.

Note: `Makefile.testbed` supposes following directory structure:
- Makefile
- iproute2_dlc
- DLC_Model_module
