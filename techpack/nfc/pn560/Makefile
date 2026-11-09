$(info   CONFIG_NXP_NFC_I2C = $(CONFIG_NXP_NFC_I2C))
obj-m  += nfc_i2c.o

nfc_i2c-y := common.o \
             common_ese.o \
             i2c_drv.o

ccflags-y += -DCONFIG_TMS_GUIDE_DEVICE=1
