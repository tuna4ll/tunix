// The kernel image, as objcopy -O binary makes it from kernel.elf: from
// kernel_image_start to the end of its data. It runs where it lies.
.section .kernel_blob, "a"
.incbin KERNEL_BIN
