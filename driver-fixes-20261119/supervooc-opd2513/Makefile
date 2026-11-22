obj-m += opd2513_vooc.o

# Opt in when building alongside the ported OEM ufcs_class module.
ifneq ($(UFCS_CLASS_SRC),)
obj-m += ufcs_oem_auth.o
CFLAGS_ufcs_oem_auth.o += -I$(UFCS_CLASS_SRC)/include -DCONFIG_OPLUS_UFCS_CLASS=1
CFLAGS_opd2513_vooc.o += -I$(UFCS_CLASS_SRC)/include -DCONFIG_OPLUS_UFCS_CLASS=1 -DOPD2513_OEM_UFCS
endif
