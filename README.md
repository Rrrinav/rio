# rio

An async runtime based on liburing.

> [!WARNING]
    Doesn't use raw io_uring, I am using linux liburing abstraction layer/library.

Read requirments.

## Building

```sh
$ g++ ./bld.cpp -o bld --std=c++23 && ./bld
```

You can find precompiled modules in ./bin/pcms/ & linking files/libs in ./bin/libs/

Follow instructions if bulding std module fails.

## Requirements

- Clang 21.1
    - basically modules and c++23 support
- libc++
- liburing

## Usage

### Import rio module
```cpp
import rio
// Implementation
```

### Compile
```sh
clang++  -o main main.cpp -std=c++23 -stdlib=libc++ -fprebuilt-module-path=./bin/pcms/ -fprebuilt-module-path=./bin/std/ -L./bin/libs/ -lrio  -luring
```
