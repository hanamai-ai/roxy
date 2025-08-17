# Roxy

A lightweight Redis Layer 7 proxy.

- epoll based, can manage thousands of connections
- single threaded (for now)
- supports Redis protocol
- can intercept and modify requests and responses

## Prerequisites

- Linux (tested on Ubuntu 24.04)
- C toolchain:
  - make
  - a C compiler (e.g., gcc or clang)

## Build

From the project root:

```make all```

## Unit testing

Incomplete, at the moment only the dbus module is tested.

```make test```

## Usage

- Run redis server
- Run roxy
- connect redis client to roxy

Example:

Open a terminal and run:

```redis-server```

Open another terminal and run:

```./roxy -vvvv```

Open another terminal and run:

```redis-cli -p 6380```

