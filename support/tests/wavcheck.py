import struct, sys
raw = open(sys.argv[1], "rb").read()
body = raw[44:]
count = len(body) // 2
samples = struct.unpack("<%dh" % count, body[: count * 2]) if count else ()
loud = sum(1 for s in samples if abs(s) > 256)
peak = max((abs(s) for s in samples), default=0)
print("WAV bytes=%d samples=%d nonsilent=%d peak=%d %s"
      % (len(body), count, loud, peak,
         "AUDIBLE" if count and loud > count // 100 else "SILENT"))

# The tail on its own: an underrun that replays the ring is loud there, and one
# that has been silenced is not. This is the difference a listener hears.
tail = samples[-int(len(samples) * 0.25):] if samples else ()
tail_loud = sum(1 for s in tail if abs(s) > 256)
print("WAVTAIL samples=%d nonsilent=%d %s"
      % (len(tail), tail_loud,
         "REPEATING" if tail and tail_loud > len(tail) // 4 else "QUIET"))
