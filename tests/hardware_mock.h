#ifndef HARDWARE_MOCK_H
#define HARDWARE_MOCK_H

#ifdef TEST_MODE

int hardware_mock_init(void);
int hardware_mock_set_ac_online(int online);
void hardware_mock_cleanup(void);

#endif /* TEST_MODE */

#endif /* HARDWARE_MOCK_H */
