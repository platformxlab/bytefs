#!/bin/bash
sudo mkfs -t ext4 /dev/pmem0
sudo mount /dev/pmem0 /mnt/edrive
sudo chown csl /mnt/edrive

echo 0 | sudo tee /proc/sys/kernel/randomize_va_space
