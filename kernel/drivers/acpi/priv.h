#ifndef TUNIX_ACPI_PRIV_H
#define TUNIX_ACPI_PRIV_H

#include <stdint.h>

#include <uacpi/namespace.h>
#include <uacpi/types.h>

int acpi_host_start(void);

int acpi_ec_probe(void);
void acpi_ec_start(void);
int acpi_ec_present(void);
uint64_t acpi_ec_events(void);

void acpi_button_probe(void);
void acpi_video_probe(void);
unsigned acpi_video_outputs(void);
void acpi_thermal_probe(void);
void acpi_processor_probe(void);
unsigned acpi_thermal_zones(void);

int acpi_device_present(uacpi_namespace_node *node);
const char *acpi_node_path(uacpi_namespace_node *node, char *buffer, unsigned size);

#endif
