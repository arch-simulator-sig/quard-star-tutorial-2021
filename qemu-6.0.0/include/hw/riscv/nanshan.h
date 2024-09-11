#ifndef HW_RISCV_NANSHAN_H
#define HW_RISCV_NANSHAN_H


#include "hw/riscv/riscv_hart.h"
#include "hw/sysbus.h"
#include "qom/object.h"


#define NANSHAN_CPUS_MAX 8
#define NANSHAN_SOCKETS_MAX 8

#define TYPE_NANSHAN_MACHINE MACHINE_TYPE_NAME("nanshan")
typedef struct NanshanState NanshanState;

DECLARE_INSTANCE_CHECKER(NanshanState, NANSHAN_MACHINE,
                         TYPE_NANSHAN_MACHINE)

struct NanshanState
{
    // private
    MachineState parent;

    // public
    RISCVHartArrayState soc[NANSHAN_CPUS_MAX];
};

enum {
    NANSHAN_MROM,
    NANSHAN_SRAM,
    NANSHAN_UART0,
    NANSHAN_DRAM
};

#endif