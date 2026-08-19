# Copyright (C) 2023 Alif Semiconductor - All Rights Reserved.
# Use, distribution and modification of this code is permitted under the
# terms stated in the Alif Semiconductor Software License Agreement
#
# You should have received a copy of the Alif Semiconductor Software
# License Agreement with this file. If not, please write to:
# contact@alifsemi.com, or visit: https://alifsemi.com/license

import argparse
import numpy as np
import matplotlib.pyplot as plt

STRUCT_SIZE = 20

def parse_args():
    parser = argparse.ArgumentParser(description='''Script to plot LE audio presentation
                                     compensation debug data''',
                                     epilog='''Data can be logged using the
                                     CONFIG_PRESENTATION_COMPENSATION_DEBUG Kconfig option. When
                                     the debug callback is called, dump the data from the debug
                                     buffer to a binary file (e.g. using GDB: "dump binary memory
                                     <filename>.bin dbg_data dbg_data +
                                     CONFIG_PRESENTATION_COMPENSATION_DEBUG_SAMPLES"). The binary
                                     file should be passed to this script using the -f argument''')
    parser.add_argument("--file", "-f", required=True, type=str, help="Filename of the binary file to parse and plot")

    return parser.parse_args()

def plot_data(data):
    fig = plt.figure()

    gs = fig.add_gridspec(5, 1, hspace=0.1)

    axs = gs.subplots(sharex='col')

    axs[0].plot(data['error_us'])
    axs[0].set_ylabel("Error (us)")

    axs[1].plot(data['correction_us'])
    axs[1].set_ylabel("Correction (us)")

    axs[2].plot(data['clock_freq'])
    axs[2].set_ylabel("Clock frequency (Hz)")

    axs[3].plot(data['pi_output'],)
    axs[3].set_ylabel("PI output (Hz)")

    axs[4].plot(data['pi_integrator'])
    axs[4].set_ylabel("PI integrator")

    for ax in axs:
        ax.grid()

    plt.show()

def main():
    args = parse_args()

    with open(args.file, "rb") as f:
        raw_data = f.read()

    assert(0 == (len(raw_data) % STRUCT_SIZE))

    datatype = np.dtype([('error_us', '<i4'),
                         ('correction_us', '<i4'),
                         ('clock_freq', '<u4'),
                         ('pi_output', '<f4'),
                         ('pi_integrator', '<f4')])
    parsed_data = np.frombuffer(raw_data, dtype=datatype)

    plot_data(parsed_data)

if __name__ == "__main__":
    main()