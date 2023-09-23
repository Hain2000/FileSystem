// fs.cpp: File System

#include "sfs/fs.h"

#include <algorithm>
#include <string>
#include <stdio.h>
#include <string.h>
#include <vector>
#include <cmath>

// Debug file system -----------------------------------------------------------

void FileSystem::debug(Disk *disk) {
    Block block;

    // Read Superblock
    disk->read(0, block.Data);

    printf("SuperBlock:\n");
    if (block.Super.MagicNumber == MAGIC_NUMBER) {
        printf("    magic number is valid\n");
    } else {
        printf("    magic number is invalid\n");
    }
    printf("    %u blocks\n"         , block.Super.Blocks);
    printf("    %u inode blocks\n"   , block.Super.InodeBlocks);
    printf("    %u inodes\n"         , block.Super.Inodes);

    // Read Inode blocks
    // 从超级块取出inode_block的个数
    std::vector<uint32_t> direct_blocks, indirect_data_blocks;
    uint32_t inode_block_num = block.Super.InodeBlocks;
    for (uint32_t k = 1; k <= inode_block_num; k++) {
        disk->read(k, block.Data);
        for (uint32_t i = 0; i < INODES_PER_BLOCK; i++) {
            if (block.Inodes[i].Valid == 0) {
                continue;
            }
            for (uint32_t j = 0; j < POINTERS_PER_INODE; j++) {
                if (block.Inodes[i].Direct[j]) {
                    direct_blocks.emplace_back(block.Inodes[i].Direct[j]);
                }
            }
            uint32_t indirect_ptr = block.Inodes[i].Indirect;
            if (indirect_ptr) {
                Block indirect_block;
                disk->read(indirect_ptr, indirect_block.Data);
                for (uint32_t u = 0; u < POINTERS_PER_BLOCK; u++) {
                    if (indirect_block.Pointers[u]) {
                        indirect_data_blocks.emplace_back(indirect_block.Pointers[u]);
                    }
                }
            }
            printf("Inode %u:\n", i);
            printf("    size: %u bytes\n"    , block.Inodes[i].Size);
            printf("    direct blocks:");
            for (auto dres : direct_blocks) {
                printf(" %u", dres);
            }
            printf("\n");
            if (indirect_data_blocks.size()) {
                printf("    indirect block: %u\n", indirect_ptr);
                printf("    indirect data blocks:");
                for (auto ires : indirect_data_blocks) {
                    printf(" %u", ires);
                }
                printf("\n");
            }
        }
    }
}

// Format file system ----------------------------------------------------------

bool FileSystem::format(Disk *disk) {
    if (disk->mounted()) {
        return false;
    }
    // Write superblock
    Block block;
    memset(block.Data, 0, disk->BLOCK_SIZE);
    block.Super.MagicNumber = MAGIC_NUMBER;
    block.Super.Blocks = disk->size();
    block.Super.InodeBlocks = static_cast<uint32_t>(::ceil(disk->size() / 10));
    block.Super.Inodes = INODES_PER_BLOCK * block.Super.InodeBlocks;
    disk->write(0, block.Data);
    // Clear all other blocks
    char buf[BUFSIZ] = {0};
    for (uint32_t i = 1; i < block.Super.Blocks; i++) {
        disk->write(i, buf);
    }
    return true;
}


// Mount file system -----------------------------------------------------------

bool FileSystem::mount(Disk *disk) {
    if (disk->mounted()) {
        return false;
    }
    // Read superblock
    Block block;
    disk->read(0, block.Data);
    if (block.Super.MagicNumber != MAGIC_NUMBER
    || block.Super.Inodes != block.Super.InodeBlocks * INODES_PER_BLOCK
    || block.Super.Blocks < 0) {
        return false;
    }

    if (disk->size() % 10 == 0) {
        if (block.Super.InodeBlocks != disk->size() / 10) {
            return false;
        }
    } else {
        if (block.Super.InodeBlocks != disk->size() / 10 + 1) {
            return false;
        }
    }

    // Set device and mount
    disk->mount();
    // Copy metadata
    inodes_num_ = block.Super.Inodes;
    blocks_num_ = block.Super.Blocks;
    inode_blocks_num_ = block.Super.InodeBlocks;
    disk_ = disk;
    // Allocate free block bitmap
    bitmap_.resize(blocks_num_, 1);
    bitmap_[0] = 0;
    for (uint32_t i = 1; i <= inode_blocks_num_; i++) {
        bitmap_[i] = 0;
    }
    for (uint32_t k = 1; k <= inode_blocks_num_; k++) {
        Block inode_block;
        disk->read(k, inode_block.Data);
        for (uint32_t i = 0; i < INODES_PER_BLOCK; i++) {
            if (!inode_block.Inodes[i].Valid) {
                continue;
            }
            for (uint32_t j = 0; j < POINTERS_PER_INODE; j++) {
                bitmap_[inode_block.Inodes[i].Direct[j]] = 0;
            }
            if (inode_block.Inodes[i].Indirect) {
                bitmap_[inode_block.Inodes[i].Indirect] = 0;
                Block ptr_block;
                disk->read(inode_block.Inodes[i].Indirect, ptr_block.Data);
                for (uint32_t j = 0; j < POINTERS_PER_BLOCK; j++) {
                    bitmap_[ptr_block.Pointers[j]] = 0;
                }
            }
        }
    }

    return true;
}


