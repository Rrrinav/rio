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
