#!/usr/bin/env python3

import os
import re
import shutil
import subprocess 
import sys
import time

# Set these depending on your setup
strMountPoint = "path_to_symbolcache"
strSymbolCacheServer = "smb://yourserver/symbolcache"

# Create a file strSymbolCacheServer/uuidsources.txt with the path to the
# binaries that should be index. This script expects that each line in the 
# file is a relative path in ~/mnt 
# The script will iterate over all files in these directories and use objdump
# to check which files are actually mach-o binaries

def WriteUuidFile(ostrArch = None):
	m = re.search(r"^\s+uuid\s+([0-9A-Z\-]+)\n", subprocess.check_output(["objdump", "--macho", "--private-headers"] + (["--arch="+ostrArch] if ostrArch else []) + [strFilePath]).decode(), re.MULTILINE)
	if m:
		print(strFilePath + " " + (ostrArch if ostrArch else ""))
		strUuid = m.group(1)

		# We use the same folder format for our uuid -> binary map that lldb would use for 
		# the uuid -> debug symbol map. See https://lldb.llvm.org/symbols.html
		# uuids have the form C4CBD2CF-39D5-3185-851E-85C7DD2F8C7F and the path to the uuid file will be
		# C4CB/D2CF/39D5/3185/851E/85C7DD2F8C7F
		liststrUuid = [strUuid[0:4]] + strUuid[4:].split("-")
		
		while True:
			try:
				strUuidFile = os.path.join(strDbDir, *liststrUuid)
				os.makedirs(os.path.dirname(strUuidFile), exist_ok=True)
				with open(strUuidFile, "w") as fout: # we truncate and only store one binary path
					fout.write(os.path.relpath(strFilePath, os.path.expanduser("~/mnt/")))
				break
			except BlockingIOError:
				time.sleep(1)
	else:
		print("\t[FAILED] No uuid found ")

if __name__ == "__main__":
	if len(sys.argv) < 2:
		print("Syntax: RebuildUuidDatabase.py <cachefolder>")
		exit(1)
	
	strDbDir = os.path.expanduser(sys.argv[1])
	os.makedirs(strDbDir, exist_ok=True)

	# Mount symbolcache if necessary
	if not os.path.exists(os.path.join(strMountPoint, "uuidsources.txt")):
		os.makedirs(strMountPoint, exist_ok=True)
		subprocess.check_call(["mount", "-t", "smbfs", strSymbolCacheServer, strMountPoint])

	astrSourceFolders = []
	with open(os.path.join(strMountPoint, "uuidsources.txt")) as fin:
		astrSourceFolders = list(filter(lambda str: not str.startswith("#"), map(lambda str: str.strip(), fin.readlines())))
	
	for strSourceDir in astrSourceFolders:
		strExpandedSourceDir = os.path.join(os.path.expanduser("~/mnt/"), strSourceDir)
		for strRoot, liststrDirs, liststrFiles in os.walk(strExpandedSourceDir):
			# Don't recurse into *.dSYM folders
			# The symbol file is a mach binary with the same UUID as our actual binary
			liststrDirs[:] = list( filter(lambda str: not str.endswith(".dSYM"), liststrDirs) )

			# Test if file is mach binary starting with magic value
			abMagicMach64 = bytearray([0xcf, 0xfa, 0xed, 0xfe])
			abMagicFatHeader = bytearray([0xca, 0xfe, 0xba, 0xbe])

			for strFile in liststrFiles:
				strFilePath = os.path.abspath(os.path.join(strRoot, strFile))
				if not os.path.islink(strFilePath):
					ab = bytearray()

					while True:
						try:
							with open(strFilePath, "rb") as f:
								ab = bytearray(f.read(4))
							break
						except BlockingIOError:
							time.sleep(1)

					if ab==abMagicMach64:
						WriteUuidFile()
					elif ab==abMagicFatHeader:
						# Write uuid file for each contained architecture, except i386.
						# Some fat binaries contain optimized versions e.g. for Haswell architecture processors
						# and the generic x86_64 binary
						for strLine in subprocess.check_output(["objdump", "--macho", "--universal-headers", strFilePath]).decode().split("\n"):
							m = re.search(r"^\s*architecture\s+(.+)", strLine, re.MULTILINE)
							if m and m.group(1) != "i386":
								WriteUuidFile( m.group(1) )
							

							
								