// Create inode ----------------------------------------------------------------

ssize_t FileSystem::create() {
    // Locate free inode in inode table
    int inumber = -1;
    for (uint32_t k = 1; k <= inode_blocks_num_; k++) {
        Block block;
        disk_->read(k, block.Data);
        for (uint32_t i = 0; i < INODES_PER_BLOCK; i++) {
            if (!block.Inodes[i].Valid) {
                make_inode(block.Inodes[i], true);
                disk_->write(k, block.Data);
                inumber = i + INODES_PER_BLOCK * (k - 1);
                break;
            }
        }
        if (inumber != -1) {
            break;
        }
    }

    // Record inode if found


    return inumber;
}


// Remove inode ----------------------------------------------------------------

bool FileSystem::remove(size_t inumber) {
    // Load inode information
    Inode inode;
    if (!load_inode(&inode, inumber)) {
        return false;
    }
    if (!inode.Valid) {
        return false;
    }
    // Free direct blocks
    for (uint32_t i = 0; i < POINTERS_PER_INODE; i++) {
        bitmap_[inode.Direct[i]] = 1;
        inode.Direct[i] = 0;
    }
    // Free indirect blocks
    if (inode.Indirect) {
        Block ptr_block;
        bitmap_[inode.Indirect] = 1;
        disk_->read(inode.Indirect, ptr_block.Data);
        for (uint32_t i = 0; i < POINTERS_PER_BLOCK; i++) {
            bitmap_[ptr_block.Pointers[i]] = 1;
            ptr_block.Pointers[i] = 0;
        }
    }
    // Clear inode in inode table
    inode.Indirect = 0;
    inode.Valid = 0;
    inode.Size = 0;
    if (!save_inode(&inode, inumber)) {
        return false;
    }
    return true;
}


// Inode stat ------------------------------------------------------------------

ssize_t FileSystem::stat(size_t inumber) {
    // Load inode information
    Inode inode;
    if (!load_inode(&inode, inumber)) {
        return -1;
    }
    if (!inode.Valid) {
        return -1;
    }
    return inode.Size;
}


// Read from inode -------------------------------------------------------------

ssize_t FileSystem::read(size_t inumber, char *data, size_t length, size_t offset) {
    // Load inode information
    Inode inode;
    if (!load_inode(&inode, inumber)) {
        return -1;
    }
    if (offset > inode.Size) {
        return -1;
    }
    // Adjust length
    length = std::min(length, inode.Size - offset);
    Block ptr_block;
    // 直接块不够，去间接块
    if ((offset + length) / disk_->BLOCK_SIZE > POINTERS_PER_INODE) {
        if (!inode.Indirect) {
            return -1;
        }
        disk_->read(inode.Indirect, ptr_block.Data);
    }
    size_t idx = 0;
    for (uint32_t i = offset / disk_->size(); idx < length; i++) {
        uint32_t cur_block_i;
        if (i < POINTERS_PER_INODE) {
            cur_block_i = inode.Direct[i];
        } else {
            cur_block_i = ptr_block.Pointers[i - POINTERS_PER_INODE];
        }

        if (cur_block_i == 0) {
            return -1;
        }

        Block new_block;
        disk_->read(cur_block_i, new_block.Data);
        size_t pos;
        size_t cpy_len;

        if (idx == 0) {
            pos = offset % disk_->BLOCK_SIZE;
            cpy_len = std::min(disk_->BLOCK_SIZE - pos, length);
        } else {
            pos = 0;
            cpy_len = std::min(disk_->BLOCK_SIZE - pos, length - idx);
        }
        memcpy(data + idx, new_block.Data + pos, cpy_len);
        idx += cpy_len;
    }

    return idx;
}


// Write to inode --------------------------------------------------------------


