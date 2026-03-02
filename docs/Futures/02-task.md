# Futures: Task<T>

This is the type you will probably use most of the time when you just want to store and run things at the application boundary.

> [!WARNING]
> Incurs heap allocation and virtual function call overhead.

## Structure

This is the basic internal structure of the task. It is pretty easy to understand:

```cpp
template <typename T>
struct Task_concept
{
    virtual ~Task_concept() = default;
    virtual rio::fut::res<T> poll() = 0;
};
```

We then wrap this interface to create a new user-facing type that contains a `std::unique_ptr<Task_concept<T>>`. This **type erases** the complicated, deeply nested lambda type signatures from your variables.

By doing this, we make two deliberate trade-offs:

* **Heap Allocation:** To avoid object slicing and the limitation of not knowing the exact future size at compile time, we allocate the inner state on the heap.
* **Virtual Calls:** To hide the complexity of the exact pollers, we use virtual function calls to execute the state machine.

But these make programming using futures more pleasent.

## Advice

I highly recommend creating your async pipelines using the zero-overhead `rio::Future` types first, and only boxing them into a `rio::fut::Task<T>` at the very end when you need to store them in a container or pass them across system boundaries.

Creating pipelines is not tough, guessing their final type is. I juse use `auto` or for a single future, use`rio::Call_poll`. XD.

## Usage

```cpp
// we can just save the complicated future into `Task`.
rio::fut::Task<void> handle_client(rio::context &ctx, rio::Tcp_socket sock, rio::address addr)
{
    auto session = std::make_shared<Session>(ctx, std::move(sock), addr);

    return rio::fut::loop(session, [](std::shared_ptr<Session> sess) {
        return rio::fut::buff::read_till(sess->reader, '0')
            .then([sess](std::optional<std::string> msg) {
                if (!msg) {
                    std::println(" [RIO]: {} disconnected", sess->addr);
                    sess->write_buf.clear();
                } else {
                    while (!msg->empty() && (msg->back() == '\n' || msg->back() == '\r'))
                        msg->pop_back();
                    std::println(" [RIO]: {} sent: '{:?}'", sess->addr, escape_string(*msg)); // escape_string string is anonymous
                    msg->push_back('\n');
                    sess->write_buf = std::move(*msg);
                }
                return rio::fut::write_all(sess->ctx, sess->sock, sess->write_buf);
            })
            .then([sess]() -> rio::fut::ready_t<std::shared_ptr<Session>> {
                if (sess->write_buf.empty())
                    return rio::fut::error<std::shared_ptr<Session>>(std::make_error_code(std::errc::broken_pipe));
                return rio::fut::ready(sess);
            });
    });
}
```

Here `rio::fut::Task<void>` let's you handle the return type with ease which if you go to even try without `auto` will look like this.

```cpp
// Given by compiler
rio::Future<
    rio::fut::Loop_impl<
        std::shared_ptr<Session>,
        handle_client(rio::context&, rio::Tcp_socket, rio::address)::$_0 // This just means lambda, it removes nested futures info completely.
    >,
    rio::fut::Call_poll
>

// This is what I think it would be
rio::Future<rio::fut::Loop_impl<
        std::shared_ptr<Session>,
        /* The lambda $_0, which returns the nested pipeline: */
        auto (*)(std::shared_ptr<Session>) -> rio::Future<
            rio::fut::Then_impl<
                rio::Future<
                    rio::fut::Then_impl<
                        rio::Future<
                            rio::fut::buff::Read_till_impl<
                                rio::Async_buffered_reader<rio::Tcp_socket, 4096>
                            >
                        >,
                        handle_client(rio::context&, rio::Tcp_socket, rio::address)::<lambda(std::optional<std::string>)>
                    >
                >,
                handle_client(rio::context&, rio::Tcp_socket, rio::address)::<lambda()>
            >
        >
    >,
    rio::fut::Call_poll
>
```
But this can fixed too and be made kind of simpler by using `structs` and `rio::Call_poll{}`.
