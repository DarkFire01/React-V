# React-V
React-V The open source hypervisor for freeloader booted operating systems.

## What this and what this isn't:
Hyper-V is a very complicated piece of software, one I've never been a fan of. This isn't meant to be documentation ON hyper-v.
The goal of this project is to reimplement an ABI compatible hypervisor that is compatible with the drivers and user mode applications.
targetting the 24H2 version of Hyper-V, while annoying me 30% less.

So if you're looking for documentation on hyper-v itself this probably isn't it :). if you're looking for chaos this is absolutely it.

React-V as I'm unoffically calling it is an library that gets linked into freeloader, during the boot process it puts the OS in a root partition, and then provides surfaces all through the OSes life.
This means powering the WHPX backend of qemu, or a custom VM software I'm slapping together myself. Or more interestingly; WSL.

# Supported Chipsets
Anything ARM64? No lol
Anything Intel? Not yet! I started it though!
Anything AMD? Ryzen all the way.