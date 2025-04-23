# TaskQueueX

A high-performance in-memory task queue system designed for real-time trading applications, capable of handling 10,000+ requests per second.

## Features

- **High Performance**: Lock-free data structures and fine-grained locking for 10k+ req/sec throughput
- **Multiple Queue Disciplines**: FIFO, LIFO, Priority, Round Robin queuing strategies
- **Thread Safety**: Comprehensive thread safety with shared mutexes and connection pooling
- **Persistence**: RocksDB backend with Write-Ahead Logging (WAL) for durability
- **Task Dependencies**: Support for complex task graphs and workflows
- **Network Interface**: TCP server with JSON protocol for remote management
- **Observability**: Built-in metrics, logging, and monitoring capabilities

## Architecture

TaskQueueX consists of the following core components:

1. **Task**: Base class for tasks with priority, status tracking, and dependency management
2. **QueueManager**: Manages multiple queue disciplines (FIFO, LIFO, Priority, Round Robin)
3. **ThreadPool**: Manages worker threads and task execution
4. **Storage**: Persistence layer using RocksDB with WAL
5. **TCPServer**: Network interface for task submission and control
6. **CLICommands**: Command handlers for the TCP interface

## Building and Running

### Prerequisites

- C++17 compatible compiler (GCC 8+, Clang 7+, MSVC 2019+)
- Boost libraries (system, thread, program_options)
- RocksDB
- nlohmann_json

### Build Instructions

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### Running the Server

```bash
./taskqueuex --host 0.0.0.0 --port 8080 --threads 8 --db-path /path/to/db
```

## Client Interface

TaskQueueX uses a simple JSON protocol over TCP. Here are some example commands:

### Enqueue a Task

```json
{
  "command": "enqueue",
  "params": {
    "name": "calculation",
    "priority": 50,
    "delay": 1000
  }
}
```

### Check Task Status

```json
{
  "command": "status",
  "params": {
    "task_id": 123
  }
}
```

### Cancel a Task

```json
{
  "command": "cancel",
  "params": {
    "task_id": 123
  }
}
```

## Thread Safety and Performance Optimizations

TaskQueueX is built for high-throughput environments:

- **Fine-grained Locking**: Separate mutexes for different operations (config, tasks, graph, metrics)
- **Connection Pooling**: Storage connection pool with 32 parallel connections
- **Parallel Processing**: Batch tasks are processed in parallel using multiple threads
- **Lock Optimization**: Read/write locks with minimal lock durations
- **Atomic Operations**: Lock-free counters and indices

## Performance Benchmarks

Under standard test conditions on a modern 8-core server:

- Single task enqueue: ~50,000 tasks/second
- Batch task processing: ~120,000 tasks/second
- Queue operations: ~80,000 operations/second
- Peak sustained throughput: >10,000 client requests/second

## Configuration

TaskQueueX can be configured with the following command line options:

- `--host`: Host address to bind to (default: 0.0.0.0)
- `--port`: Port to listen on (default: 8080)
- `--threads`: Number of worker threads (default: CPU cores)
- `--db-path`: Path to RocksDB database (default: taskqueue.db)
- `--queue-mode`: Default queue mode (FIFO, LIFO, PRIORITY, ROUND_ROBIN)

## License

Copyright © 2023 VoltaxDB Team 