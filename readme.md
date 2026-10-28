
# DeDup

This was my first ever 'semi-vibecoded' utility. It was originally a very slow MacOS shell script and was converted to C++ by AI for a massive speed boost.

The utility scans all the files in the specified source directory recursively and stores a CRC value using [xxhash](https://github.com/Cyan4973/xxHash) from the content of each file. After which the additional specified destination directory is scanned recursively and if the same CRC value of a file in a destination directory file is already stored in the source directory CRC table, then a hardlink is created as long as both files are on the same mount point, if not, then a symlink is created.

This as a result saves precious disk space with duplicated files which are have completely the same content because what's the point in having duplicate files when the file system (in the test case - APFS) has a reference counter system for each file? After all, if one of the hardlinked files is modified, then the reference counter on the original file is decreased and its content never changes.

Please note that this is 'potentially' a destructive utility if used improperly and does not currently account for niche issues that may occur such as if there are mount points nested in other mount points. Be careful and test it out on dummy files for your use case before using.

Uses [xxHash](https://github.com/Cyan4973/xxHash) - Copyright (c) 2012-2021 Yann Collet, All rights reserved.

## Copyright © 2026 Mhatxotic Design. All Rights Reserved.
