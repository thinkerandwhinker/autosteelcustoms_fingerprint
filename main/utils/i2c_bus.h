/**
 * I2C_BUS_H
 *
 * bus master for lcd 16x2 & rtc
 */
#ifndef I2C_BUS_H
#define I2C_BUS_H

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * only allow to call at booting process or at 1 context
     * it's idemp
     *
     * only provided for for ui_task_start()
     */
    esp_err_t i2c_bus_init(void);

    /**
     * NULL when i2c_bus_init() not success yet
     *
     * for other modul consumption
     */
    i2c_master_bus_handle_t i2c_bus_get(void);

#ifdef __cplusplus
}
#endif

#endif
