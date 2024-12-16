#!/bin/bash

sudo chown csl /mnt/edrive
mkdir /mnt/edrive/postgrel
sudo rsync -av /var/lib/postgresql /mnt/edrive
sudo mv /mnt/edrive/postgresql/10/main.bak /mnt/edrive/postgresql/10/main
sudo systemctl start postgresql
sudo -u postgres psql -c "ALTER USER csl PASSWORD 'password';"
sudo -u postgres psql -c "CREATE DATABASE benchbase;"
sudo -u postgres psql -c "GRANT ALL PRIVILEGES ON DATABASE benchbase TO csl;"
