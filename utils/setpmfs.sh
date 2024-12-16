#!/bin/bash
sudo modprobe pmfs
sudo mount -t pmfs -o init /dev/pmem0 /mnt/edrive
sudo chown csl /mnt/edrive

echo 0 | sudo tee /proc/sys/kernel/randomize_va_space
