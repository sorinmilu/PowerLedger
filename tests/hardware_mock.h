#ifndef HARDWARE_MOCK_H
#define HARDWARE_MOCK_H

#ifdef TEST_MODE

int hardware_mock_init(void);
int hardware_mock_set_ac_online(int online);
int hardware_mock_set_bat_status(const char *status);
int hardware_mock_set_bat_power(long power_uw);
int hardware_mock_set_bat_energy(long energy_now, long energy_full);
void hardware_mock_cleanup(void);

#endif /* TEST_MODE */

#endif /* HARDWARE_MOCK_H */
