/* ============================================================================
Dedup - Hardlink or symlink duplicate files.
Copyright (c) 2026 Mhatxotic Design.

Uses xxHash - https://github.com/Cyan4973/xxHash
Copyright (c) 2012-2021 Yann Collet, All rights reserved.

BSD 2-Clause License (https://www.opensource.org/licenses/bsd-license.php)

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
============================================================================ */
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <format>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>
/* ------------------------------------------------------------------------- */
#define XXH_IMPLEMENTATION
#define XXH_NO_EXTERNC_GUARD
#define XXH_STATIC_LINKING_ONLY
#include "xxhash.h"
/* ------------------------------------------------------------------------- */
// Structure to track counters and operational flags during execution.
struct OperationStats {
    int totalSources = 0;
    int totalSourceDirs = 0;
    int totalDests = 0;
    int totalDestDirs = 0;
    int totalHardlinked = 0;
    bool useSymlinks = false;
};
/* ------------------------------------------------------------------------- */
// Calculates a 128-bit XXH3 hash using the header-only xxHash library.
std::string GetFileHash(const std::string& filepath) {
    FILE* file = fopen(filepath.c_str(), "rb");
    if (file == nullptr) {
        return "";
    }

    XXH3_state_t* state = XXH3_createState();
    if (state == nullptr) {
        fclose(file);
        return "";
    }

    if (XXH3_128bits_reset(state) == XXH_ERROR) {
        XXH3_freeState(state);
        fclose(file);
        return "";
    }

    // Read file contents in 16 KiB chunks.
    char buffer[16384];
    size_t bytesRead = 0;
    while ((bytesRead = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        XXH3_128bits_update(state, buffer, bytesRead);
    }

    XXH128_hash_t hash = XXH3_128bits_digest(state);
    XXH3_freeState(state);
    fclose(file);

    // Format 128-bit result into a 32-character hexadecimal string.
    return std::format("{:016x}{:016x}", hash.high64, hash.low64);
}
/* ------------------------------------------------------------------------- */
// Traverses source directory recursively, computes checksums, and populates
// unique file records.
void ReadSources(const std::string& currentPath,
                 std::unordered_map<std::string, std::string>& filesMap,
                 OperationStats& stats) {
    DIR* dir = opendir(currentPath.c_str());
    if (dir == nullptr) {
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }

        std::string fullPath = currentPath + "/" + name;
        struct stat fileStat;

        // Obtain stat metadata without following symbolic links.
        if (lstat(fullPath.c_str(), &fileStat) != 0) {
            continue;
        }

        // Skip symbolic links, recurse into directories, or process files.
        if (S_ISLNK(fileStat.st_mode)) {
            continue;
        } else if (S_ISDIR(fileStat.st_mode)) {
            ReadSources(fullPath, filesMap, stats);
        } else if (S_ISREG(fileStat.st_mode)) {
            std::string hash = GetFileHash(fullPath);
            if (!hash.empty()) {
                stats.totalSources++;
                filesMap[hash] = fullPath;
                std::cout << std::format("{:6}\b\b\b\b\b\b",
                                         stats.totalSources)
                          << std::flush;
            }
        }
    }

    closedir(dir);
    stats.totalSourceDirs++;
}
/* ------------------------------------------------------------------------- */
// Traverses destination directory recursively and replaces duplicate files
// safely using atomic renaming.
void WriteDestinations(
    const std::string& currentPath,
    const std::unordered_map<std::string, std::string>& filesMap,
    OperationStats& stats) {
    DIR* dir = opendir(currentPath.c_str());
    if (dir == nullptr) {
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }

        std::string fullPath = currentPath + "/" + name;
        struct stat fileStat;

        if (lstat(fullPath.c_str(), &fileStat) != 0) {
            continue;
        }

        if (S_ISLNK(fileStat.st_mode)) {
            continue;
        } else if (S_ISDIR(fileStat.st_mode)) {
            WriteDestinations(fullPath, filesMap, stats);
        } else if (S_ISREG(fileStat.st_mode)) {
            std::string hash = GetFileHash(fullPath);
            if (!hash.empty()) {
                auto iterator = filesMap.find(hash);
                if (iterator != filesMap.end()) {
                    std::string sourcePath = iterator->second;

                    struct stat sourceStat;
                    if (stat(sourcePath.c_str(), &sourceStat) == 0) {
                        // Check if source and destination are identical.
                        bool isSameFile =
                            (sourcePath == fullPath) ||
                            (sourceStat.st_dev == fileStat.st_dev &&
                             sourceStat.st_ino == fileStat.st_ino);

                        if (!isSameFile && fileStat.st_nlink < 2) {
                            std::string tempLink = fullPath + ".tmp_link";

                            int linkResult = -1;
                            if (stats.useSymlinks) {
                                linkResult = symlink(sourcePath.c_str(),
                                                     tempLink.c_str());
                            } else {
                                linkResult = link(sourcePath.c_str(),
                                                  tempLink.c_str());
                            }

                            if (linkResult == 0) {
                                // Atomically replace destination path.
                                if (rename(tempLink.c_str(),
                                           fullPath.c_str()) == 0) {
                                    stats.totalHardlinked++;
                                    std::cout
                                        << std::format(
                                               "{:6}\b\b\b\b\b\b",
                                               stats.totalHardlinked)
                                        << std::flush;
                                } else {
                                    unlink(tempLink.c_str());
                                }
                            }
                        }
                    }
                }
            }
            stats.totalDests++;
        }
    }

    closedir(dir);
    stats.totalDestDirs++;
}
/* ------------------------------------------------------------------------- */
int main(int argc, char* argv[]) {
    // Validate command line arguments.
    if (argc != 3) {
        std::cerr << std::format("Usage: {} [Source] [Destination]\n",
                                 argv[0]);
        return 1;
    }

    // Verify source directory status.
    struct stat sourceStat;
    if (stat(argv[1], &sourceStat) != 0) {
        std::cerr << std::format("Error: Source directory '{}' not found!\n",
                                 argv[1]);
        return 3;
    }
    if (!S_ISDIR(sourceStat.st_mode)) {
        std::cerr << std::format("Error: Source '{}' is not a directory!\n",
                                 argv[1]);
        return 4;
    }

    // Verify destination directory status.
    struct stat destinationStat;
    if (stat(argv[2], &destinationStat) != 0) {
        std::cerr << std::format(
            "Error: Destination directory '{}' not found!\n", argv[2]);
        return 5;
    }
    if (!S_ISDIR(destinationStat.st_mode)) {
        std::cerr << std::format(
            "Error: Destination '{}' is not a directory!\n", argv[2]);
        return 6;
    }

    OperationStats stats;

    // Check whether source and destination are across different mounts.
    if (sourceStat.st_dev != destinationStat.st_dev) {
        std::cout << "Warning: Symbolic links will be used since source "
                     "and destination are on different mounts...\n";
        std::cout << std::format("* {} -> {}\n", argv[1],
                                 sourceStat.st_dev);
        std::cout << std::format("* {} -> {}\n", argv[2],
                                 destinationStat.st_dev);

        while (true) {
            std::cout << "Continue anyway? [Y/N] ";
            std::string reply;
            std::getline(std::cin, reply);

            if (!reply.empty()) {
                if (reply[0] == 'Y' || reply[0] == 'y') {
                    stats.useSymlinks = true;
                    break;
                }
                if (reply[0] == 'N' || reply[0] == 'n') {
                    return 7;
                }
            }
        }
    }

    // Resolve absolute canonical paths.
    char resolvedSourcePath[PATH_MAX];
    if (realpath(argv[1], resolvedSourcePath) == nullptr) {
        return 10;
    }

    char resolvedDestinationPath[PATH_MAX];
    if (realpath(argv[2], resolvedDestinationPath) == nullptr) {
        return 11;
    }

    std::unordered_map<std::string, std::string> filesMap;

    // Execute source traversal and hashing phase.
    std::cout << std::format("Reading {}...", resolvedSourcePath);
    ReadSources(resolvedSourcePath, filesMap, stats);
    std::cout << std::format(
        " {} unique of {} total files in {} directories.\n",
        filesMap.size(), stats.totalSources, stats.totalSourceDirs);

    // Terminate early if all files are unique.
    if (filesMap.size() == static_cast<size_t>(stats.totalSources)) {
        return 0;
    }

    // Execute destination matching and linking phase.
    std::cout << std::format("Hardlinking {}...", resolvedDestinationPath);
    WriteDestinations(resolvedDestinationPath, filesMap, stats);
    std::cout << std::format(
        " {} hardlinked of {} total files in {} directories.\n",
        stats.totalHardlinked, stats.totalDests, stats.totalDestDirs);

    return 0;
}
/* ========================================================================= */
