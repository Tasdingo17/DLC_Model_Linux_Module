import argparse
from typing import List, Optional, Tuple

import numpy as np
import scipy.stats as sps
import pandas as pd

def get_average_loss(losses: List[int]) -> float:
    return np.array(losses).mean()


def get_average_delay(delays: List[float], losses: List[int]) -> float:
    assert len(losses) == len(delays)
    valuable_delays = [delay for i, delay in enumerate(delays) if losses[i] != 1]
    return np.array(valuable_delays).mean()


def get_jitter(delays: List[float], losses: List[int], mean: Optional[float] = None) -> float:
    assert len(losses) == len(delays)
    valuable_delays = np.array([delay for i, delay in enumerate(delays) if losses[i] != 1])
    true_mean = mean
    if not true_mean:
        true_mean = valuable_delays.mean()
    jitters = np.absolute(valuable_delays - true_mean)
    return jitters.max()


def get_average_loss_burst_len(losses: List[int]) -> float:
    burst_lens = []
    start, stop = 0, 0
    while start < len(losses):
        if losses[start] == 1:
            stop = start
            while stop < len(losses) and losses[stop] == 1:
                stop += 1
            burst_lens.append(stop - start)
            start = stop
        start += 1
    if not burst_lens:
        return 0
    return np.array(burst_lens).mean()


MAX_DELAY = -1

def parse_args():
    parser = argparse.ArgumentParser(description='udp exp parser')
    parser.add_argument('clt_file', type=str, help='Path to packets from input interface')
    parser.add_argument('srv_file', type=str, help='Path to packets from output interface')
    args = parser.parse_args()
    return args


def get_delay_and_loss(clt_file: str, srv_file: str, max_delay: float = MAX_DELAY) -> Tuple[List, List]:
    header = ['frame_idx', 'time_epoch', 'iperf_idx']
    df_src, df_dst = pd.read_csv(clt_file, names=header), pd.read_csv(srv_file, names=header)
    src_idx, dst_idx = 0, 0
    delays, losses = [], []
    while src_idx < df_src.shape[0] and dst_idx < df_dst.shape[0]:
        src_row = df_src.iloc[src_idx]
        if df_dst.iloc[dst_idx]['iperf_idx'] > src_row['iperf_idx']:
            losses.append(1)
            delays.append(max_delay)
            src_idx += 1
            continue
        delay = df_dst.iloc[dst_idx]['time_epoch'] - src_row['time_epoch']
        if delay > 0:
            losses.append(0)
            delays.append(delay)
        src_idx += 1
        dst_idx += 1
    return delays, losses


def print_stats() -> None:
    print("average loss:", get_average_loss(losses))
    print("average delay:", get_average_delay(delays, losses))
    print("jitter:", get_jitter(delays, losses))
    print("average loss burst len:", get_average_loss_burst_len(losses))
    corr = sps.pearsonr(delays, losses)[0]
    corr = corr if corr is not np.nan else 0.0
    print("Pearson corr:", corr)


def draw() -> None:
    pass


if __name__ == '__main__':
    args = parse_args()
    delays, losses = get_delay_and_loss(args.clt_file, args.srv_file)
    print_stats()
