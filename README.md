# Duktape

Duktape JavaScript engine for Erlang.

This library embeds the [Duktape](https://duktape.org/) JavaScript engine (v2.7.0) as an Erlang NIF, allowing you to evaluate JavaScript code directly from Erlang.

## Features

- Execute JavaScript code from Erlang
- Bidirectional type conversion between Erlang and JavaScript
- Multiple isolated JavaScript contexts
- CommonJS module support
- **Event framework for JS ↔ Erlang communication**
- **Register Erlang functions callable from JavaScript**
- **console.log/info/warn/error/debug support**
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
    {duktape, {git, "https://github.com/benoitc/erlang-duktape.git", {branch, "main"}}}
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

#### `new_context(Opts) -> {ok, context()} | {error, term()}`

Create a new JavaScript context with options.

Options:
- `handler => pid()`: Process to receive events from JavaScript. The handler will receive messages of the form `{duktape, Type, Data}` where Type is an atom (e.g., `log`) and Data is the event payload.

```erlang
{ok, Ctx} = duktape:new_context(#{handler => self()}),
{ok, _} = duktape:eval(Ctx, <<"console.log('hello')">>),
receive
    {duktape, log, #{level := info, message := <<"hello">>}} ->
        io:format("Got log message~n")
end.
```

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

### Event Framework

The event framework enables bidirectional communication between JavaScript and Erlang.

#### `send(Ctx, Event, Data) -> {ok, Value} | ok | {error, term()}`

Send data to a registered JavaScript callback. If JavaScript code has registered a callback using `Erlang.on(event, fn)`, this function will call that callback with the provided data.

Returns `{ok, Result}` where Result is the return value of the callback, or `ok` if no callback is registered for the event.

```erlang
{ok, Ctx} = duktape:new_context(),
%% JavaScript registers a callback
{ok, _} = duktape:eval(Ctx, <<"
    var received = null;
    Erlang.on('data', function(d) { received = d; return 'got it'; });
">>),
%% Erlang sends data to the callback
{ok, <<"got it">>} = duktape:send(Ctx, data, #{value => 42}),
{ok, #{<<"value">> := 42}} = duktape:eval(Ctx, <<"received">>).
```

#### JavaScript API

The `Erlang` global object provides the following methods:

**`Erlang.emit(type, data)`** - Send an event to the Erlang handler process.

```javascript
Erlang.emit('custom_event', {key: 'value', count: 42});
```

The handler receives: `{duktape, custom_event, #{<<"key">> => <<"value">>, <<"count">> => 42}}`

**`Erlang.log(level, ...args)`** - Send a log message to the Erlang handler.

```javascript
Erlang.log('info', 'User logged in:', userId);
Erlang.log('warning', 'Rate limit exceeded');
Erlang.log('error', 'Connection failed:', error);
Erlang.log('debug', 'Request details:', request);
```

The handler receives: `{duktape, log, #{level => info, message => <<"User logged in: 123">>}}`

**`Erlang.on(event, callback)`** - Register a callback for events from Erlang.

```javascript
Erlang.on('config_update', function(config) {
    applyConfig(config);
    return 'applied';
});
```

**`Erlang.off(event)`** - Unregister a callback.

```javascript
Erlang.off('config_update');
```

#### Console Object

A standard `console` object is available that wraps `Erlang.log`:

```javascript
console.log('Hello, world!');      // level: info
console.info('Information');        // level: info
console.warn('Warning message');    // level: warning
console.error('Error occurred');    // level: error
console.debug('Debug info');        // level: debug
```

#### Complete Example

```erlang
%% Create context with event handler
{ok, Ctx} = duktape:new_context(#{handler => self()}),

%% Set up JavaScript callback
{ok, _} = duktape:eval(Ctx, <<"
    var messages = [];
    Erlang.on('message', function(msg) {
        messages.push(msg);
        console.log('Received:', msg.text);
        return messages.length;
    });
">>),

%% Send from Erlang
{ok, 1} = duktape:send(Ctx, message, #{text => <<"Hello">>}),
{ok, 2} = duktape:send(Ctx, message, #{text => <<"World">>}),

%% Receive console.log events
receive {duktape, log, #{message := <<"Received: Hello">>}} -> ok end,
receive {duktape, log, #{message := <<"Received: World">>}} -> ok end,

%% Verify messages were stored
{ok, [#{<<"text">> := <<"Hello">>}, #{<<"text">> := <<"World">>}]} =
    duktape:eval(Ctx, <<"messages">>).
```

### Erlang Functions

Register Erlang functions that can be called synchronously from JavaScript.

#### `register_function(Ctx, Name, Fun) -> ok | {error, term()}`

Register an Erlang function callable from JavaScript. The function receives a list of arguments passed from JavaScript.

Supports both anonymous functions and `{Module, Function}` tuples. The function must accept a single argument (the list of JS arguments).

```erlang
{ok, Ctx} = duktape:new_context(),

%% Register with anonymous function
ok = duktape:register_function(Ctx, greet, fun([Name]) ->
    <<"Hello, ", Name/binary, "!">>
end),
{ok, <<"Hello, World!">>} = duktape:eval(Ctx, <<"greet('World')">>).

%% Register with {Module, Function} tuple
ok = duktape:register_function(Ctx, my_func, {my_module, my_function}).
```

**Multiple Arguments:**

```erlang
ok = duktape:register_function(Ctx, add, fun(Args) ->
    lists:sum(Args)
end),
{ok, 10} = duktape:eval(Ctx, <<"add(1, 2, 3, 4)">>).
```

**Nested Calls (Erlang functions calling each other):**

```erlang
ok = duktape:register_function(Ctx, double, fun([N]) -> N * 2 end),
{ok, _} = duktape:eval(Ctx, <<"function quadruple(n) { return double(double(n)); }">>),
{ok, 20} = duktape:eval(Ctx, <<"quadruple(5)">>).
```

**Error Handling:**

Erlang exceptions are converted to JavaScript errors:

```erlang
ok = duktape:register_function(Ctx, fail, fun(_) ->
    error(something_bad)
end),
%% JavaScript can catch the error
{ok, _} = duktape:eval(Ctx, <<"
    try {
        fail();
    } catch (e) {
        console.log('Caught:', e.message);
    }
">>).
```

**Note:** Registered functions are stored in the calling process's dictionary. The process that registers the function must also be the one that calls `eval/call`.

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

## Benchmarks

Performance benchmarks on Apple M4 Pro, Erlang/OTP 28:

| Benchmark | Ops/sec | Mean (ms) | P95 (ms) | P99 (ms) |
|-----------|--------:|----------:|---------:|---------:|
| eval_simple | 2,633 | 0.380 | 0.418 | 0.458 |
| eval_complex | 2,355 | 0.425 | 0.469 | 0.513 |
| eval_bindings_small (5 vars) | 2,632 | 0.380 | 0.418 | 0.457 |
| eval_bindings_large (50 vars) | 1,688 | 0.593 | 0.715 | 0.773 |
| call_no_args | 2,513 | 0.398 | 0.488 | 0.532 |
| call_with_args (5 args) | 2,478 | 0.404 | 0.492 | 0.533 |
| call_many_args (20 args) | 2,176 | 0.460 | 0.540 | 0.604 |
| type_convert_simple | 2,548 | 0.392 | 0.461 | 0.524 |
| type_convert_array (1000 elem) | 2,121 | 0.472 | 0.555 | 0.594 |
| type_convert_nested | 2,489 | 0.402 | 0.485 | 0.520 |
| context_create | 2,702 | 0.370 | 0.430 | 0.485 |
| module_require_cached | 2,230 | 0.448 | 0.525 | 0.577 |
| concurrent_same_context | 120,617 | 0.829 | 1.070 | 1.199 |
| concurrent_many_contexts | 33,276 | 3.005 | 3.380 | 3.873 |

Run benchmarks yourself:

```bash
./run_bench.sh              # Run all benchmarks
./run_bench.sh eval_simple  # Run specific benchmark
./run_bench.sh --smoke      # Quick validation
```

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

Duktape is also licensed under the MIT License.
