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
Usage: composite-cli [--help] [--version] --config-file VAR [--server VAR] [--port VAR] [--log-level VAR]

Optional arguments:
  -h, --help         shows help message and exits 
  -v, --version      prints version information and exits 
  -f, --config-file  application configuration file [required]
  -s, --server       REST server address [nargs=0..1] [default: "localhost"]
  -p, --port         REST server port [nargs=0..1] [default: 5000]
  -l, --log-level    log level [trace, debug, info, warning, error, critical, off] [nargs=0..1] [default: "info"]
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
    - **name (required, string):** The name of the shared object to be loaded (e.g., `"name": "my_component"` will attempt to load `libmy_component.so`).
    - **id (optional, string):** When defining two components of the same name, a unique identifier must be provided with this field
      to differentiate them. By default, the `id` is assigned the `name`. This identifier must also be used when making connections (see below).
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
            "name": "my_component",
            "properties": {
                "specific_param": "123",
                "processing_gain": "2.5"
            }
        }
    ]
}
```

4. **connections (required, array of objects):** An array where each object defines a connection between an output
port of one component and an input port of another component (or a NATS subject). Each connection object must contain:
    - **output (required, object):** Specifies the source of the data. Must contain keys identifying the output component and
      its port (e.g., "component": "component_A_id", "port": "data_out").
    - **input (required, object):** Specifies the destination of the data. Must contain keys identifying the input component
      and its port (e.g., "component": "component_B_id", "port": "data_in").
        - For NATS output, the object keys become `"nats"` and `"subject"` for defining the NATS URI and subject to publish data to.

```json
{
    "name": "my_streaming_application",
    "properties": {
        ...
    },
    "components": [
        ...
    ],
    "connections": [
        {
            "output": { "component": "source_component", "port": "output_data" },
            "input": { "component": "processing_component", "port": "input_data" }
        },
        {
            "output": { "component": "processing_component", "port": "processed_output" },
            "input": { "nats": "nats://my_nats_server/some/subject", "subject": "nats_subject" }
        }
    ]
}
```

## Component Interface

The **composite** framework is designed around a component-based architecture.
Each component follows a well-defined interface that allows it to be integrated into a larger streaming pipeline.
The key aspects of the component interface include:

- **Lifecycle Management**: Each component follows a structured lifecycle, including creation, execution, and teardown.
- **Configuration**: Components can be configured via properties, allowing for flexible runtime behavior.
- **Initialization**: Components define an initialization phase where necessary resources are allocated.
- **Data Processing**: Components process incoming data and produce outputs, which are streamed to downstream components.

### Ports

The **composite** framework provides a type-safe, smart-pointer-based port system for connecting components.
The system facilitates the transfer of time-stamped contiguous data buffers and associated metadata between components.
It handles smart pointer ownership semantics (`std::unique_ptr` and `std::shared_ptr`) to optimize memory use and performance.

Each port is either an `input_port<T>` or an `output_port<T>`, where `T` is a smart pointer to a contiguous buffer type
such that `T::element_type` satisfies `std::ranges::contiguous_range` (e.g., `std::unique_ptr<std::vector<float>>`).

### Output Port

The `output_port` class is responsible for publishing time-stamped buffer data to one or more connected `input_port<T>`
instances or to a NATS subject if configured. It supports efficient transfer semantics by minimizing copies and
adjusting behavior based on the pointer types of connected inputs.

#### Key Features

- Compatible with `std::unique_ptr` and `std::shared_ptr`
- Ability to send metadata independently to connected input ports via `send_metadata()`
- Sorts connected inputs internally to handle ownership safety and optimize move semantics
- Forwards data intelligently by choosing to move, copy, or promote based on smart pointer types
- Respects range and type constraints for safety and flexibility
- Optionally publishes raw byte buffers over NATS

#### Data Transfer Semantics

Behavior is determined by the smart pointer types of the output and input ports. Connected ports are internally sorted
such that `unique_ptr` destinations are processed last to enable efficient move semantics for the final `unique_ptr` recipient.

From output_port<T>  |   To input_port<T>  |  Behavior
-------------------  |   ----------------- |  --------
unique_ptr<T>        |   unique_ptr<T>     |  Move (last), Deep-copy (others)
unique_ptr<T>        |   shared_ptr<T>     |  Promote to shared_ptr once (by copy or release), reuse shared_ptr
shared_ptr<T>        |   shared_ptr<T>     |  Share reference (no copy)
shared_ptr<T>        |   unique_ptr<T>     |  Deep-copy buffer into unique_ptr

#### Metadata Transmission

- An `output_port` can send metadata to all its connected `input_port` instances using the `send_metadata(const metadata&)` function.
  - Updated metadata must be sent before the next data packet so that it can be associated correctly
- This metadata is "latched" by the receiving input ports and is intended to be associated with the *next* data packet that is
subsequently enqueued and retrieved from those input ports.

### Input Port

The `input_port` class provides a thread-safe queue to receive time-stamped data buffers from an `output_port`.
It can be configured with a depth limit and exposes methods for inspection and clearing.

#### Key Features

- Compatible with `std::unique_ptr` and `std::shared_ptr`
- Thread-safe receive queue with condition variable
- Optional bounded queue depth (default: unbounded, i.e., `std::numeric_limits<std::size_t>::max()`)
- Blocking `get_data()` retrieves a tuple containing the data buffer, its timestamp, and optional associated metadata, with 1-second timeout
- Methods to clear and inspect the current queue state

#### Lifecycle & Behavior

- Data, along with its timestamp, is enqueued by an `output_port`'s `send_data()` method. If metadata was previously sent by the output port, that
  metadata is packaged with this incoming data during the `add_data` call.
- The internal queue honors the configured `depth` limit; data arriving when the queue is full (i.e., `m_queue.size() >= m_depth when` `add_data` is called) is dropped.
  - The queue depth can be configured dynamically at runtime with the `input_port`'s `depth(std::size_t)` method.
  - Setting a depth of 0 "disables" the port because all incoming data will be dropped.
- Consumers call get_data() to retrieve a `std::tuple<buffer_type, timestamp, std::optional<metadata>>`.
  - If data is available, the tuple contains the data, its timestamp, and any metadata that was associated with it at the time of enqueuing.
  - If no data is received within the 1-second timeout, the `buffer_type` element of the tuple will be null (or equivalent, e.g. a `nullptr` for smart pointers),
    and the timestamp and metadata will be default/empty (as `get_data()` returns {}).

#### Metadata Association

- Metadata sent by an `output_port` is received by the `input_port` and stored in its internal `m_metadata` member. This is the "latching" mechanism.
- When the next data packet is enqueued into the `input_port`, this latched `m_metadata` is bundled with that data packet and timestamp into a tuple,
  which is then added to the queue.
- Immediately after the latched m_metadata is used to form this tuple, the `input_port`'s internal m_metadata member is reset.
  This makes the input port ready to latch new metadata for any subsequent data packets.

### Properties and Configuration

Components in the **composite** framework are configurable through a property system managed by the `property_set` class. This allows for flexible
adaptation of component behavior at initialization or, for certain properties, during runtime.

#### Defining Properties

Properties are typically defined with a component's constructor by linking them to member variable. This is done using the `add_property()` method
provided by the `component` base class:

```cpp
#include <composite/component.hpp>
#include <optional>
#include <string>

