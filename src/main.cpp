#include "app/Application.hpp"

int main(int argc, char* argv[])
{
    ifs::Application app;

    if (!app.initialize(argc, argv))
        return 1;

    return app.run();
}
