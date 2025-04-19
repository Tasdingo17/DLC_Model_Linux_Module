# DLC Model for Linux kernel

## Introduction
Delay-loss correlation qdisc module for linux tc utility.

## Prerequisites

I don't know (sorry): mb only linux-headers package, mb whole kernel development kit.
Developed and teste in Ubuntu 20.04, kernel 5.4.0-212-generic/

Download custom iproute2 for passing parameters: https://github.com/Tasdingo17/iproute2_dlc

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
sudo insmod sch_dlc.ko
```
Ensure module successfully installed: `lsmod | grep sch_dlc` (or `dmesg | tail` for kernel logs)

4) (Optional) remove module after work:
```
sudo rmmod sch_dlc
```

## Usage example

1) Start: ..

2) Dump: ..
