ifneq ($(KERNELRELEASE),)
clean-files := pkvm/hyp.lds pkvm/hyp-reloc.S
obj-m := pkvm_smc.o

pkvm_smc-y := pkvm-smc.o pkvm/kvm_nvhe.o

$(obj)/pkvm/kvm_nvhe.o: FORCE
	$(Q)$(MAKE) $(build)=$(obj)/pkvm $(obj)/pkvm/kvm_nvhe.o
else
all:
	make -C $(KDIR) M=$(PWD) modules
clean:
	make -C $(KDIR) M=$(PWD) clean
endif