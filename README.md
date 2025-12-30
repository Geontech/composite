# composite

**composite** is a lightweight framework for building componentized streaming applications.
It provides a modular approach to constructing streaming workflows.

## Features

- **Modular Architecture**: Build applications by composing reusable components.
- **Lightweight Design**: Minimal overhead ensures high performance in streaming scenarios.
- **Efficient Memory Management**: Minimize copies with smart pointer movement between component ports.

## Getting Started

### Prerequisites

Ensure you have the following installed:

- [CMake](https://cmake.org/) (version 3.15 or higher)
- A compatible C++ compiler (e.g., GCC, Clang) with C++20 support
- OpenSSL (version 3.0 or higher) if compiling with `-DCOMPOSITE_USE_OPENSSL=ON`
- [nats.c](https://github.com/nats-io/nats.c) if compiling with `-DCOMPOSITE_USE_NATS=ON`

### Build and Install

```cmake
cmake -B build
cmake --build build [--parallel N]
cmake --install build
```

### Build Options

- `COMPOSITE_USE_NATS`: Enable components to publish data to a NATS server on a defined subject
- `COMPOSITE_USE_OPENSSL`: Compile with OpenSSL support to enable a secure REST server

## Composite CLI Application and Configuration

The **composite** framework includes a command-line interface (CLI) application, `composite-cli`,
for running and managing composite applications. This application uses a JSON file for its configuration.

```bash
$ composite-cli -h
Usage: composite-cli [--help] [--version] [--server VAR] [--port VAR] [--log-level VAR] config-file

Positional arguments:
  config-file      application configuration file 

Optional arguments:
  -h, --help       shows help message and exits 
  -v, --version    prints version information and exits 
  -s, --server     REST server address [nargs=0..1] [default: "localhost"]
  -p, --port       REST server port [nargs=0..1] [default: 5000]
  -l, --log-level  log level [trace, debug, info, warning, error, critical, off] [nargs=0..1] [default: "info"]
```

### JSON Configuration File

The `composite-cli` application requires a JSON file to define the structure and behavior of the streaming application.
This file specifies the components, their properties, and how they are interconnected.

#### Schema Overview

The main structure of the JSON configuration file includes the following top-level keys:

1. **name (optional, string):** Specifies a name for the application. If not provided, a default name will be generated.

```json
{
    "name": "my_streaming_application"
    ...
}
```

2. **properties (optional, object):** Defines application-level properties that can be applied to components.
These properties are applied to all components and are applied before the component-level properties. All property
values must be defined as strings.

```json
{
    "name": "my_streaming_application",
    "properties": {
        "global_setting": "value1",
        "feature_flags": {
            "enable_x": "true"
        }
    }
    ...
}
```

3. **components (required, array of objects):** An array where each object defines a component to be loaded into the application.
Each component object must contain:
    - **id (required, string):** A unique identifier for this component instance. This identifier is used when making connections (see below)
      and is passed to the component's constructor.
    - **library (required, string):** The shared library to load. Can be either:
      - A filename (e.g., `"libmy_component.so"`) - searched in `LD_LIBRARY_PATH` and standard library paths
      - An absolute path (e.g., `"/path/to/libmy_component.so"`) - loaded from the specified location
    - **properties (optional, object):** Defines component-specific properties that can override application-level properties or
      provide unique configurations for this component. Just as with application-level properties, all property values must be defined
      as strings.

```json
{
    "name": "my_streaming_application",
    "properties": {
        ...
    },
    "components": [
        {
            "id": "my_component_instance",
            "library": "libmy_component.so",
            "properties": {
                "specific_param": "123",
                "processing_gain": "2.5"
            }
        }
    ]
}
```

4. **connections (required, array of objects):** An array where each object defines a connection between an output
port of one component and an input port of another component. Each connection object must contain:
    - **output (required, object):** Specifies the source of the data. Must contain:
      - `"component"`: The ID of the source component
      - `"port"`: The name of the output port
    - **input (required, object):** Specifies the destination of the data. Must contain:
      - `"component"`: The ID of the target component
      - `"port"`: The name of the input port

```json
{
    "name": "my_streaming_application",
    "properties": {
        ...
    },
    "components": [
        {
            "id": "sensor",
            "library": "libsensor_reader.so"
        },
        {
            "id": "processor",
            "library": "libdata_processor.so"
        },
        {
            "id": "writer",
            "library": "libfile_writer.so"
        }
    ],
    "connections": [
        {
            "output": { "component": "sensor", "port": "raw_data" },
            "input": { "component": "processor", "port": "data_in" }
        },
        {
            "output": { "component": "processor", "port": "processed_data" },
            "input": { "component": "writer", "port": "data_in" }
        }
    ]
}
```

**Note**: An output port can be connected to multiple input ports (fan-out), but an input port can only receive from one output port.

## Component Interface

The **composite** framework is designed around a component-based architecture.
Each component follows a well-defined interface that allows it to be integrated into a larger streaming pipeline.

### Component Loading

Components are dynamically loaded at runtime as shared libraries:

- **Library Path**: The `library` field in configuration can specify either:
  - A library filename (e.g., `"libmy_component.so"`) searched via `LD_LIBRARY_PATH` and standard locations
  - An absolute path (e.g., `"/opt/components/libmy_component.so"`) for direct loading
- **Factory Function**: Each component library must export a `create()` factory function with one of these signatures:
  ```cpp
  // For simple components:
  auto create(std::string_view id) -> std::shared_ptr<composite::component>

  // For components that need additional arguments:
  auto create(std::string_view id, std::string_view arg) -> std::shared_ptr<composite::component>
  ```
- **Component ID**: Each component instance must have a unique ID (required in configuration), which is passed to the constructor and used for connections and logging

### Component Lifecycle

Each component follows a well-defined lifecycle:

1. **Construction**: Component is instantiated, ports and properties are registered
2. **Initialize**: `initialize()` method is called to set up resources (optional override)
3. **Start**: Component thread is started (only if `enabled` property is `true`)
4. **Process Loop**: `process()` method is called repeatedly in the component's dedicated thread
5. **Stop**: Thread is stopped and resources are cleaned up

**Thread Management**: Each component runs in its own `std::jthread` managed by the framework. The thread name is set to the component ID for debugging purposes.

**Runtime Lifecycle Control**: Components can be dynamically enabled or disabled at runtime via the `enabled` property:

- Setting `enabled=false` stops the component thread and pauses all input ports (sets queue depth to 0)
  - Pausing input ports prevents memory bloat by immediately dropping incoming data instead of queuing it
- Setting `enabled=true` restarts the component thread and restores saved input port queue depths
  - After calling `set_properties()` to change the `enabled` property, you must call `apply_lifecycle_changes()` to apply the start/stop operation
  - This two-step process prevents deadlocks by deferring thread operations until after property locks are released

**Example - Programmatic Enable/Disable**:
```cpp
// Disable a component at runtime
component->set_properties({{"enabled", "false"}});
component->apply_lifecycle_changes();  // Required to trigger stop

// Later, re-enable the component
component->set_properties({{"enabled", "true"}});
component->apply_lifecycle_changes();  // Required to trigger start
```

> **Note**: When using the REST API to update properties, `apply_lifecycle_changes()` is called automatically by the server after property updates complete.

### Process Return Values

The `process()` method must return a `composite::retval` enum value that controls the component's execution flow:

- `NORMAL`: Normal processing occurred. Component yields to allow other threads to run.
- `NOOP`: No operation was performed (e.g., no data available). Component briefly sleeps before next iteration.
- `FINISH`: Component requests graceful shutdown. The thread will terminate after this return.
- `NO_YIELD`: Processing occurred but component should immediately call `process()` again without yielding.


## Ports

The **composite** framework provides a type-safe, buffer-based port system for connecting components.
The system facilitates the transfer of time-stamped contiguous data buffers and associated metadata between components,
with support for zero-copy optimizations, backpressure management, and comprehensive statistics tracking.

Each port is either an `input_port<BufferType>` or an `output_port<BufferType>`, where `BufferType` is one of:
- `mutable_buffer<T>`: Exclusive ownership buffer backed by `std::unique_ptr<std::vector<T>>`
- `immutable_buffer<T>`: Shared ownership buffer backed by `std::shared_ptr<const std::vector<T>>`

### Buffer Types

**Mutable Buffers** (`mutable_buffer<T>`):
- Support in-place modification via `operator[]` and iterators
- Enable zero-copy transfer when moving between ports
- Can be promoted to immutable buffers via `to_immutable()`
- Ideal for in-place processing and single-consumer workflows
- **Capacity management** (for dynamic containers like `std::vector`):
  - `resize(new_size)`: Change buffer size, expanding or truncating as needed
  - `reserve(new_capacity)`: Pre-allocate storage without changing size (avoids reallocations)
  - `capacity()`: Query current allocated capacity
  - `shrink_to_fit()`: Request reduction of capacity to match size
  - `clear()`: Remove all elements while preserving capacity
  - Example: `buffer.reserve(10000);  // Pre-allocate for 10k elements to avoid reallocation`

**Immutable Buffers** (`immutable_buffer<T>`):
- Read-only access to shared data
- Enable zero-copy sharing across multiple consumers via `share()`
- Backed by `shared_ptr` for efficient reference counting
- Ideal for broadcast/fan-out scenarios
- **Zero-copy slicing**: Create views of buffer subsets without copying data via `slice(offset, count)` or `slice_from(offset)`
  - Slices share the underlying data with the parent buffer
  - Use `immutable_buffer<T>::npos` as count parameter for "to end" semantics
  - Example: `auto header = buffer.slice(0, 64);  // First 64 elements`

### Output Port

The `output_port` class is responsible for publishing time-stamped buffer data to one or more connected `input_port`
instances or to a NATS subject if configured. It supports efficient transfer semantics by minimizing copies and
adjusting behavior based on the mutability of connected inputs.

#### Key Features

- Compatible with both `mutable_buffer<T>` and `immutable_buffer<T>`
- Ability to send metadata independently to connected input ports via `send_metadata()`
- Forwards data intelligently by choosing to move, copy, or promote based on buffer types
- **Statistics tracking**: Monitors packets/bytes transferred and throughput
- **Backpressure support**: Query connected port capacity with `can_send()`

#### Data Transfer Semantics

Behavior is determined by the buffer types of the output and input ports. The system optimizes for minimal copies
while respecting mutability constraints.

From output_port         |   To input_port          |  Behavior
------------------------ |   ----------------------- |  --------
`mutable_buffer<T>`      |   `mutable_buffer<T>`     |  Move (single output) or copy (fan-out)
`mutable_buffer<T>`      |   `immutable_buffer<T>`   |  Promote to immutable (move)
`immutable_buffer<T>`    |   `immutable_buffer<T>`   |  Share (zero-copy via `shared_ptr`)
`immutable_buffer<T>`    |   `mutable_buffer<T>`     |  Deep-copy to new mutable buffer

**Fan-out optimization** (mutable output to multiple inputs):
- Copy for all but the last destination
- Move to the final destination for efficiency

#### Statistics

Output ports track the following metrics (accessible via `stats()`):
- `packets_transferred`: Number of packets successfully sent
- `bytes_transferred`: Total bytes transmitted
- `last_activity_ns`: Timestamp of last send operation
- `throughput_mbps()`: Calculated throughput in megabits per second

Use `reset_stats()` to clear all statistics counters.

#### Backpressure

- `can_send()`: Returns `true` if at least one connected input port has available capacity
- Allows producers to check downstream capacity before generating new data

#### Connection Management

Output ports support dynamic runtime reconfiguration of connections:

- `disconnect(input_port_base* port)`: Disconnect from a specific input port
  - Returns `true` if the port was connected and is now disconnected
  - Returns `false` if the port was not connected
- `disconnect()`: Disconnect from all connected input ports
  - Returns the number of ports that were disconnected
- `is_connected()`: Check if connected to any input port
- `is_connected_to(const input_port_base* port)`: Check if connected to a specific input port
- `connection_count()`: Get the number of currently connected input ports
- `connected_ports()`: Get a list of connected port names for introspection

**Example:**
```cpp
// Disconnect a specific connection
bool was_connected = output.disconnect(&input1);

// Disconnect all connections
std::size_t count = output.disconnect();
logger()->info("Disconnected {} connections", count);

// Query connection state
if (output.is_connected_to(&input2)) {
    logger()->info("Still connected to input2");
}
```

**Thread Safety:** All connection management operations are thread-safe and can be called while data is being transmitted.

#### Metadata Transmission

- An `output_port` can send metadata to all its connected `input_port` instances using the `send_metadata(const metadata&)` function.
  - Updated metadata must be sent before the next data packet so that it can be associated correctly
- This metadata is "latched" by the receiving input ports and is intended to be associated with the *next* data packet that is
subsequently enqueued and retrieved from those input ports.

### Input Port

The `input_port` class provides a thread-safe queue to receive time-stamped data buffers from an `output_port`.
It can be configured with a depth limit and exposes methods for inspection, backpressure management, and statistics tracking.

#### Key Features

- Compatible with both `mutable_buffer<T>` and `immutable_buffer<T>`
- Thread-safe receive queue with condition variable
- Optional bounded queue depth (default: unbounded, i.e., `std::numeric_limits<std::size_t>::max()`)
- **Multiple receive variants**: Default timeout (1s), custom timeout, or blocking indefinitely
- **Statistics tracking**: Monitors packets transferred/dropped, queue depth, and throughput
- **Backpressure API**: Query queue state with `is_full()` and `available_capacity()`
- **Overflow callbacks**: Get notified when packets are dropped due to full queue
- Methods to clear and inspect the current queue state

#### Lifecycle & Behavior

- Data, along with its timestamp, is enqueued by an `output_port`'s `send_data()` method. If metadata was previously sent by the output port, that
  metadata is packaged with this incoming data during the internal `add_data` call.
- The internal queue honors the configured `depth` limit; data arriving when the queue is full (i.e., `m_queue.size() >= m_depth` when `add_data` is called) is dropped.
  - The queue depth can be configured dynamically at runtime with the `input_port`'s `depth(std::size_t)` method.
  - Setting a depth of 0 "disables" the port because all incoming data will be dropped.
  - Dropped packets increment the `packets_dropped` counter and trigger the overflow callback if set.

#### Receiving Data

Three variants are available for retrieving data:

1. **Default timeout** (1 second):
   ```cpp
   auto [buffer, ts, metadata] = input_port.get_data();
   ```

2. **Custom timeout**:
   ```cpp
   auto [buffer, ts, metadata] = input_port.get_data(std::chrono::milliseconds(500));
   ```

3. **Blocking** (waits indefinitely):
   ```cpp
   auto [buffer, ts, metadata] = input_port.get_data(composite::blocking);
   ```

Each method returns a `std::tuple<buffer_type, timestamp, std::optional<metadata>>`:
- If data is available, the tuple contains the data, its timestamp, and any metadata that was associated with it.
- If no data is received within the timeout (for non-blocking variants), an empty tuple is returned (buffer size will be 0).

#### Statistics

Input ports track the following metrics (accessible via `stats()`):
- `packets_transferred`: Number of packets successfully received by consumers
- `packets_dropped`: Number of packets dropped due to queue overflow
- `bytes_transferred`: Total bytes received by consumers
- `max_queue_depth`: High-water mark of queue depth
- `last_activity_ns`: Timestamp of last receive operation
- `throughput_mbps()`: Calculated throughput in megabits per second
- `drop_rate()`: Ratio of dropped packets (0.0 to 1.0)

Use `reset_stats()` to clear all statistics counters.

#### Backpressure

- `is_full()`: Returns `true` when queue depth equals configured limit
- `available_capacity()`: Returns number of packets that can be queued before overflow
- Allows upstream producers to implement flow control

#### Overflow Handling

Set a callback to be notified when packets are dropped:
```cpp
input_port.set_overflow_callback([](std::size_t dropped_count) {
    // Handle overflow condition (e.g., log warning, alert monitoring system)
});
```

The callback is invoked each time packets are dropped, with the count of dropped packets.

#### Metadata Association

- Metadata sent by an `output_port` is received by the `input_port` and stored in its internal `m_metadata` member. This is the "latching" mechanism.
- When the next data packet is enqueued into the `input_port`, this latched `m_metadata` is bundled with that data packet and timestamp into a tuple,
  which is then added to the queue.
- Immediately after the latched m_metadata is used to form this tuple, the `input_port`'s internal m_metadata member is reset.
  This makes the input port ready to latch new metadata for any subsequent data packets.

### Transport System

The **composite** framework provides a transport abstraction layer that allows output ports to publish data to external messaging systems.
Transports enable components to send data outside the application to distributed systems, monitoring tools, or remote consumers.

#### Transport Types

The framework supports multiple transport backends:

- **NATS**: Publish-subscribe messaging via NATS server (requires `-DCOMPOSITE_USE_NATS=ON`)
  - Lightweight, high-performance messaging
  - Subject-based routing and wildcards
  - Quality of service guarantees

Future transport types are planned.

#### Attaching Transports to Output Ports

Transports are attached to output ports at runtime, allowing components to send data to both connected input ports and external systems simultaneously:

```cpp
#include <composite/transports/nats/transport.hpp>

// In component initialization or via configuration
auto transport = std::make_unique<composite::nats::transport>(
    "nats://localhost:4222",  // NATS server URL
    "sensor.temperature"       // Subject/topic to publish to
);

// Attach to output port
m_output_port.attach_transport(std::move(transport));
```

**Key Features:**
- **Parallel Operation**: Transports operate alongside port-to-port connections without interference
- **Statistics Tracking**: Each transport tracks packets sent, bytes transferred, failures, and throughput
- **Error Handling**: Send failures are counted but don't block internal port connections
- **Multiple Transports**: A single output port can have multiple transports attached

#### Transport Interface

All transports implement a common interface (`transport_base`):

- `send(data, timestamp)`: Publish data buffer with timestamp to the transport
- `is_connected()`: Check if transport is connected to its backend
- `type()`: Get transport type enum (e.g., `transport_type::nats`)
- `endpoint()`: Get human-readable endpoint description
- **Statistics**: `packets_sent()`, `bytes_sent()`, `send_failures()`, `throughput_mbps()`
- `reset_stats()`: Clear statistics counters

#### NATS Transport

When built with NATS support, the NATS transport provides:

- Automatic reconnection on connection failure
- Subject-based pub/sub with wildcard support
- Efficient binary data transmission
- Configurable connection options (timeouts, retries, etc.)

**Example Usage:**
```cpp
// Create NATS transport
auto nats_transport = std::make_unique<composite::nats::transport>(
    "nats://nats.example.com:4222",
    "telemetry.sensor.data"
);

// Attach to output port
output_port.attach_transport(std::move(nats_transport));

// Data sent via send_data() is now published to both:
// 1. Connected input ports (within application)
// 2. NATS subject "telemetry.sensor.data" (external consumers)
```

## Properties and Configuration

Components in the **composite** framework are configurable through a property system managed by the `property_set` class. This allows for flexible
adaptation of component behavior at initialization or, for certain properties, during runtime.

### Defining Properties

Properties are typically defined in a component's constructor by linking them to member variables. This is done using the `add_property()` method
provided by the `component` base class:

```cpp
#include <composite/composite.hpp>
#include <optional>
#include <string>

class MyConfigurableComponent : public composite::component {
public:
    explicit MyConfigurableComponent(std::string_view id) : composite::component(id) {
        // Define a mandatory integer property with units and runtime configurability
        add_property("threshold", m_threshold)
            .units("dB")
            .configurability(composite::properties::config_type::RUNTIME);

        // Define an optional string property (m_api_key is std::optional<std::string>)
        // Default configurability is INITIALIZE, no units specified
        add_property("api_key", m_api_key);

        // Define a property that can only be set at initialization (default behavior)
        add_property("buffer_size", m_buffer_size).units("elements");
    }

    // ... process() and other methods ...

private:
    // Member variables for properties
    int32_t m_threshold{};
    std::optional<std::string> m_api_key{}; // Initially no value
    uint32_t m_buffer_size{1024};
};
```

#### Key aspects of property definition:

- **Type System**: The system automatically deduces the property type from the member variable's C++ type
  (e.g., `int` becomes `"int32"`, `float` becomes `"float"`, `std::string` becomes `"string"`).
  - `std::optional<T>` is supported for properties that may not always have a value. Its type will be represented as `"<type>?"`
    (e.g., `std::optional<int>` corresponds to type string `"int32?"`).
- **Fluent Configuration**: `add_property()` returns a reference that allows for chained calls to set metadata:
  - `.units(std::string_view)`: Specifies units for the property (e.g., "ms", "items", "percent"). This is for informational purposes.
  - `.configurability(composite::properties::config_type)`: Defines when the property can be changed:
    - `composite::properties::config_type::INITIALIZE` (default): The property can only be set during initialization configuration of values from JSON file.
    - `composite::properties::config_type::RUNTIME`: The property can be modified while the component is running.
- **Reference Binding**: Properties are registered by passing a reference to the component's member variable.
  The `property_set` directly reads and modifies this memory location when getting or setting property values.

### List Properties

For properties that represent collections of values, simply use `add_property()` with a `std::vector<T>`. List properties support indexing, appending, and clearing operations.

```cpp
#include <composite/composite.hpp>
#include <vector>

class MyComponentWithList : public composite::component {
public:
    explicit MyComponentWithList(std::string_view id) : composite::component(id) {
        // Define a list property - just pass the vector member
        add_property("thresholds", m_thresholds)
            .configurability(composite::properties::config_type::RUNTIME);

        // Add an indexed change listener for validation
        add_property_change_listener("thresholds",
            [this](std::size_t index) -> bool {
                // Validate the value at the specified index
                if (m_thresholds[index] < 0.0f || m_thresholds[index] > 100.0f) {
                    logger()->warn("Threshold at index {} is out of range", index);
                    return false; // Reject change
                }
                return true; // Accept change
            });
    }
    // ... other methods and members ...
private:
    std::vector<float> m_thresholds{};
};
```

**Accessing List Items in JSON:**
- Replace entire list: `"thresholds": ["10.5", "20.0", "30.5"]`
- Modify specific item: `"thresholds[0]": "15.0"`
- Append new item: `"thresholds[]": "40.0"`

### Structured Properties

For more complex configurations, properties can be grouped into structures. This allows for namespaced properties
(e.g., `"network.host"`, `"network.port"`) and better organization.

To use a struct as a property, you must specialize `composite::properties::property_traits<T>` for your struct type:

```cpp
#include <composite/composite.hpp>
#include <string>

struct NetworkConfig {
    std::string host{"localhost"};
    uint16_t port{8080};
    std::optional<std::string> protocol{};
};

// Specialize property_traits to register struct fields
template<>
struct composite::properties::property_traits<NetworkConfig> {
    static constexpr std::string_view type_name = "network_config";
    static void register_fields(composite::properties::property_set& ps, NetworkConfig& cfg) {
        using composite::properties::config_type;
        ps.add("host", cfg.host, config_type::RUNTIME);
        ps.add("port", cfg.port);  // Default: INITIALIZE
        ps.add("protocol", cfg.protocol);  // Optional property
    }
};

class MyComponentWithStructProp : public composite::component {
public:
    explicit MyComponentWithStructProp(std::string_view id) : composite::component(id) {
        // Simply add the struct as a property - fields are registered via property_traits
        add_property("network", m_net_config, composite::properties::config_type::RUNTIME);
    }
    // ... other methods and members ...
private:
    NetworkConfig m_net_config;
};
```

**Accessing Struct Fields in JSON:**
- Set individual field: `"network.host": "192.168.1.1"`
- Set multiple fields: `"network": {"host": "192.168.1.1", "port": "9000"}`

### List-of-Structs Properties

For advanced use cases, you can combine lists and structures. This creates a vector of structured objects.
Like struct properties, you must specialize `property_traits` for the element type:

```cpp
#include <composite/composite.hpp>
#include <vector>

struct Connection {
    std::string host{"localhost"};
    uint16_t port{8080};
};

// Specialize property_traits for the struct type
template<>
struct composite::properties::property_traits<Connection> {
    static constexpr std::string_view type_name = "connection";
    static void register_fields(composite::properties::property_set& ps, Connection& conn) {
        using composite::properties::config_type;
        ps.add("host", conn.host, config_type::RUNTIME);
        ps.add("port", conn.port, config_type::RUNTIME);
    }
};

class MyComponentWithStructList : public composite::component {
public:
    explicit MyComponentWithStructList(std::string_view id) : composite::component(id) {
        // Add the vector as a property - element fields are registered via property_traits
        add_property("connections", m_connections, composite::properties::config_type::RUNTIME);

        // Indexed change listener receives the index of modified/added item
        add_property_change_listener("connections",
            [this](std::size_t index) -> bool {
                auto& conn = m_connections[index];
                if (conn.port == 0) {
                    logger()->error("Invalid port 0 at index {}", index);
                    return false;
                }
                logger()->info("Connection {} validated: {}:{}", index, conn.host, conn.port);
                return true;
            });
    }
    // ... other methods and members ...
private:
    std::vector<Connection> m_connections{};
};
```

**Accessing List-of-Structs in JSON:**
- Replace entire list: `"connections": [{"host": "server1", "port": "8080"}, {"host": "server2", "port": "8081"}]`
- Append new struct: `"connections[]": {"host": "server3", "port": "8082"}`
- Modify field in item: `"connections[0].host": "new-server1"`

### Setting and Retrieving Property Values

While properties are defined within the component, their values are typically set externally (e.g., from a configuration file or via REST APIs).
The `component` base class provides a `set_properties()` method that accepts a list of string-based key-value pairs. This method handles:

- Resolving property names, including structured paths like `"network.port"`
- Performing type conversion from the input string to the target property's actual C++ type
- Validating changes against the property's configurability rules (INITIALIZE vs RUNTIME)
- Invoking registered change listeners (see below)

### Handling Property Changes

Components can react to changes in their properties in two main ways:

1. **Change Listeners**: A specific callback function can be attached to an individual property using the `property`'s `change_listener()` method.
   This callback is invoked by `set_property` *before* the property is finalized but *after* the member variable has been tentatively
   updated. If the callback returns `false`, the change is rejected, and the property value is reverted to its previous state.
   For struct and list properties, parent/list listeners also fire when nested fields change (e.g., `"network.host"` or `"connections[0].host"`).

   ```cpp
   // Use the change_listener() method to add a callback
   // Assume m_threshold is an int32_t member variable
   add_property("threshold", m_threshold)
       .units("percentage")
       .configurability(composite::properties::config_type::RUNTIME)
       .change_listener([this]() {
           // Inside the listener, m_threshold already holds the new, proposed value
           if (m_threshold < 0 || m_threshold > 100) {
               logger()->warn("Proposed threshold {} is out of range [0, 100]. Change will be rejected.", m_threshold);
               // Returning false will cause property_set to revert m_threshold to its previous value
               return false; // Reject change
           }
           logger()->info("Threshold will be changed to: {}. Change accepted.", m_threshold);
           // Perform any immediate actions needed due to this specific change
           // For example: self->reconfigure_threshold_dependent_logic();
           return true; // Accept change
       });
   ```

   For **list properties**, change listeners receive the index of the modified/added/removed item. Struct list field updates also notify the list listener:
   ```cpp
   add_property_change_listener("thresholds",
       [this](std::size_t index) -> bool {
           // Validate the item at the specified index
           if (m_thresholds[index] < 0.0f) {
               return false; // Reject
           }
           logger()->info("Item {} updated to {}", index, m_thresholds[index]);
           return true; // Accept
       });
   ```

   **Contextual Change Listeners**: For more advanced use cases, listeners can receive a `change_type` parameter indicating what kind of change occurred:

   - `change_type::SET` - A new value was set (e.g., optional property set from `nullopt`, or list item appended)
   - `change_type::MODIFY` - An existing value was modified
   - `change_type::RESET` - Value was reset (e.g., optional set to `nullopt`, list item deleted, or scalar reset to default)

   ```cpp
   // Contextual listener for optional properties - know if value is being set, modified, or cleared
   add_property_change_listener("timeout",
       [this](composite::properties::change_type ctx) -> bool {
           using composite::properties::change_type;
           if (ctx == change_type::SET) {
               logger()->info("Timeout configured for the first time: {}", *m_timeout);
           } else if (ctx == change_type::MODIFY) {
               logger()->info("Timeout changed to: {}", *m_timeout);
           } else if (ctx == change_type::RESET) {
               logger()->info("Timeout cleared, will use default behavior");
           }
           return true;
       });

   // Contextual indexed listener for lists - know if item was added, modified, or removed
   add_property_change_listener("connections",
       [this](std::size_t index, composite::properties::change_type ctx) -> bool {
           using composite::properties::change_type;
           if (ctx == change_type::SET) {
               logger()->info("New connection added at index {}", index);
           } else if (ctx == change_type::MODIFY) {
               logger()->info("Connection {} updated: host={}", index, m_connections[index].host);
           } else if (ctx == change_type::RESET) {
               logger()->info("Connection at index {} removed", index);
           }
           return true;
       });
   ```

   Contextual listeners are backward compatible - if you register a simple listener (without `change_type`), it will still work. The system falls back to the simple listener if no contextual listener is registered.

2. **`property_change_handler()`**: The `component` class provides a `virtual void property_change_handler()` method. This method is called
   once at the end of a successful `set_properties()` call, after all specified properties have been updated and their individual change listeners
   (if any) have approved the changes. Subclasses can override this method to perform more complex or coordinated reconfigurations based on the
   new overall state of multiple properties.

   ```cpp
   // In MyComponent class
   void property_change_handler() override {
       // This method is called after one or more properties have been successfully updated.
       logger()->info("Properties updated. Component will reconfigure based on new state.");
       // Example: if m_buffer_size or other related properties changed, reallocate buffers or update internal structures.
       // this->reinitialize_buffers_if_needed();
       // this->update_processing_parameters();
   }
   ```

### Runtime Property Control via REST API

The **composite-cli** application provides a REST API for runtime property inspection and modification. The server runs on `localhost:5000` by default
(configurable via `--server` and `--port` command-line arguments).

**Key Endpoints:**

- `GET /app` - Get full application state including all components and properties
- `GET /app/components/:id` - Get specific component state
- `PATCH /app/components/:id` - Update multiple properties atomically
- `GET /app/components/:id/properties` - List all properties for a component
- `GET /app/components/:id/properties/:name` - Get specific property value and metadata
- `PUT /app/components/:id/properties/:name` - Update a scalar property
- `DELETE /app/components/:id/properties/:name` - Reset property to default (null)

**List Property Operations:**

- `GET /app/components/:id/properties/:name/items` - Get all list items
- `GET /app/components/:id/properties/:name/items/:index` - Get specific list item
- `POST /app/components/:id/properties/:name/items` - Append new item to list
- `PUT /app/components/:id/properties/:name/items/:index` - Update specific list item
- `DELETE /app/components/:id/properties/:name/items/:index` - Remove list item

**Struct Property Operations:**

- `GET /app/components/:id/properties/:name/fields` - Get all struct fields
- `GET /app/components/:id/properties/:name/fields/:field` - Get specific field value
- `PATCH /app/components/:id/properties/:name/fields/:field` - Update specific field

**Port Connection Operations:**

- `GET /app/components/:id/ports` - List all ports for a component with connection status
  - Returns port name, type (input/output), connection state, count, and connected port names
- `GET /app/components/:id/ports/:port_name` - Get detailed information for a specific port
- `POST /app/connections` - Create a new connection between component ports
  - Request body: `{"output": {"component": "source_id", "port": "port_name"}, "input": {"component": "target_id", "port": "port_name"}}`
  - Returns 201 Created with connection details on success
- `DELETE /app/components/:id/ports/:port_name/connections` - Disconnect all connections from a port
  - Returns the number of connections that were disconnected
- `DELETE /app/connections` - Disconnect a specific connection between two ports
  - Request body: `{"output": {"component": "source_id", "port": "port_name"}, "input": {"component": "target_id", "port": "port_name"}}`
  - Mirrors the POST format for symmetric create/delete operations
  - Returns 200 OK with connection details on success

**Example REST API Usage:**

```bash
# Get all properties for a component
curl http://localhost:5000/app/components/my_component/properties

# Update a single scalar property
curl -X PUT http://localhost:5000/app/components/my_component/properties/threshold \
  -H "Content-Type: application/json" \
  -d '{"value": "75.5"}'

# Update multiple properties atomically (single lock, single property_change_handler call)
curl -X PATCH http://localhost:5000/app/components/my_component \
  -H "Content-Type: application/json" \
  -d '{"properties": {"threshold": "75.5", "enabled": "true"}}'

# Replace entire list property
curl -X PATCH http://localhost:5000/app/components/my_component \
  -H "Content-Type: application/json" \
  -d '{"properties": {"thresholds": ["10.0", "20.0", "30.0"]}}'

# Append item to list property
curl -X POST http://localhost:5000/app/components/my_component/properties/thresholds/items \
  -H "Content-Type: application/json" \
  -d '{"value": "40.0"}'

# Update struct field
curl -X PATCH http://localhost:5000/app/components/my_component/properties/network/fields/host \
  -H "Content-Type: application/json" \
  -d '{"value": "192.168.1.100"}'

# Batch update list-of-structs (clears list first, then adds both items)
curl -X PATCH http://localhost:5000/app/components/my_component \
  -H "Content-Type: application/json" \
  -d '{"properties": {"connections": [
    {"host": "server1", "port": "8080"},
    {"host": "server2", "port": "8081"}
  ]}}'

# Get all ports for a component with connection status
curl http://localhost:5000/app/components/source/ports

# Get detailed information for a specific port
curl http://localhost:5000/app/components/source/ports/data_out

# Create a new connection between components
curl -X POST http://localhost:5000/app/connections \
  -H "Content-Type: application/json" \
  -d '{"output": {"component": "source", "port": "data_out"}, "input": {"component": "sink", "port": "data_in"}}'

# Disconnect all connections from a port
curl -X DELETE http://localhost:5000/app/components/source/ports/data_out/connections

# Disconnect a specific connection (mirrors POST format)
curl -X DELETE http://localhost:5000/app/connections \
  -H "Content-Type: application/json" \
  -d '{"output": {"component": "source", "port": "data_out"}, "input": {"component": "sink", "port": "data_in"}}'
```

**Notes on REST API Behavior:**

- Only properties marked with `config_type::RUNTIME` can be modified via REST API
- PATCH requests are **atomic with rollback** - all properties updated under a single mutex lock:
  - If any property update fails (validation error, type conversion error, or change listener rejection), all previously applied changes in that batch are automatically rolled back
  - Either all properties are updated successfully, or none are modified
  - This ensures consistent state even when updating multiple related properties
- `property_change_handler()` is called once after all property changes validated and applied
- For list properties, each individual POST creates a separate update (multiple handler calls)
- PATCH with array replaces entire list (clears first, then adds items) - single handler call
- **List deletions in batch updates**: Removing list items (setting indexed items to null) is not supported within multi-property batch updates due to rollback complexity. Use single-property DELETE requests instead.
- Struct-list item updates via `PUT /items/:index` are applied atomically as a single change (one list listener notification).
- Error responses include detailed information about validation failures

## Implementing a Component

To create a new component, developers must implement the required interface functions, ensuring
compatibility with the **composite** framework. Example:

```cpp
#include <composite/composite.hpp>

class MyComponent : public composite::component {
public:
    explicit MyComponent(std::string_view id) : composite::component(id) {
        // Add ports to port set
        add_port(&m_in_port);
        add_port(&m_out_port);

        // Add properties to configure
        add_property("processing_gain", m_processing_gain)
            .units("factor")
            .configurability(composite::properties::config_type::RUNTIME)
            .change_listener([this]() {
                logger()->info("Change listener validating new processing_gain value: {}", m_processing_gain);
                // Add validation logic as needed
                // ...
                // return false; // reject change if invalid value
                return true; // accept change
            });
    }

    ~MyComponent() final = default;

    // Implement the pure virtual function defined in composite::component
    auto process() -> composite::retval override {
        using enum composite::retval;

        // Get data from an input port (if available)
        // get_data() returns a tuple: {data_buffer, timestamp, optional_metadata}
        auto [buffer, ts, metadata] = m_in_port.get_data();
        if (buffer.size() == 0) {
            // No data received within the timeout
            return NOOP; // Indicate no operation was performed, component will sleep briefly
        }

        // Check if metadata was received with this data packet
        if (metadata.has_value()) {
            // Printing metadata for debug purposes
            logger()->debug("Received metadata with data packet: {}", metadata->to_string());

            // Process metadata as needed
            // ...

            // Send metadata downstream for follow-on components
            // Any updated metadata must be sent before the next data packet is sent
            m_out_port.send_metadata(metadata.value());
        }

        // User-defined processing logic
        // Example: Apply gain (actual processing depends on data content)
        logger()->debug("Processing data (size: {}) with gain: {}", buffer.size(), m_processing_gain);

        // This example modifies the data in-place using mutable_buffer
        for (std::size_t i = 0; i < buffer.size(); ++i) {
            buffer[i] *= m_processing_gain;
        }

        // Send data via an output port (buffer is moved)
        m_out_port.send_data(std::move(buffer), ts);

        return NORMAL; // indicate normal processing occurred, component will yield
    }

    auto property_change_handler() -> void override {
        logger()->info("Properties have been updated. Current gain: {}", m_processing_gain);
        // Potentially reconfigure aspects of the component based on new property values
    }

private:
    // Ports using mutable buffers for in-place processing
    composite::input_port<composite::mutable_buffer<float>> m_in_port{"data_in"};
    composite::output_port<composite::mutable_buffer<float>> m_out_port{"data_out"};

    // Properties
    float m_processing_gain{1.0f}; // example property with a default value

}; // class MyComponent

// Export the factory function for dynamic loading
extern "C" {
    auto create(std::string_view id) -> std::shared_ptr<composite::component> {
        return std::make_shared<MyComponent>(id);
    }
}
```

**Factory Function Requirements:**
- Must be declared `extern "C"` to prevent C++ name mangling
- Must be named `create`
- Must accept `std::string_view id` as the first parameter
- For components that need additional configuration, add a second `std::string_view arg` parameter
- Must return `std::shared_ptr<composite::component>`
