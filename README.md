# ByteFS: System Support for Memory-Semantic Solid-State Drives

## Installation

This part we will describe on how to build the artifact with the emulator version. Start by downloading the artifact from github repository:

```bash
git clone https://github.com/pollux006/ByteFS_AE.git
cd ByteFS_AE
```

Checkout branch according to the version you want to build:

```bash
git checkout baseline # or
git checkout bytefs
```

### Kernel compilation

To built a kernel on the system, there are some packages needed before you can successfully build. You can get these installed with:

```bash
sudo apt install libncurses-dev gawk flex bison openssl libssl-dev dkms libelf-dev libudev-dev libpci-dev libiberty-dev autoconf llvm
```

Now enter the linux repository and enable the configuration for the kernel:

```bash
cd ./linux
make menuconfig
```

Within the kernel configuration menu, make sure to enable the following options:

```menuconfig
# To enable ByteFS select the following
File systems --->
    <*> BYTEFS: ByteFS
# To enable baseline file systems in baseline branch:
File systems --->
    <*> The Extended 4 (ext4) filesystem
    <M> F2FS filesystem support
    <*> NOVA: log-structured file system for non-volatile memories
    Miscellaneous filesystems --->
        <M> Persistent and Protected PM file system support
```

We also provide a default configuration file for the kernel under the directory. Note that the provided config may not
apply for all setting. After the configuration, you can build the kernel with the
following command:

```bash
make -j $(threads)
make modules_install
sudo make install
```

Reboot the system to apply the new kernel.

#### Reserve DRAM space for emulation

Next, to reserve memory region for emulated flash memory, modify the Kernel command line parameters:

```bash
sudo vim /etc/default/grub # or use any other editor
# add/update grub cmdline:
"GRUB_CMDLINE_LINUX="memmap=nn[KMG]!ss[KMG]"
sudo update-grub
```

The cmdline `memmap=nn[KMG]!ss[KMG]` sets the region of memory to be used, from ss to ss+nn, and [KMG] refers to Kilo, Mega, Giga (e.g., `memmap=4G!12G` reserves 4GB of memory between 12th and 16th GB). Configuration is done within GRUB, and varies between Linux distributions. In default setting, set nn=32G and ss=32G as default. After reboot the system, you can check the reserved memory region with:

```bash
lsblk
```

You should see a new device named pmem0 with the set size.

### Setting up the workloads

Run the following commands to install the necessary workloads:

```bash
sudo ./setworkloads.sh
```

## Evaluation and Expected Results

After the setting up the experiment environment, you can run specific benchmarks with the following command:

```bash
cd ./utils
# run a given workload using the filesystem specified, and output to designated folder
./benchmark_run -w ${workload} -f ${filesystem} -t ${output_folder}
```

The detail options and instructions can be found in the help message of the script with `-h` option.

Because of different workload settings, we may require rebooting of the machine in between different run configurations. We also offers a script that runs all the experiments and data gathering by executing:

```bash
cd ./utils
# execute benchmark_run.sh on specified remote_target via ssh, and copy output back to local designated folder
./run_all.sh -r ${remote_target} -s ${workload_set} -f ${filesystem} -t ${output_folder}
```

The detail options and instructions can be found in the help message of the script with -h option. To plot the result figures, you can use the following scripts:

```bash
cd ./figure_drawing
python3 draw.py
```

The script will automatically search for all drawing configurations under the figure_drawing/figures folder and plot them accordingly. The detailed correspondence of figures in the paperTo verify the results, one can compare the generated figures directly with those in the paper, or compare the data for each figure with the example results we provided. As the detail results may vary depending on the system, the results should follow the same trend as provided. The detailed correspondence of figures in the paper is documented in each of the subfolders. More instructions on how to run the experiments and generate the figures can be found in the README file in github repository.

## Experiment Customization

1. Changing workload settings. Users can customize their own workload by modifying the provided configurations under `./utils/filebench_workloads` or `./utils/ycsb_workloads` and evaluate them.
2. Changing emulated SSD capcity. Users can change the emulated SSD capcity by modifying the kernel command line parameters reserving more DRAM space to perform the experiments. The user should also align the parameter in "./linux/ssd/ftl.h", changing the physical address range (`BYTEFS_PA_START` and `BYTEFS_PA_END`) and also flash parameters (`CH_COUNT`, `WAY_COUNT`, `BLOCK_COUNT`, and `PG_COUNT`). To apply the changes,
re-compiling the kernel.
3. Changing SSD timing model. The user can change the SSD timing model in "./linux/ssd/timing_model.h". Changes will be applied after re-compiling the kernel.
4. Changing SSD log size. The user can change the SSD log size in "./linux/ssd/ftl.h". Changes will be applied after re-compiling the kernel.

