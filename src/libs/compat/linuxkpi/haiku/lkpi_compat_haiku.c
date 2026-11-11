/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Haiku replacement for FreeBSD's linux_compat_init()/linux_compat_uninit()
 * (the globals next to them are kept by the extractor). #included at the end of the
 * generated lkpi_compat.c (it initialises that file's static variables);
 * not compiled on its own. Differences from FreeBSD: no sysctl tree (Haiku
 * has none), no vma lock (no mmap layer), per-CPU single-CPU masks built
 * simply (Haiku has at most 64 CPUs on x86_64), CPU identity from Haiku's
 * get_cpuid().
 */


static void
lkpi_compat_haiku_cpu(void)
{
	cpuid_info leaf0, leaf1;
	unsigned family, model, stepping;
	uint8_t vendor = X86_VENDOR_UNKNOWN;
	int i;

	if (get_cpuid(&leaf0, 0, 0) == B_OK) {
		if (leaf0.eax_0.vendor_id[0] == 'G')		/* GenuineIntel */
			vendor = X86_VENDOR_INTEL;
		else if (leaf0.eax_0.vendor_id[0] == 'A')	/* AuthenticAMD */
			vendor = X86_VENDOR_AMD;
		else if (leaf0.eax_0.vendor_id[0] == 'H')	/* HygonGenuine */
			vendor = X86_VENDOR_HYGON;
	}
	get_cpuid(&leaf1, 1, 0);
	stepping = leaf1.regs.eax & 0xf;
	model = (leaf1.regs.eax >> 4) & 0xf;
	family = (leaf1.regs.eax >> 8) & 0xf;
	if (family == 0xf)
		family += (leaf1.regs.eax >> 20) & 0xff;
	if (family == 0x6 || family >= 0xf)
		model |= ((leaf1.regs.eax >> 16) & 0xf) << 4;

	linux_cpu_has_clflush = (cpu_feature & CPUID_CLFSH) != 0;
	boot_cpu_data.x86_clflush_size = ((leaf1.regs.ebx >> 8) & 0xff) * 8;
	boot_cpu_data.x86_max_cores = mp_ncpus;
	boot_cpu_data.x86 = family;
	boot_cpu_data.x86_model = model;
	boot_cpu_data.x86_vendor = vendor;
	boot_cpu_data.x86_stepping = stepping;

	__cpu_data = kmalloc_array(LKPI_MAX_CPUS, sizeof(*__cpu_data),
	    M_WAITOK | M_ZERO);
	CPU_FOREACH(i)
		__cpu_data[i] = boot_cpu_data;
}

static void
linux_compat_init(void *arg)
{
	int i;

	lkpi_compat_haiku_cpu();

	kobject_init(&linux_class_root, &linux_class_ktype);
	kobject_set_name(&linux_class_root, "class");
	kobject_init(&linux_root_device.kobj, &linux_dev_ktype);
	kobject_set_name(&linux_root_device.kobj, "device");
	linux_root_device.bsddev = NULL;	/* no FreeBSD root_bus on Haiku */
	linux_class_misc.name = "misc";
	class_register(&linux_class_misc);
	init_waitqueue_head(&linux_bit_waitq);
	init_waitqueue_head(&linux_var_waitq);

	CPU_COPY(&all_cpus, &cpu_online_mask);

	static_single_cpu_mask = kmalloc_array(LKPI_MAX_CPUS,
	    sizeof(*static_single_cpu_mask), M_WAITOK | M_ZERO);
	static_single_cpu_mask_lcs = kmalloc_array(LKPI_MAX_CPUS,
	    sizeof(*static_single_cpu_mask_lcs), M_WAITOK | M_ZERO);
	CPU_FOREACH(i) {
		static_single_cpu_mask[i] = &static_single_cpu_mask_lcs[i];
		CPU_SET(i, static_single_cpu_mask[i]);
	}

	strlcpy(init_uts_ns.name.release, osrelease,
	    sizeof(init_uts_ns.name.release));
}
SYSINIT(linux_compat, SI_SUB_DRIVERS, SI_ORDER_SECOND, linux_compat_init, NULL);

static void
linux_compat_uninit(void *arg)
{
	linux_kobject_kfree_name(&linux_class_root);
	linux_kobject_kfree_name(&linux_root_device.kobj);
	linux_kobject_kfree_name(&linux_class_misc.kobj);

	kfree(static_single_cpu_mask_lcs);
	kfree(static_single_cpu_mask);
	kfree(__cpu_data);
}
SYSUNINIT(linux_compat, SI_SUB_DRIVERS, SI_ORDER_SECOND, linux_compat_uninit, NULL);
