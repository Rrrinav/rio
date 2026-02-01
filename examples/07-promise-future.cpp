import std;
import rio;

std::string File = R"(This is an example file, let us read it.)";

// Internal workings of event loop
struct context
{
    std::string data{};
    rio::Promise<rio::promise::State<std::string>> promise{};
    std::size_t index = 0;
    bool done = false;
};

// You will notice that size or string and number of ticks are same because one char is read per tick.
auto main() -> int
{
    // It is advised to set up state management and lifetimes your own selves because factory functions maybe deceiving.
    // You won't know when future holds ownership of the state and when it won't.

    // To set up a future-promise pair, you have to make a State that is both pollable and resolveable
    // i.e. acts as both future and a promise. There is an inbuilt state struct available and you can use it.
    // But you can also write your own, if they follow this concept.
    // template <typename S>
    // concept StateLike = requires(S *s, std::error_code ec) {
    //     { s->poll() } -> std::same_as<rio::fut::res<typename S::value_type>>;
    //     { s->reject(ec) };
    // };
    // Note: It must also have a resolve function but making a concept out of it becomes too restrictive.
    auto* state = new rio::promise::State<std::string>{};

    // Then you bind that state to promise.
    // Note: promise holds a raw pointer to the state.
    rio::Promise pr{ .state = state };

    // Then you bind that state to future and return the poll result for library state, you are free to do anything else with it too,
    // if you want but library state isn't written for such purposes. At the end of the day you have to return a rio::fut::res;
    rio::Future fut{
        state,
        [](rio::promise::State<std::string>* s) {
            return s->poll();
        }
    };
    // NOTE: **** If state was on stack, you would do &state for both promise and future and it would work in this case because nothing moves around. ******
    //      Library gives you full control on it.

    // An async function like emulation where this future reads one char per tick.
    // To make state error-ful, do promise.reject(std::errc), to add value, do promise.resolve(value);
    // If you did state-mangement properly, value/error will propagate to the future bound to promise/state.
    rio::Future fut_read{
        context{ "", pr, 0, false },
        [](context& cont) -> rio::fut::res<void> {
            if (cont.done)
                return rio::fut::res<void>::ready();

            if (cont.index < File.size())
            {
                cont.data += File[cont.index++];
                return rio::fut::res<void>::pending();
            }

            cont.promise.resolve(cont.data);
            cont.done = true;
            return rio::fut::res<void>::ready();
        }
    };

    int tick = 0;

    while (true)
    {
        // I do different things to achieve same results, so users can know all the ways.
        fut_read.poll();
        auto value_res = rio::poll(fut);

        if (value_res.state == rio::fut::status::ready)
        {
            std::println("tick: {}, read({}): {}", tick, value_res.value->size(), value_res.value.value_or("some error occured"));
            break;
        }

        tick++;
    }
    delete state;
    return 0;
}
