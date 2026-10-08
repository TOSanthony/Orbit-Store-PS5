/* orbit_store.elf carries the runtime it saves on the console (build/orbit_runtime.elf, the same
 * server and icon setup without this copy). Payload managers then start the saved runtime. */
extern const unsigned char orbit_runtime_image[], orbit_runtime_image_end[];
__asm__(".section .rodata\n"
        ".global orbit_runtime_image\n"
        ".global orbit_runtime_image_end\n"
        ".balign 16\n"
        "orbit_runtime_image:\n"
        ".incbin \"build/orbit_runtime.elf\"\n"
        "orbit_runtime_image_end:\n"
        ".previous\n");
