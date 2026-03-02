import rio;
import std;

inline const std::string json_data = R"({
  "level1": {
    "name": "RootObject",
    "active": true,
    "count": 42,
    "level2": {
      "description": "Second level object",
      "ratio": 3.14159,
      "level3": {
        "items": [
          {
            "id": 1,
            "tags": ["alpha", "beta", "gamma"],
            "level4": {
              "enabled": false,
              "value": null,
              "level5": {
                "deepString": "Hello, JSON",
                "deepNumber": -123.456,
                "deepArray": [1, 2, 3, {"nestedKey": "nestedValue"}],
                "deepObject": {
                  "a": true,
                  "b": false,
                  "c": null
                }
              }
            }
          },
          {
            "id": 2,
            "tags": [],
            "level4": {
              "enabled": true,
              "value": 999,
              "level5": {
                "example": "Another branch",
                "numbers": [10, 20, 30],
                "objectInArray": [
                  {"x": 1},
                  {"y": 2}
                ]
              }
            }
          }
        ],
        "emptyArray": [],
        "emptyObject": {}
      }
    }
  }
})";

std::string json_data2 = R"( {"meri dost" : "Suhana"} )";

struct Custom
{
    std::string key;
    std::string val;
};

namespace rio::jsn {
void to_json(rio::jsn::Context &ctx, const Custom &custom_data)
{
    ctx.obj_b().obj_k("key").inject(custom_data.key).obj_k("val").inject(custom_data.val).obj_e();
}
} // namespace rio::jsn

int main()
{
    // Parsing
    {
        auto ast  = rio::jsn::parse(json_data);
        auto view = rio::jsn::view(ast);

        // get yields value and throws if it doesnt exist.
        std::println("{}", view.at("level1", "level2", "description").get<std::string>());
        // as yields optional, which contains value if it exists and not if it doesnt
        std::println("{}", view.at("level1", "level2", "ratio").as<double>().value());
        std::println("{}", view.at("level1", "level2", "ratio_not").as_or<double>(0.0));
        std::println("{}", view.at("level1", "level2", "ratio_not").as<double>().value_or(0.0));
        std::println("{}", view["level1"]["level2"]["description"].get<std::string>());
        std::println("{}", view["level1"]["level2"]["level3"]["items"][1]["id"].as<double>().value());
    }
    // Serialization
    {
        rio::jsn::Context ctx{};

        ctx.obj_b();
        {
            ctx.obj_kv("hoi", "hello mate!");

            ctx.obj_k("hoi separate");
            ctx.obj_v("hello mate alone!");

            ctx.obj_k("num");
            ctx.obj_v(10.69);

            ctx.obj_k("arr objs");
            ctx.arr_b();
            {
                for (auto i{0uz}; i < 5; i++)
                {
                    ctx.obj_b();
                    {
                        ctx.obj_kv("index", static_cast<double>(i));
                    }
                    ctx.obj_e();
                }
            }
            ctx.arr_e();

            ctx.obj_k("variadic arr");
            ctx.arr_b();
                ctx.arr_vs(10.0, 20.0, 30.0);
            ctx.arr_e();

            Custom cs{ "custom key", "custom value" };

            ctx.obj_k("custom");
            ctx.inject(cs);
        }
        ctx.obj_e();

        std::fstream file("./new.json", std::ios::out);
        std::println(file, "{}", ctx.get());

        // std::println(file, "{:2w}", ctx.get()); // 2 indent
        // std::println(file, "{:2f}", ctx.get()); // 2 digit after decimal
        // std::println(file, "{:2g}", ctx.get()); // 2 significant digits
        // std::println(file, "{:E}", ctx.get());  // Don't escape strings
        // std::println(file, "{:3w6gE}", ctx.get());
    }
    return 0;
}

