# Acorn Archimedes (platform independent; host build in host/)
idf_component_register(SRCS "arc.c"
                            "memc.c"
                            "ioc.c"
                            "vidc.c"
                            "sound.c"
                            "keyboard.c"
                            "cmos.c"
                            "fdc.c"
                            "floppy.c"
                            "c711.c"
                            "ide.c"
                            "hd_blank.c"
                            "rom.c"
                            "space.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES arm2
                       PRIV_REQUIRES emudbg esp_psram diskspace)

# Runs together with the CPU core in the emulation loop
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
