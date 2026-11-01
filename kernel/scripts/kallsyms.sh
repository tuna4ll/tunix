#!/bin/sh
set -eu

NM=${1:?usage: kallsyms.sh NM [KERNEL]}
KERNEL=${2:-}

emit_empty() {
	printf '%s\n' '	.section .rodata.kallsyms,"a"' '	.balign 8' \
		'	.globl kallsyms_count' 'kallsyms_count:' '	.quad 0' \
		'	.globl kallsyms_addresses' 'kallsyms_addresses:' \
		'	.globl kallsyms_sizes' 'kallsyms_sizes:' \
		'	.globl kallsyms_offsets' 'kallsyms_offsets:' \
		'	.globl kallsyms_types' 'kallsyms_types:' \
		'	.globl kallsyms_names' 'kallsyms_names:' '	.byte 0'
}

if [ -z "$KERNEL" ]; then
	emit_empty
	exit 0
fi

"$NM" -n -S --defined-only "$KERNEL" | LC_ALL=C awk '
function hex(text,    value, digit, i) {
	value = 0
	text = tolower(substr(text, length(text) - 7))
	for (i = 1; i <= length(text); i++) {
		digit = index("0123456789abcdef", substr(text, i, 1)) - 1
		value = value * 16 + digit
	}
	return value
}
BEGIN { seen = 0; codes["T"] = 84; codes["t"] = 116; codes["W"] = 87; codes["w"] = 119 }
{
	if (NF == 4) { address = $1; size = $2; type = $3; name = $4 }
	else if (NF == 3) { address = $1; size = ""; type = $2; name = $3 }
	else next
	if (name == "_stext") stext = address
	if (name == "_etext") etext = address
	if (type !~ /^[tTwW]$/ || name ~ /^\$/ || name ~ /^\.L/) next
	if (name == "_stext" || name == "_etext") next
	seen_addresses[seen] = address; seen_sizes[seen] = size
	seen_types[seen] = type; seen_names[seen] = name
	seen++
}
END {
	if (stext == "" || etext == "") {
		print "kallsyms.sh: the kernel has no _stext/_etext" > "/dev/stderr"
		exit 1
	}
	addresses[0] = stext; sizes[0] = ""; types[0] = "T"; names[0] = "_stext"
	count = 1
	for (j = 0; j < seen; j++) {
		address = seen_addresses[j]
		if (address < stext || address >= etext) continue
		at = count
		while (at > 1 && addresses[at - 1] == address && sizes[at - 1] != "" && seen_sizes[j] == "") {
			addresses[at] = addresses[at - 1]; sizes[at] = sizes[at - 1]
			types[at] = types[at - 1]; names[at] = names[at - 1]
			at--
		}
		addresses[at] = address; sizes[at] = seen_sizes[j]
		types[at] = seen_types[j]; names[at] = seen_names[j]
		count++
	}
	addresses[count] = etext; sizes[count] = "0"; types[count] = "T"; names[count] = "_etext"
	limit = hex(etext)
	for (i = count - 1; i >= 0; i--) {
		if (addresses[i + 1] != addresses[i]) limit = hex(addresses[i + 1])
		gap = limit - hex(addresses[i])
		if (sizes[i] == "" || hex(sizes[i]) == 0 || hex(sizes[i]) > gap) sizes[i] = sprintf("%x", gap)
	}
	count++
	print "\t.section .rodata.kallsyms,\"a\""
	print "\t.balign 8"
	print "\t.globl kallsyms_count"
	print "kallsyms_count:"
	printf "\t.quad %d\n", count
	print "\t.globl kallsyms_addresses"
	print "kallsyms_addresses:"
	for (i = 0; i < count; i++) printf "\t.quad 0x%s\n", addresses[i]
	print "\t.globl kallsyms_sizes"
	print "kallsyms_sizes:"
	for (i = 0; i < count; i++) printf "\t.long 0x%s\n", sizes[i]
	print "\t.globl kallsyms_offsets"
	print "kallsyms_offsets:"
	offset = 0
	for (i = 0; i < count; i++) {
		printf "\t.long %d\n", offset
		offset += length(names[i]) + 1
	}
	print "\t.globl kallsyms_types"
	print "kallsyms_types:"
	for (i = 0; i < count; i++) printf "\t.byte 0x%02x\n", codes[types[i]]
	print "\t.globl kallsyms_names"
	print "kallsyms_names:"
	for (i = 0; i < count; i++) printf "\t.asciz \"%s\"\n", names[i]
}'
