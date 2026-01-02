# Duktape

Duktape JavaScript engine for Erlang.

This library embeds the [Duktape](https://duktape.org/) JavaScript engine (v2.7.0) as an Erlang NIF, allowing you to evaluate JavaScript code directly from Erlang.

## Features

- Execute JavaScript code from Erlang
- Bidirectional type conversion between Erlang and JavaScript
- Multiple isolated JavaScript contexts
- CommonJS module support
- Thread-safe with automatic resource cleanup
- No external dependencies - Duktape is embedded

## Requirements

- Erlang/OTP 24 or later
- CMake 3.10 or later
- C compiler (gcc, clang, or MSVC)

## Installation

Add to your `rebar.config`:

```erlang
{deps, [
    {duktape, {git, "https://github.com/benoitc/duktape-erlang.git", {branch, "main"}}}
]}.
```

Then run:

```bash
rebar3 compile
```

## Quick Start

```erlang
%% Create a JavaScript context
{ok, Ctx} = duktape:new_context().

%% Evaluate JavaScript code
{ok, 42} = duktape:eval(Ctx, <<"21 * 2">>).
{ok, <<"hello">>} = duktape:eval(Ctx, <<"'hello'">>).

%% Evaluate with variable bindings
{ok, 30} = duktape:eval(Ctx, <<"x * y">>, #{x => 5, y => 6}).

%% Define and call functions
{ok, _} = duktape:eval(Ctx, <<"function add(a, b) { return a + b; }">>).
{ok, 7} = duktape:call(Ctx, add, [3, 4]).

%% CommonJS modules
ok = duktape:register_module(Ctx, <<"utils">>, <<"
    exports.greet = function(name) {
        return 'Hello, ' + name + '!';
    };
">>).
{ok, <<"Hello, World!">>} = duktape:eval(Ctx, <<"require('utils').greet('World')">>).
```

## API Reference

### Context Management

#### `new_context() -> {ok, context()} | {error, term()}`

Create a new JavaScript context. Contexts are isolated - variables and functions defined in one context are not visible in others.

Contexts are automatically cleaned up when garbage collected, but you can also explicitly destroy them with `destroy_context/1`.

#### `destroy_context(Ctx) -> ok | {error, term()}`

Explicitly destroy a JavaScript context. This is optional - contexts are automatically cleaned up on garbage collection. Calling destroy on an already-destroyed context is safe (idempotent).

### Evaluation

#### `eval(Ctx, Code) -> {ok, Value} | {error, term()}`

Evaluate JavaScript code and return the result of the last expression.

```erlang
{ok, 3} = duktape:eval(Ctx, <<"1 + 2">>).
{ok, <<"hello">>} = duktape:eval(Ctx, <<"'hello'">>).
{error, {js_error, _}} = duktape:eval(Ctx, <<"throw 'oops'">>).
```

#### `eval(Ctx, Code, Bindings) -> {ok, Value} | {error, term()}`

Evaluate JavaScript code with variable bindings. Bindings are set as global variables before evaluation.

```erlang
{ok, 30} = duktape:eval(Ctx, <<"x * y">>, #{x => 5, y => 6}).
{ok, <<"hello world">>} = duktape:eval(Ctx, <<"greeting + ' ' + name">>,
                                       #{greeting => <<"hello">>, name => <<"world">>}).
```

### Function Calls

#### `call(Ctx, FunctionName) -> {ok, Value} | {error, term()}`

Call a global JavaScript function with no arguments.

```erlang
{ok, _} = duktape:eval(Ctx, <<"function getTime() { return Date.now(); }">>).
{ok, Timestamp} = duktape:call(Ctx, <<"getTime">>).
```

#### `call(Ctx, FunctionName, Args) -> {ok, Value} | {error, term()}`

Call a global JavaScript function with arguments. Function names can be binaries or atoms.

```erlang
{ok, _} = duktape:eval(Ctx, <<"function add(a, b) { return a + b; }">>).
{ok, 7} = duktape:call(Ctx, <<"add">>, [3, 4]).
{ok, 7} = duktape:call(Ctx, add, [3, 4]).
```

### CommonJS Modules

#### `register_module(Ctx, ModuleId, Source) -> ok | {error, term()}`

Register a CommonJS module with source code. The module can then be loaded with `require/2` or via `require()` in JavaScript.

```erlang
ok = duktape:register_module(Ctx, <<"math">>, <<"
    exports.add = function(a, b) { return a + b; };
    exports.multiply = function(a, b) { return a * b; };
">>).
```

#### `require(Ctx, ModuleId) -> {ok, Exports} | {error, term()}`

Load a CommonJS module and return its exports. Modules are cached - subsequent requires return the same exports object.

```erlang
{ok, Exports} = duktape:require(Ctx, <<"math">>).
```

### Utility

#### `info() -> {ok, string()}`

Get NIF information. Used to verify the NIF is loaded correctly.

## Type Conversions

### Erlang to JavaScript

| Erlang | JavaScript |
|--------|------------|
| `integer()` | number |
| `float()` | number |
| `binary()` | string |
| `true` | true |
| `false` | false |
| `null` | null |
| `undefined` | undefined |
| other atoms | string |
| `list()` | array (or string if iolist) |
| `map()` | object |
| `tuple()` | array |

### JavaScript to Erlang

| JavaScript | Erlang |
|------------|--------|
| number (integer) | `integer()` |
| number (float) | `float()` |
| NaN | `nan` (atom) |
| Infinity | `infinity` (atom) |
| -Infinity | `neg_infinity` (atom) |
| string | `binary()` |
| true | `true` |
| false | `false` |
| null | `null` |
| undefined | `undefined` |
| array | `list()` |
| object | `map()` |

## Error Handling

JavaScript errors are returned as `{error, {js_error, Message}}` where `Message` is a binary containing the error message and stack trace.

```erlang
{error, {js_error, <<"ReferenceError: x is not defined", _/binary>>}} =
    duktape:eval(Ctx, <<"x + 1">>).
```

Contexts remain usable after errors - you can continue to evaluate code in the same context.

## Thread Safety

All context operations are thread-safe. Multiple Erlang processes can share a context, though operations are serialized via a mutex. For maximum parallelism, create separate contexts for concurrent workloads.

## Resource Management

Contexts are managed as Erlang NIF resources with automatic cleanup:

- Contexts are garbage collected when no Erlang process holds a reference
- Multiple processes can share a context safely
- Explicit `destroy_context/1` is optional but can be used for immediate cleanup
- Reference counting ensures contexts are not destroyed while in use

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

Duktape is also licensed under the MIT License - see [c_src/duktape/LICENSE.txt](c_src/duktape/LICENSE.txt).
