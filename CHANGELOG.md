# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
