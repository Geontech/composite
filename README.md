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
- OpenSSL (version 3.0 or higher) if compiling with  `-DCOMPOSITE_USE_OPENSSL=ON`

### Build and Install

```cmake
cmake -B build
cmake --build build [--parallel N]
cmake --install build
```

### Build Options

- `COMPOSITE_USE_NATS`: Enable components to publish data to a NATS server on a defined subject
- `COMPOSITE_USE_OPENSSL`: Compile with OpenSSL support to enable a secure REST server

## Component Interface

The **composite** framework is designed around a component-based architecture.
Each component follows a well-defined interface that allows it to be integrated into a larger
streaming pipeline.
The key aspects of the component interface include:

- **Configuration**: Components can be configured via properties, allowing for flexible runtime behavior.
- **Initialization**: Components define an initialization phase where necessary resources are allocated.
- **Data Processing**: Components process incoming data and produce outputs, which are streamed to downstream components.
- **Lifecycle Management**: Each component follows a structured lifecycle, including creation, execution, and teardown.

### Implementing a Component

To create a new component, developers must implement the required interface functions, ensuring
compatibility with the **composite** framework. Example:

```cpp
#include <composite/component.hpp>

class MyComponent : public composite::component {
    using input_port_t = std::shared_ptr<std::vector<std::byte>>;
    using output_port_t = input_port_t;

public:
    MyComponent() : composite::component("MyComponent") {
        // Add ports to port set
        add_port(&m_in_port);
        add_port(&m_out_port);

        // Add properties to configure
        add_property("property_name", &m_property_name);
    }

    ~MyComponent() final = default;

    // Implement the pure virtual function defined in composite::component
    auto process() -> composite::retval override {
        using enum composite::retval;
        
        // Get data from an input port (if available)
        auto [data, ts] = m_in_port->get_data();
        if (data == nullptr) {
            return NOOP;
        }

        // User-defined processing logic
        // ...

        // Send data via an output port
        m_out_port->send_data(std::move(data), ts);

        return NORMAL;
    }

private:
    // Ports
    input_port_t m_in_port{"data_in"};
    output_port_t m_out_port{"data_out"};

    // Properties
    uint32_t m_property_name{};

}; // class MyComponent
```
