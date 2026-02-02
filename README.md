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
clang++ -o main main.cpp -std=c++23 -stdlib=libc++ -fprebuilt-module-path=./bin/pcms/ -fprebuilt-module-path=./bin/std/ -L./bin/libs/ -lrio  -luring
clang++ ./examples/04-future-echo-server.cpp -o main2 -std=c++23 ./bin/libs/librio.a -fprebuilt-module-path=./bin/pcms -fprebuilt-module-path=./bin/std -luring

```

## Notes

- If you are doing incremental builds and clang cries, try `bld -build`. This doesn't build std again.
- If you want to compile std again too, do `bld -build-all`, but it will compile whole thing again.

## Credits

[rinav](github.com/rrrinav)
