## Building and Running

### Prerequisites
- C++17 compiler
- pthread support
- POSIX

### build

```bash
cd position-distributor

make
#debug mode
make debug
```

### Running

1. Start the postion_server:
```bash
./position_server
```

2. Start multiple clients with different name:

```bash
# Example for Binance strategy
./position_client BN

# Example for Huobi strategy
./position_client HB

# Example for KuCoin strategy
./position_client KC
```

### Simulation

#### The clients automatically generate random position updates for testing purposes. When run, you should see:
1. Each client generates random position updates for various symbols
2. All clients receive updates from other clients
3. The sequence of updates is preserved across all clients
4. When a client reconnects, it receives all updates it missed while disconnected

#### If you kill one of the clients:
1. Start the client again with the same name
2. The client receives all position updates it missed

#### If you try to restart the server:
And for all the position updates are stored in position_server.log in the directory. 
If you stop the server and restart the server again, you can see the server will be restarted based on the position_server.log and carry on the future storation task after this.

### Structure
The implementation uses a client-server architecture in TCP protocol:

- **Position Server**: Central component that assigns sequence numbers, logs messages, and distributes position updates to all clients.
- **Position Clients**: Individual strategy processes both send and receive position updates.

#### Key notes
- **Global Sequencing**: The server assigns increasing sequence numbers to all messages.
- **Ack**: Clients track the last received sequence number and send acks to enable recovery of missed messages.
- **Non-blocking I/O**: All socket operations use non-blocking mode
- **MessageQueue**: Thread-safe queue for ordered message processing


### Futher Improvements for Low latency

- **TCP_NODELAY disables Nagle's algorithm**: Each message will be sent immediately without waiting to be combined with other messages.
- **Socket Buffer Tuning**: sets both send and receive buffers to 1MB to handle bursts of data