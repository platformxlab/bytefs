#!/bin/bash
sudo modprobe f2fs
sudo rm -rf /mnt/edrive/*
sudo mkfs -t f2fs /dev/pmem0
sudo mount /dev/pmem0 /mnt/edrive
sudo chown csl /mnt/edrive

echo 0 | sudo tee /proc/sys/kernel/randomize_va_space
