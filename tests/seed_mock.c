#ifdef TEST_MODE

#include "hardware_mock.h"

#include <stdlib.h>

int main(void)
{
    return hardware_mock_init() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

#else

#include <stdlib.h>

int main(void)
{
    return EXIT_SUCCESS;
}

#endif /* TEST_MODE */
