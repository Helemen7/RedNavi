#!/usr/bin/env python3
'''
Decompresses a pbzx stream.

Simplified/corrected version of <https://gist.github.com/Lekensteyn/6e0840e77bc9bd013f57>

Example usage (from Python):

  decompress_pbzx(open('PayloadJava', 'rb'), open('PayloadJava.cpio', wb'))

Example usage (from shell):

  # These are all equivalent
  ./decompress_pbzx.py < PayloadJava > PayloadJava.cpio
  ./decompress_pbzx.py PayloadJava > PayloadJava.cpio
  ./decompress_pbzx.py PayloadJava PayloadJava.cpio

Another example, extract Payload from a .pkg file, convert it to a cpio
archive (this script) and list contents (cpio -t):

  bsdtar -xOf some.pkg Payload | ./decompress_pbzx.py Payload | cpio -t
'''

import sys
import lzma

def read_f(f, count):
	data = f.read(count)
	assert len(data) == count, f'unexpected EOF: got {len(data)}, expected {count}'
	return data

def write_f(f, data):
	count = f.write(data)
	assert len(data) == count, f'could not write all data: {len(data)} got, {count} written'

read64 = lambda f: int.from_bytes(read_f(f, 8))

def decompress_pbzx(pbzx_file, out_file, block_size=1024**2, log=None):
	magic = pbzx_file.read(4)
	assert magic == b'pbzx', f'not a pbzx file (magic = {magic})'

	flags = read64(pbzx_file)
	if log: print(f'flags = {flags:016x}', file=log)

	while (flags & (1 << 24)):
		flags, length = read64(pbzx_file), read64(pbzx_file)
		# if log: print(f'flags = {flags:016x}, length = {length:#x}', file=log)

		unxz = None
		if length != 0x1000000: # compressed block
			unxz = lzma.LZMADecompressor()

		while length > 0:
			block = read_f(pbzx_file, sz := min(length, block_size))
			if unxz: block = unxz.decompress(block)
			write_f(out_file, block)
			length -= sz

		if unxz:
			assert unxz.eof, 'incomplete LZMA block'
			assert not unxz.unused_data, 'trailing data after LZMA block'

	if log: print(f'final flags = {flags:016x}', file=log)

if __name__ == '__main__':
	def open_file(argno, mode, f):
		if len(sys.argv) > argno:
			return open(sys.argv[argno], mode)
		return f
	in_file = open_file(1, "rb", sys.stdin.buffer)
	out_file = open_file(2, "wb", sys.stdout.buffer)
	decompress_pbzx(in_file, out_file, log=sys.stderr)
	if in_file.read(1):
		print(f'warning: trailing data after stream ({in_file.tell()-1})', file=sys.stderr)
