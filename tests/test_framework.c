#include "framework.h"
int main(void) {
    CHECK(1 == 1);
    CHECK(0xFFu == 255u);
    TEST_DONE();
}
