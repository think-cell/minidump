#!/usr/bin/env python3

import os
import re
import shutil
import subprocess 
import sys
import time

# Set these depending on your setup
strBinaryCacheServer = "smb://server/path"
strMountPoint = "path_to_mount"
strSymbolSubfolder = "macOS Binaries"

if __name__ == "__main__":
	strSymbolDir = os.path.join(strMountPoint, strSymbolSubfolder)

	if not os.path.isdir(strSymbolDir):
		os.makedirs(strMountPoint, exist_ok=True)
		subprocess.check_call(["mount", "-t", "smbfs", strBinaryCacheServer, strMountPoint])

	matchOSVersion = re.search(
		r"^ProductVersion:\s+([0-9\.]+)\nBuildVersion:\s+([A-Za-z0-9]+)",
		subprocess.check_output(["sw_vers"]).decode(),
		re.MULTILINE
	)

	strBinaryDir = os.path.join(strSymbolDir, matchOSVersion.group(1) + "_" + matchOSVersion.group(2))
	os.mkdir(strBinaryDir)

	for strSearchDir in ["/System/Library/Frameworks", "/System/Library/PrivateFrameworks", "/usr/lib"]:
		for strRoot, liststrDirs, liststrFiles in os.walk(strSearchDir):
			# Test if file is mach binary starting with magic value
			abMagicMach64 = bytearray([0xcf, 0xfa, 0xed, 0xfe])
			abMagicFatHeader = bytearray([0xca, 0xfe, 0xba, 0xbe])
			for strFile in liststrFiles:
				strFilePath = os.path.abspath(os.path.join(strRoot, strFile))
				if not os.path.islink(strFilePath):
					with open(strFilePath, "rb") as f:
						ab = bytearray(f.read(4))
						if ab==abMagicMach64 or ab==abMagicFatHeader:
							print(strFilePath)
							strFileTargetPath = os.path.join(strBinaryDir, os.path.relpath(strFilePath, "/"))
							os.makedirs(os.path.dirname(strFileTargetPath), exist_ok=True)
							shutil.copyfile(strFilePath, strFileTargetPath)

	strDyldCache = "/System/Library/dyld"
	if os.path.isdir(strDyldCache):
		strTargetDyldCache = os.path.join(strBinaryDir, os.path.relpath(strDyldCache, "/"))
		os.makedirs(strTargetDyldCache)
		for strArch in os.listdir(strDyldCache):
			strArchLocalFile = os.path.join(strDyldCache, strArch)
			assert strArch.startswith("dyld_shared_cache") or strArch.startswith("aot_shared_cache") 
			if strArch.startswith("dyld_shared_cache") and os.path.isfile(strArchLocalFile) and not strArch.endswith(".map"):
				print(strArchLocalFile)
				
				strArchTargetDir = os.path.join(strTargetDyldCache, strArch)
				os.mkdir(strArchTargetDir)
				subprocess.check_call(["dyld_shared_cache_util", "-extract", strArchTargetDir, strArchLocalFile])
			
