from __future__ import annotations

import dataclasses
from time import sleep
import subprocess
import shlex
import os
import signal

import exp_utils
import numpy as np
import scipy.stats as sps

# NOT FORGET ABOUT FLUSH

DELAY = 25
MEAN_BURST_LEN = 3
MEAN_GOOD_BURST_LEN = 10
#LOSSES = [0.0001, 0.001, 0.01]  # convert to percentage
#MUES = [0.05, 0.2, 0.5] # convert to percentage
#JITTERS = [1, 5, 15]
LOSSES = [0.001, 0.01]
MUES = [0.5, ]
JITTERS = [2, ]

N_REPEATS = 2
N_PKTS = 1000
CLT_FILE = 'clt_pkts.pcap'
SRV_FILE = 'srv_pkts.pcap'

CLT_TMP_FILE = 'clt.csv'
SRV_TMP_FILE = 'srv.csv'

EXP_LOGFILE = 'exp_log.txt'

RES_FILE = 'exp_results.csv'


TC = "./iproute2_dlc/tc/tc"
TCPDUMP = '/tmp/tcpdump'    # sudo cp /usr/sbin/tcpdump /tmp/tcpdump

MODE = 'DLC'
#MODE = 'NETEM'

# Процессы, которые нужно убивать вручную
processes: list[subprocess.Popen] = []


@dataclasses.dataclass
class ExpParams:
    exp_i: int
    delay: int
    mean_burst_len: int
    mean_good_burst_len: int
    loss: float
    mu: float
    jitter: float


@dataclasses.dataclass
class AnalyzeResults:
    loss: float
    delay: float
    jitter: float
    mean_burst: float
    corr: float


def kill_zombies():
    for name in ['tcpdump', 'iperf3']:
        subprocess.run(['sudo', 'pkill', '-9', '-f', name])

def run_bg(cmd: str):
    global logfile
    # Background
    proc = subprocess.Popen(shlex.split(cmd), stdout=logfile, stderr=logfile, preexec_fn=os.setsid)
    processes.append(proc)
    return proc


def run_fg(cmd: str, check: bool = True):
    global logfile
    # Blocking
    return subprocess.run(shlex.split(cmd), stdout=logfile, stderr=logfile, check=check)


def get_dlc_cmd(exp_params: ExpParams):
    return (f"{TC} qdisc add dev veth0 root dlc limit 100000 "
            f"delay {exp_params.delay}ms {exp_params.jitter}ms loss {exp_params.loss*100}% "
            f"mu {exp_params.mu*100}% mean_burst_len {exp_params.mean_burst_len} "
            f"mean_good_burst_len {exp_params.mean_good_burst_len} rate 100mbit")


def get_netem_cmd(exp_params: ExpParams):
    p31 = 1 / exp_params.mean_burst_len
    p13 = exp_params.loss / (exp_params.mean_burst_len * (1 - exp_params.loss))
    return (f"{TC} qdisc add dev veth0 root netem limit 100000 "
            f"delay {exp_params.delay}ms {exp_params.jitter}ms loss state {p13*100:.5f}% {p31*100:.5f}% rate 100mbit") 


def setup_exp(exp_params: ExpParams):
    if MODE == "DLC":
        qdisc_cmd = get_dlc_cmd(exp_params)
    elif MODE == "NETEM":
        qdisc_cmd = get_netem_cmd(exp_params)
    else:
        raise ValueError("Unknown mode")
    run_fg(qdisc_cmd)

    run_bg(f"{TCPDUMP} -i nflog:1 -s 0 -w {CLT_FILE}")
    run_bg(f"ip netns exec h1 {TCPDUMP} -i veth1 udp -s 100 -w {SRV_FILE}")

    run_bg("ip netns exec h1 iperf3 -s")
    sleep(0.5)
    run_fg(f"iperf3 -c 10.1.1.2 -u -b 40m -k {N_PKTS}")


