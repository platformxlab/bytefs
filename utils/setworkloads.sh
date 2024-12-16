#!/bin/bash

git clone https://github.com/filebench/filebench.git
cd filebench

sudo apt-get install libtool automake -y

libtoolize
aclocal
autoheader
automake --add-missing
autoconf
./configure
make
make install

cd ..

# # filebench set
echo 0 | sudo tee /proc/sys/kernel/randomize_va_space
# echo "echo 0 | tee /proc/sys/kernel/randomize_va_space" >> ~/.bashrc

# set up maven 
sudo apt-get install maven -y

#set up java
sudo apt-get install openjdk-8-jdk -y
# include java in path
export JAVA_HOME=/usr/lib/jvm/java-8-openjdk-amd64
# export PATH=$JAVA_HOME/bin:$PATH


# SET UP ycsb
sudo apt-get install python
git clone https://github.com/brianfrankcooper/YCSB.git
cd YCSB
mvn -pl site.ycsb:rocksdb-binding -am clean package
cd ..


exit 0

sudo apt-get install openjdk-17-jdk -y
# include java in path
export JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64
# export PATH=$JAVA_HOME/bin:$PATH



# SET UP benchbase
git clone --depth 1 https://github.com/cmu-db/benchbase.git
cd benchbase
./mvnw clean package -P mysql
cd ..


# setup mysql 
