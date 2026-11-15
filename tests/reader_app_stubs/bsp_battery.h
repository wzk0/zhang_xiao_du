#pragma once
int stub_battery_soc(void);
int stub_battery_mv(void);
static inline int bsp_battery_soc(void) { return stub_battery_soc(); }
static inline int bsp_battery_mv(void) { return stub_battery_mv(); }
