#!/bin/bash
sudo rm -rf /mnt/edrive/*
sudo mount -t NOVA -o init /dev/pmem0 /mnt/edrive
sudo chown csl /mnt/edrive

echo 0 | sudo tee /proc/sys/kernel/randomize_va_space