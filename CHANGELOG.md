# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.2.0] - 2026-01-03

### Added

- **Erlang Function Registration**: Call Erlang functions synchronously from JavaScript
  - `register_function/3` - register Erlang funs or `{Module, Function}` tuples
  - Trampoline pattern enables nested calls (e.g., `double(double(5))`)
  - Functions stored per-process in process dictionary
  - Erlang exceptions converted to JavaScript errors

- **Event Framework**: Bidirectional communication between JavaScript and Erlang
  - `new_context/1` with `#{handler => pid()}` option
  - `send/3` - send data to JavaScript callbacks
  - `Erlang.emit(type, data)` - emit events from JavaScript
  - `Erlang.on/off(event, callback)` - register/unregister callbacks
  - `Erlang.log(level, ...)` - structured logging
  - `console.log/info/warn/error/debug` support

- New performance benchmarks for register_function and events
- Comprehensive test coverage (141 tests)

## [0.1.0] - 2025-01-03

### Added

- Initial release
- Embedded Duktape JavaScript engine v2.7.0
- JavaScript context management
  - `new_context/0` - create isolated JavaScript contexts
  - `destroy_context/1` - explicit context destruction (optional, GC handles cleanup)
- JavaScript evaluation
  - `eval/2` - evaluate JavaScript code
  - `eval/3` - evaluate with Erlang variable bindings
- Function calling
  - `call/2` - call global JavaScript functions
  - `call/3` - call with arguments
- CommonJS module support
  - `register_module/3` - register modules with source code
  - `require/2` - load and cache modules
- Bidirectional type conversion
  - Erlang → JavaScript: integers, floats, binaries, atoms, lists, maps, tuples
  - JavaScript → Erlang: numbers, strings, booleans, null, undefined, arrays, objects
  - Special values: NaN, Infinity, -Infinity returned as atoms
- Thread-safe context access with mutex protection
- Automatic resource cleanup via NIF reference counting
- CMake-based build system
- Comprehensive test suite (109 tests)
- Performance benchmarks
