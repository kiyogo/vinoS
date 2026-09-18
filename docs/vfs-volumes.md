# KOS VFS Volumes and Filesystems

## Overview

KOS organizes storage devices and file systems using a modular Virtual File System with Windows style drive letter mapping.

## Architecture

- Volume Manager: Maps detected partitions and disk images to drive letters such as C and D.
- Block Cache: LRU cache maintaining recently accessed disk blocks in memory.
- FAT32 Engine: Complete filesystem implementation for reading and writing files and directories.
- Initramfs and RAMFS: In-memory temporary filesystems for early boot and fast runtime data.
- Path Resolution: Transparently resolves paths containing forward slashes or Windows style backward slashes.