class MyConfigurableComponent : public composite::component {
public:
    MyConfigurableComponent() : composite::component("MyConfigurableComponent") {
        // Define a mandatory integer property with units and runtime configurability
        add_property("threshold", &m_threshold)
            .units("dB")
            .configurability(composite::properties::config_type::RUNTIME);

        // Define an optional string property (m_api_key is std::optional<std::string>)
        // Default configurability is INITIALIZE, no units specified
        add_property("api_key", &m_api_key);

        // Define a property that can only be set at initialization (default behavior)
        add_property("buffer_size", &m_buffer_size).units("elements");
    }

    // ... process() and other methods ...

private:
    // Member variables for properties
    int32_t m_threshold{};
    std::optional<std::string> m_api_key{}; // Initially no value
    uint32_t m_buffer_size{1024};
};
```

##### Key aspects of property definition:

- **Type System**: The system automatically deduces the property type from the member variable's C++ type
  (e.g., `int` becomes `"int32"`, `float` becomes `"float"`, `std::string` becomes `"string"`).
  - `std::optional<T>` is supported for properties that may not always have a value. Its type will be represented as `"<type>?"` 
    (e.g., `std::optional<int>` corresponds to type string `"int32?"`).
- **Fluent Configuration**: `add_property()` returns a reference that allows for chained calls to set metadata:
  - `.units(std::string_view)`: Specifies units for the property (e.g., "ms", "items", "percent"). This is for informational purposes.
  - `.configurability(composite::properties::config_type)`: Defines when the property can be changed:
    - `composite::properties::config_type::INITIALIZE` (default): The property can only be set during initialization configuration of values from JSON file.
    - `composite::properties::config_type::RUNTIME`: The property can be modified while the component is running.
- **Pointers**: Properties are registered by passing a pointer to the component's member variable that will store the actual value.
  The `property_set` directly manipulates this memory location.

#### Structured Properties

For more complex configurations, properties can be grouped into structures using `add_struct_property()`. This allows for namespaced properties
(e.g., `"network.host"`, `"network.port"`) and better organization.

```cpp
#include <composite/component.hpp>
#include <string>

