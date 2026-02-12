int main()
{
    auto response = rio::http11::fetch(IO, "man_what?.com");

    while (true)
    {
        IO.poll();
    }
}
