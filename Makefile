# Makefile

# main object file for module
obj-m += sch_dlc_qdisc.o

# # define object-dependencies for sch_dlc_qdisc: sch_dlc_qdisc-objs
# DLC_SRCS += $(wildcard dlc/*.c)
# $(info DLC_SRCS: $(DLC_SRCS))

# DLC_OBJS += $(patsubst %.c,%.o,$(DLC_SRCS))
# $(info DLC_OBJS: $(DLC_OBJS))

DLC_OBJS = dlc/dlc_random.o dlc/markov_chain.o dlc/states.o dlc/dlc_mod.o

sch_dlc_qdisc-objs = sch_dlc.o $(DLC_OBJS)

# complile with kernel flows
all:
	make -C /lib/modules/$(shell uname -r)/build M=$(shell pwd) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(shell pwd) clean
