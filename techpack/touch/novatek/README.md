# Spring Novatek source port

Base source is Xiaomi SM4450 NT36672C_SPI at commit 188826d57006aadbeca2ae35bb2a5baf9ae8b306. The Spring panel selection, firmware names and trim records were recovered from the original Spring module. The candidate includes the Spring notifier, power-supply charger detection, Longcheer info, lockdown, proximity and gesture integration, and the original recovery callback export. The temporary V6 build reproduced the original export CRC.

The remaining Longcheer work, grip and self-test integration, and runtime MiSight fault reporting still need porting. The driver has not been tested on hardware.

Disassembly of the working slot A driver confirms that probe calls
`register_touch_panel(dev, 0, hardware_param, hardware_operation)` after the
panel notifier is registered. The reference operation table assigns
`nvt_set_cur_value` at byte 0x20, `nvt_palm_sensor_write` at 0x60, and
`nvt_touch_dfs_test` at 0xe8. The first callback receives an integer pointer
and calls `update_driver_cur_value` after handling the mode. The DFS callback
uses Xiaomi MiSight reporting. The reference unregisters panel 0 during
both remove and shutdown.

The SM4450 source's legacy set-mode callback takes an integer value by value.
The Spring adapter uses the pointer signature, clamps the recovered hardware modes, and reports the applied value after a successful SPI transfer. The registration data starts zeroed, matching the reference parameter object. The DFS callback reproduces the five reference diagnostic event codes. Compile-time checks cover the operation table offsets and size. The rebuilt module matches the retained provider import CRCs. Hardware testing remains pending.