ssize_t FileSystem::write(size_t inumber, char *data, size_t length, size_t offset) {

    // Load inode
    Inode inode;
    if (!load_inode(&inode, inumber)) {
        return -1;
    }
    if (offset > inode.Size) {
        return -1;
    }
    size_t MAX_SIZE = disk_->BLOCK_SIZE * (POINTERS_PER_INODE + POINTERS_PER_BLOCK);
    length = std::min(length, MAX_SIZE - offset);
    // Write block and copy to data
    size_t idx = 0;
    Block ptr_block;
    bool read_indirect = false;
    bool modify_ptr_block = false;
    bool modify_inode = false;
    for (uint32_t block_i = offset / disk_->BLOCK_SIZE; block_i < POINTERS_PER_INODE + POINTERS_PER_BLOCK && idx < length; block_i++) {
        ssize_t cur_block_i;
        if (block_i < POINTERS_PER_INODE) {
            if (inode.Direct[block_i] == 0) {
                ssize_t new_block_i = allocate_free_block();
                if (new_block_i == -1) {
                    break;
                }
                inode.Direct[block_i] = new_block_i;
                modify_inode = true;
            }
            cur_block_i = inode.Direct[block_i];
        } else {
            if (!inode.Indirect) {
                ssize_t ptr_block_i = allocate_free_block();
                if (ptr_block_i == -1) {
                    return idx;
                }
                inode.Indirect = ptr_block_i;
                modify_ptr_block = true;
            }

            if (!read_indirect) {
                disk_->read(inode.Indirect, ptr_block.Data);
                read_indirect = true;
            }

            if (ptr_block.Pointers[block_i - POINTERS_PER_INODE] == 0) {
                ssize_t new_block_i = allocate_free_block();
                if (new_block_i == -1) {
                    break;
                }
                ptr_block.Pointers[block_i - POINTERS_PER_INODE] = new_block_i;
                modify_ptr_block = true;
            }
            cur_block_i = ptr_block.Pointers[block_i - POINTERS_PER_INODE];
        }

        char buf[disk_->BLOCK_SIZE];
        size_t pos;
        size_t cpy_len;

        if (idx == 0) {
            pos = offset % disk_->BLOCK_SIZE;
            cpy_len = std::min(disk_->BLOCK_SIZE - pos, length);
        } else {
            pos = 0;
            cpy_len = std::min(disk_->BLOCK_SIZE - pos, length - idx);
        }

        if (cpy_len < disk_->BLOCK_SIZE) {
            disk_->read(cur_block_i, buf);
        }
        memcpy(buf + pos, data + idx, cpy_len);
        disk_->write(cur_block_i, buf);
        idx += cpy_len;
    }

    if (idx + offset > (size_t)inode.Size) {
        inode.Size = idx + offset;
        modify_inode = true;
    }
    if (modify_inode) {
        save_inode(&inode, inumber);
    }
    if (modify_ptr_block) {
        disk_->write(inode.Indirect, ptr_block.Data);
    }
    return idx;
}



bool FileSystem::load_inode(FileSystem::Inode *inode, size_t inumber) {
    uint32_t cur_block_i = (inumber / INODES_PER_BLOCK) + 1;
    if (inumber >= inodes_num_) {
        return false;
    }
    Block new_block;
    disk_->read(cur_block_i, new_block.Data);
    *inode = new_block.Inodes[inumber % INODES_PER_BLOCK];
    return true;
}

void FileSystem::make_inode(FileSystem::Inode &inode, bool valid) {
    inode.Valid = valid;
    inode.Size = 0;
    inode.Indirect = 0;
    for (uint32_t i = 0; i < POINTERS_PER_INODE; i++) {
        inode.Direct[i] = 0;
    }
}

bool FileSystem::save_inode(FileSystem::Inode *inode, size_t inumber) {
    uint32_t cur_block_i = (inumber / INODES_PER_BLOCK) + 1;
    if (inumber >= inodes_num_) {
        return false;
    }
    Block new_block;
    disk_->read(cur_block_i, new_block.Data);
    new_block.Inodes[inumber % INODES_PER_BLOCK] = *inode;
    disk_->write(cur_block_i, new_block.Data);
    return true;
}

ssize_t FileSystem::allocate_free_block() {
    int block = -1;
    for (uint32_t i = 0; i < blocks_num_; i++) {
        if (bitmap_[i]) {
            bitmap_[i] = 0;
            block = i;
            break;
        }
    }
    if (block != -1) {
        char data[disk_->BLOCK_SIZE];
        memset(data, 0, disk_->BLOCK_SIZE);
        disk_->write(block, data);
    }
    return block;
}