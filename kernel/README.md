# Introduciton
This is an example for a kernel module that use the c-capnproto library.

# Getting started
1. Install capnpc
1. Follow the instructions and compile capnpc-c
1. Compile the `addressbook.capnp` example
```shell
capnp compile --verbose -o../capnpc-c -I ../compiler/ src/addressbook.capnp
```
1. Compile the kernel module
```shell
make
```
1. Load the kernel module
```shell
sudo insmod address.ko
```