def cleanup_exp():
    for proc in processes:
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
        except Exception as e:
            print(f"Error killing proc: {e}")
    processes.clear()
    kill_zombies()
    # Удалить qdisc
    try:
        run_fg(f"{TC} qdisc del dev veth0 root", check=False)
    except Exception as e:
        print('Error:', e)


def analyze(exp_params: ExpParams) -> AnalyzeResults:
    global logfile
    # tshark в csv
    with open(CLT_TMP_FILE, 'w') as clt_csv:
        cmd = f'tshark -r {CLT_FILE} -Y \"iperf3.sequence\" -e frame.number -e frame.time_epoch -e iperf3.sequence -E separator=, -T fields'
        subprocess.run(shlex.split(cmd), stdout=clt_csv, stderr=logfile, check=True)
    with open(SRV_TMP_FILE, 'w') as srv_csv:
        cmd = f'tshark -r {SRV_FILE} -Y \"iperf3.sequence\" -e frame.number -e frame.time_epoch -e iperf3.sequence -E separator=, -T fields'
        subprocess.run(shlex.split(cmd), stdout=srv_csv, stderr=logfile, check=True)

    max_delay = (exp_params.delay + exp_params.jitter) / 1000
    delays, losses = exp_utils.get_delay_and_loss(CLT_TMP_FILE, SRV_TMP_FILE, max_delay=max_delay)
    avg_loss = exp_utils.get_average_loss(losses)
    avg_delay = exp_utils.get_average_delay(delays, losses)
    jitter = exp_utils.get_jitter(delays, losses)
    avg_burst = exp_utils.get_average_loss_burst_len(losses)

    corr = 0.0
    if len(delays) > 2 and np.var(losses) > 0:
        corr = sps.pearsonr(delays, losses)[0]
        if np.isnan(corr):
            corr = 0.0
    return AnalyzeResults(
        loss=avg_loss,
        delay=avg_delay,
        jitter=jitter,
        mean_burst=avg_burst,
        corr=corr,
    )


def run_experiment(exp_params: ExpParams) -> AnalyzeResults:
    try:
        setup_exp(exp_params)
        sleep(0.1)
        cleanup_exp()
    except Exception as e:
        print("Error exp_run:", e)
        return AnalyzeResults(-1, -1, -1, -1, -1)
    sleep(0.1)
    try: 
        return analyze(exp_params)
    except Exception as e:
        print("Error exp_analyze:", e)
        return AnalyzeResults(-1, -1, -1, -1, -1)


if __name__ == '__main__':
    if os.geteuid() != 0:
        exit("You need to have root privileges to run this script.\nPlease try again, this time using 'sudo'. Exiting.")
    print(f"Stating {MODE} exps!")
    header = 'exp_n,delay,mean_burst_len,mean_good_burst_len,loss%,mu%,jitter,comp_delay,comp_jitter,comp_loss,comp_mean_burst_len,correlation\n'
    with open(RES_FILE, 'w') as res_file:
        res_file.write(header)
        for loss in LOSSES:
            for mu in MUES:
                for jitter in JITTERS:
                    for exp_i in range(N_REPEATS):
                        with open(EXP_LOGFILE, 'w') as logfile:
                            print(f'loss={loss}, mu={mu}, jitter={jitter}, exp_i={exp_i}', flush=True)
                            exp_params = ExpParams(
                                exp_i=exp_i,
                                delay=DELAY,
                                mean_burst_len=MEAN_BURST_LEN,
                                mean_good_burst_len=MEAN_GOOD_BURST_LEN,
                                loss=loss,
                                mu=mu,
                                jitter=jitter,
                            )
                            r = run_experiment(exp_params)
                            res_file.write(f'{exp_i},{DELAY},{MEAN_BURST_LEN},{MEAN_GOOD_BURST_LEN},{loss*100}%,{mu*100}%,{jitter},{r.delay},{r.jitter},{r.loss},{r.mean_burst},{r.corr}\n')
                            res_file.flush()
                            sleep(0.5)
    print('Complete!')
