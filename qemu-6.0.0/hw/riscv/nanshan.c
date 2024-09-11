#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "hw/sysbus.h"
#include "hw/char/serial.h"
#include "hw/misc/unimp.h"
#include "target/riscv/cpu.h"
#include "hw/riscv/riscv_hart.h"
#include "hw/riscv/nanshan.h"
#include "hw/riscv/boot.h"
#include "chardev/char.h"
#include "sysemu/arch_init.h"
#include "sysemu/sysemu.h"
#include "exec/address-spaces.h"
#include "hw/riscv/numa.h"

//直接从mrom开始执行
    //DRAM暂不使用
static const MemMapEntry nanshan_memmap[] = {
    [NANSHAN_MROM] = {0x0, 0x8000},
    [NANSHAN_SRAM] = {0x8000, 0x8000},
    [NANSHAN_UART0] = {0x10000000, 0x100},
    [NANSHAN_DRAM] = {0x80000000, 0x0},
};

static void nanshan_machine_instance_init(Object *obj)
{
}

void nanshan_setup_rom_reset_vec(MachineState *machine, RISCVHartArrayState *harts,
                               hwaddr start_addr,
                               hwaddr rom_base, hwaddr rom_size,
                               uint32_t fdt_load_addr, void *fdt)
{
    int i;
    uint32_t start_addr_hi32 = 0x00000000;

    if (!riscv_is_32bit(harts)) {
        start_addr_hi32 = start_addr >> 32;
    }
    /* reset vector */
    uint32_t reset_vec[10] = {
        0x00000297,                  /* 1:  auipc  t0, %pcrel_hi(fw_dyn) */
        0x02828613,                  /*     addi   a2, t0, %pcrel_lo(1b) */
        0xf1402573,                  /*     csrr   a0, mhartid  */
        0,
        0,
        0x00028067,                  /*     jr     t0 */
        start_addr,                  /* start: .dword */
        start_addr_hi32,
        fdt_load_addr,               /* fdt_laddr: .dword */
        0x00000000,
                                     /* fw_dyn: */
    };
    if (riscv_is_32bit(harts)) {
        reset_vec[3] = 0x0202a583;   /*     lw     a1, 32(t0) */
        reset_vec[4] = 0x0182a283;   /*     lw     t0, 24(t0) */
    } else {
        reset_vec[3] = 0x0202b583;   /*     ld     a1, 32(t0) */
        reset_vec[4] = 0x0182b283;   /*     ld     t0, 24(t0) */
    }

    /* copy in the reset vector in little_endian byte order */
    for (i = 0; i < ARRAY_SIZE(reset_vec); i++) {
        reset_vec[i] = cpu_to_le32(reset_vec[i]);
    }
    rom_add_blob_fixed_as("mrom.reset", reset_vec, sizeof(reset_vec),
                          rom_base, &address_space_memory);
    return;
}


static void nanshan_board_init(MachineState *machine) {
    const MemMapEntry *mmap = nanshan_memmap;
    NanshanState *s = NANSHAN_MACHINE(machine);
    MemoryRegion *system_memory = get_system_memory();
    MemoryRegion *main_mem = g_new(MemoryRegion, 1); //ddr
    MemoryRegion *sram_mem = g_new(MemoryRegion, 1); //sram
    MemoryRegion *mask_rom = g_new(MemoryRegion, 1); //mrom

    char *soc_name;
    int i, base_hartid, hart_count;

 /* Check socket count limit */
    if (NANSHAN_SOCKETS_MAX < riscv_socket_count(machine)) {
        error_report("number of sockets/nodes should be less than %d",
            NANSHAN_SOCKETS_MAX);
        exit(1);
    }

    /* Initialize sockets */
    for (i = 0; i < riscv_socket_count(machine); i++) {
        if (!riscv_socket_check_hartids(machine, i)) {
            error_report("discontinuous hartids in socket%d", i);
            exit(1);
        }

        base_hartid = riscv_socket_first_hartid(machine, i);
        if (base_hartid < 0) {
            error_report("can't find hartid base for socket%d", i);
            exit(1);
        }

        hart_count = riscv_socket_hart_count(machine, i);
        if (hart_count < 0) {
            error_report("can't find hart count for socket%d", i);
            exit(1);
        }

        soc_name = g_strdup_printf("soc%d", i);
        object_initialize_child(OBJECT(machine), soc_name, &s->soc[i],
                                TYPE_RISCV_HART_ARRAY);
        g_free(soc_name);
        object_property_set_str(OBJECT(&s->soc[i]), "cpu-type",
                                machine->cpu_type, &error_abort);
        object_property_set_int(OBJECT(&s->soc[i]), "hartid-base",
                                base_hartid, &error_abort);
        object_property_set_int(OBJECT(&s->soc[i]), "num-harts",
                                hart_count, &error_abort);
        sysbus_realize(SYS_BUS_DEVICE(&s->soc[i]), &error_abort);
    }
    /* register system main memory (actual RAM) 这里size=0 暂时忽略 */
    memory_region_init_ram(main_mem, NULL, "riscv.nanshan.ram", machine->ram_size, &error_fatal);
    memory_region_add_subregion(system_memory, mmap[NANSHAN_DRAM].base, main_mem);

    /* sram为早期启动代码时数据存放空间 */
    memory_region_init_ram(sram_mem, NULL, "riscv.nanshan.sram",
                           mmap[NANSHAN_SRAM].size, &error_fatal);
    memory_region_add_subregion(system_memory, mmap[NANSHAN_SRAM].base,
                                sram_mem);

    /* maskrom用于cpu启动时固定执行其内部的代码 */
    memory_region_init_rom(mask_rom, NULL, "riscv.nanshan.mrom", mmap[NANSHAN_MROM].size, &error_fatal);
    memory_region_add_subregion(system_memory, mmap[NANSHAN_MROM].base, mask_rom);
    
    /* load the reset vector */
    // 暂时忽略设备树, 直接执行mrom, kernel_entry = 0
    nanshan_setup_rom_reset_vec(machine, &s->soc[0], mmap[NANSHAN_MROM].base,
                              mmap[NANSHAN_MROM].base,
                              mmap[NANSHAN_MROM].size,
                              0x0, 0x0);
}


static void nanshan_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "RISC-V Nanshan board";
    mc->init = nanshan_board_init;
    mc->max_cpus = NANSHAN_CPUS_MAX;
    mc->is_default = false;
    mc->default_cpu_type = TYPE_RISCV_CPU_BASE;
    // 多核初始化
    mc->possible_cpu_arch_ids = riscv_numa_possible_cpu_arch_ids;
    // 多核cpu配置
    mc->cpu_index_to_instance_props = riscv_numa_cpu_index_to_props;
    mc->get_default_cpu_node_id = riscv_numa_get_default_cpu_node_id;
    mc->numa_mem_supported = true;
}

static const TypeInfo nanshan_machine_typeinfo = {
    .name = MACHINE_TYPE_NAME("nanshan"),
    .parent     = TYPE_MACHINE,
    .class_init = nanshan_machine_class_init,
    .instance_init = nanshan_machine_instance_init,
    .instance_size = sizeof(NanshanState),
};


// board 加入到 type table
static void nanshan_machine_init_register_types(void) {
    type_register_static(&nanshan_machine_typeinfo);
}

// 初始化模块，注册到module table
type_init(nanshan_machine_init_register_types)