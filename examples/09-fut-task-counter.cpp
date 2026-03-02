import std;
import rio;

struct Counter_s
{
    int l;
    int h;
};

int main()
{
    rio::fut::Task<int> my_task = rio::fut::make(Counter_s{.l = 0, .h = 5}, [](Counter_s &state) -> rio::fut::res<int> {
        if (state.l < state.h) {
            state.l++;
            std::println("Task 01 counting... {}", state.l);
            return rio::fut::res<int>::pending();
        }
        return rio::fut::res<int>::ready(state.l);
    });

    rio::fut::Task<int> my_task2 = rio::fut::make(Counter_s{.l = 0, .h = 17}, [](Counter_s &state) -> rio::fut::res<int> {
        if (state.l < state.h) {
            state.l++;
            std::println("Task 02 counting... {}", state.l);
            return rio::fut::res<int>::pending();
        }
        return rio::fut::res<int>::ready(state.l);
    });

    std::vector<rio::fut::Task<int>> counters;

    counters.push_back(std::move(my_task));
    counters.push_back(std::move(my_task2));

    while (!counters.empty()) {

        std::erase_if(counters, [](auto &t) {
            auto s = rio::poll(t);

            if (s.state == rio::fut::status::ready) {
                std::println("Counter finished! Final value: {}", s.value.value());
                return true;
            } else if (s.state == rio::fut::status::error) {
                std::println("Task failed!");
                return true;
            }

            return false;
        });
    }

    return 0;
}

