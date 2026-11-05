#include "app/App.h"

int main() {
    dhsh::App app;
    return app.logical_width() == 800 && app.logical_height() == 600 ? 0 : 1;
}
