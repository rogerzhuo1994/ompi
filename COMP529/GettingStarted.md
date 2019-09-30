# Getting started with OpenMPI for COMP529

## Basic Information about the Stack
MPI (Message Passing Interface) is a commonly used standard interface for communication for HPC applications.
We will be using an implementation of MPI called OpenMPI.

OpenMPI is typically used over an InfiniBand network, but can also be used with various other transport mechanisms.
We will use an intermediate layer called portals4 that can communicate over UDP.

    ----------------------
    |  HPC Application   |
    ----------------------
    |      OpenMPI       |
    ----------------------
    |     Portals4       |
    ----------------------
    |    UDP sockets     |
    ----------------------

## Building everything
Portals4 requires `libev` as a build dependency, so we'll build that first.
### Building libev
    wget http://dist.schmorp.de/libev/libev-4.27.tar.gz
    tar xzf libev-4.27.tar.gz
    cd libev-4.27
    mkdir _install
    ./configure --prefix=`pwd`/_install
    make && make install


### Building Portals4
    git clone https://github.com/Portals4/portals4.git
    cd portals4/
    autoreconf -i
    mkdir _build _install
    cd _build
    ../configure --disable-transport-ib --prefix=`pwd`/../_install --with-ev=/PATH/TO/libev-4.27/_install/ --enable-zero-mrs --enable-transport-udp --enable-reliable-udp --disable-transport-shmem
    make -j
    make check 
    make install
    
There's a race condition that can cause `make check` to fail on one test.
Don't be alarmed if it does.

### Building OpenMPI
    cd ~
    git clone https://github.com/ptaffet/ompi.git
    git checkout ipmulticast
    mkdir _build _install
    cd _build
    ../configure --prefix=`pwd`/../_install --with-portals4=/PATH/TO/portals4/_install
    make -j
    make install

That's all you need to get started.
If you have question or encounter problems, ask me for help.

## Running MPI applications
In our current setup, MPI is a little tricky to run. 
Essentially, when you launch MPI, it `ssh`-es to the machines you want to run across, launches the right binary on all those machines, and then sets up all the communication.

One implication of this is that you need password-less ssh set up, at least from one node to the rest.
I'm writing, compiling, and launching code from `bold-node012`, so on that machine, I ran:

    ssh-keygen
    ssh-copy-id bold-node012    

Another implication of how MPI launches binaries is that every machine has to have a local copy of your MPI libraries and the binary you want to run.

I use a script like this in `~/openmpi-4.0.1/_build/doall.sh`:

    #!/bin/bash
    set -e
    make -j
    make install
    ssh bold-node013 "rm -rf ~/openmpi-4.0.1/_install"
    scp -r /home/pt2/openmpi-4.0.1/_install bold-node013:openmpi-4.0.1/
    
At some point before this, create `~/openmpi-4.0.1` on `bold-node013`.

Launching an MPI application is also a little tricky since the testbed has MPI version 1.6.5 already.
First, make sure your version of MPI is before that version in the `PATH`.
For example, `export PATH=/home/pt2/openmpi-4.0.1/_install/bin/:$PATH`.
Since `PATH` is not automatically propogated via SSH or set on every other node, you need to tell MPI where to find itself on other machines. 
The easiest way to do this is to launch with 
    
    `which mpirun` ...

The shell expands `` `which mpirun` `` into the full path for `mpirun` and then MPI uses that path to look for itself on other machines.
You can also use the `--prefix` flag for the same purpose.

Here's an example of a full command line:

    `which mpirun` -n 2 -H bold-node012,bold-node013  --report-bindings --mca pml cm --mca mtl portals4    ./a.out

* `-n 2` means launch 2 MPI ranks (i.e. two processes). This is 2 total, which means 1 on each machine.
* `-H bold-node012,bold-node013` tells MPI which machines to use (the host list)
* `--report-bindings` prints a message to `stdout` during initialization telling you which rank is running on which node. It can be useful to help you debug.
* `--mca pml cm --mca mtl portals4` tells MPI to use portals4. If you don't use this, it will use TCP instead and not invoke the changes you've made to the MPI library.
* `./a.out` the name of the actual binary you want to run

## Building MPI applications
Unlike many other tools, MPI provides compiler wrappers that add the write library and include paths.
So basically, with your custom version of MPI in your `PATH`, you use `mpicc` where you would use `gcc` and `mpicxx` where you would use `g++`.
For example

    mpicc -g hello.c -std=c11

You need to keep the compiled version in sync across all nodes that will launch it or you will get very strange errors.
Get in the habit of `scp`-ing the binary as soon as you compile it.
