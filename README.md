# Chat-Server

A multi-user instant messaging (IM) server and client written in C using TCP sockets. The server handles each user on its own thread, and users can list who is online, send private messages, broadcast to everyone, or message a random user.

## Components

| File | Description |
|------|-------------|
| `sserver.c` | Multithreaded server (pthreads + a mutex-protected client table, up to 100 users) |
| `sclient.c` | Terminal client that uses `select()` to read from the keyboard and the server at the same time |

## Build

```bash
gcc -Wall -pthread -o sserver sserver.c
gcc -Wall -o sclient sclient.c
```

## Run

Start the server (default port is 5000; if that port is busy it tries the next one up):

```bash
./sserver [port]
# IM server listening on port 5000
```

Connect one or more clients:

```bash
./sclient <server-ip> <port>
# e.g. ./sclient 127.0.0.1 5000
```

The first line you type is your **username** (it must be unique). After that you can use these commands:

| Command | What it does |
|---------|--------------|
| `list` | Show everyone who is online |
| `send <user> <msg>` | Send a private message to one user |
| `broadcast <msg>` | Send a message to everyone (including you) |
| `random <msg>` | Send a message to one randomly chosen online user |
| `close` | Disconnect |

Other users are notified when someone joins or leaves.

## Example

```
Connected. Send your username as the first line, then IM commands.
oliver
Welcome oliver!
...
send alice hey, are you there?
Message sent to alice.
broadcast hello everyone
[broadcast] oliver: hello everyone
```