struct NetworkConfig {
    std::string host{"localhost"};
    uint16_t port{8080};
    std::optional<std::string> protocol{};
};

class MyComponentWithStructProp : public composite::component {
public:
    MyComponentWithStructProp() : composite::component("MyComponentWithStructProp") {
        add_struct_property("network", &m_net_config,
            // This lambda registers the fields of the NetworkConfig struct
            [](auto& ps, auto* conf) {
                ps.add_property("host", &conf->host).configurability(composite::properties::config_type::RUNTIME);
                ps.add_property("port", &conf->port); // Default: INITIALIZE
                ps.add_property("protocol", &conf->protocol); // Optional property
            }
        );
    }
    // ... other methods and members ...
private:
    NetworkConfig m_net_config;
};
```

#### Setting and Retrieving Property Values

While properties are defined within the component, their values are typically set externally (e.g., from a configuration file or via REST APIs).
The `component` base class provides a `set_properties()` method that accepts a list of string-based key-value pairs. This method handles:

- Resolving property names, including structured paths like `"network.port"`
- Performing type conversion from the input string to the target property's actual C++ type
- Validating changes against the property's configurability rules (INITIALIZE vs RUNTIME)
- Invoking registered change listeners (see below)

#### Handling Property Changes

Components can react to changes in their properties in two main ways:

1. **Change Listeners**: A specific callback function can be attached to an individual property using the `property`'s `change_listener()` method.
   This callback is invoked by `set_property` *before* the property is finalized but *after* the pointed-to-member variable has been tenatively
   updated. If the callback returns `false`, the change is rejected, and the property value is reverted to its previous state.

   ```cpp
   // Use the change_listener() method to add a callback
   // Assume m_threshold is an int32_t member variable
   add_property("threshold", &m_threshold)
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

### Implementing a Component

To create a new component, developers must implement the required interface functions, ensuring
compatibility with the **composite** framework. Example:

```cpp
#include <composite/component.hpp>

class MyComponent : public composite::component {
    using input_t = std::unique_ptr<std::vector<float>>;
    using output_t = input_t;

public:
    MyComponent() : composite::component("MyComponent") {
        // Add ports to port set
        add_port(&m_in_port);
        add_port(&m_out_port);

        // Add properties to configure
        add_property("processing_gain", &m_processing_gain)
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
        auto [data, ts, metadata] = m_in_port.get_data();
        if (data == nullptr) {
            // No data received within the timeout
            return NOOP; // Indicate no operation was performed, component will sleep briefly
        }

        // Check is metadata was received with this data packet
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
        logger()->debug("Processing data (size: {}) with gain: {}", data->size(), m_processing_gain);
        // This example assumes the data processing modifies the data in-place
        for (auto& val : *data) {
            val *= m_processing_gain;
        }

        // Send data via an output port
        m_out_port.send_data(std::move(data), ts);

        return NORMAL; // indicate normal processing occurred, component will yield
    }

    auto property_change_handler() -> void override {
        logger()->info("Properties have been updated. Current gain: {}", m_processing_gain);
        // Potentially reconfigure aspects of the component based on new property values
    }

private:
    // Ports
    composite::input_port<input_t> m_in_port{"data_in"};
    composite::output_port<output_t> m_out_port{"data_out"};

    // Properties
    float m_processing_gain{1}; // example property with a default value

}; // class MyComponent
```
