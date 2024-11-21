#include "fs.h"

#include <stdint.h>

#include <cstring>
#include <iostream>
#include <vector>

int16_t FS::findDirEntry(uint16_t dirFirstBlock, std::string fileName,
                         std::array<dir_entry, 64>& dirBlock,
                         uint16_t& dirBlockIndex) {
    int fatIndex = dirFirstBlock;
    do {
        this->disk.read(fatIndex, (uint8_t*)dirBlock.data());
        for (uint8_t i = 0; i < 64; i++) {
            if (fileName.compare(0, 56, dirBlock[i].file_name) == 0) {
                dirBlockIndex = i;
                return dirBlock[i].first_blk;
            }
        }
        fatIndex = this->fat[fatIndex];
    } while (fatIndex != FAT_EOF);
    return -1;
}

int FS::getEmptyFat() const {
    for (uint16_t i = 2; i < BLOCK_SIZE / 2; i++)
        if (this->fat[i] == FAT_FREE) return i;
    return -1;
}

FS::FS() { std::cout << "FS::FS()... Creating file system\n"; }

FS::~FS() {}

// formats the disk, i.e., creates an empty file system
int FS::format() {
    this->fat[0] = FAT_EOF;
    this->fat[1] = FAT_EOF;
    for (int i = 2; i < BLOCK_SIZE / 2; i++) this->fat[i] = FAT_FREE;

    // size 64 to make sure we don't go out of scope in disk.write since that
    // function takes a uint8_t* and then indexes 4096 steps into that
    dir_entry directories[64] = {// Root dir metadata
                                 {
                                     .file_name = ".",
                                     .size = 64,
                                     .first_blk = 0,
                                     .type = TYPE_DIR,
                                     .access_rights = READ | WRITE,
                                 }};

    this->disk.write(ROOT_BLOCK, (uint8_t*)&directories);
    this->disk.write(FAT_BLOCK, (uint8_t*)&(this->fat));
    return 0;
}

// create <filepath> creates a new file on the disk, the data content is
// written on the following rows (ended with an empty row)
int FS::create(std::string filepath) {
    std::cout << "FS::create(" << filepath << ")\n";

    // name for either the directory we are passing through or the new file
    std::string fileName;
    fileName.reserve(56);

    // The block of memory that contains the current dir
    std::array<dir_entry, 64> blockContainingDir;

    // load root directory
    this->disk.read(ROOT_BLOCK, (uint8_t*)blockContainingDir.data());
    int blockContainingDirFat = 0;

    // set currentDir to root
    uint16_t currentDirIndex = ROOT_BLOCK;

    std::string data;

    // find the directory that we should put our file in
    uint8_t nameIndex = 0;
    for (size_t i = 0; i < filepath.length(); i++) {
        if (filepath[i] == '/') {
            blockContainingDirFat = this->findDirEntry(
                blockContainingDir[currentDirIndex].first_blk, fileName,
                blockContainingDir, currentDirIndex);
            if (blockContainingDirFat == -1 ||
                blockContainingDir[currentDirIndex].type != TYPE_DIR)
                return -1;  // Dir not found
            fileName = "";
            nameIndex = 0;
        } else if (filepath[i] == '\n') {
            // Aparantly data is encoded in the filePath and we have reached
            // that - "Creates a new file with the name filename on the disk. The
            // data contents are written on the following rows. An empty
            // row ends the user input data. This enables the user to write
            // several lines of input."
            // this is litterally retarded
            data = std::string(filepath.begin() + i, filepath.end() -1); // -1 Might be wrong
            data.reserve(((data.size() / 4096) + 1) * 4096); // to prevent segFault
            break;
        } else {
            if (nameIndex == 56) return -1;  // Invalid filePath or name
            fileName[nameIndex++] = filepath[i];
        }
    }
    if (fileName == "") return -1;  // Invalid name, prob

    // find the fat of the last dirblock
    int fatIndex = blockContainingDir[currentDirIndex].first_blk;
    while (this->fat[fatIndex] != FAT_EOF) fatIndex = this->fat[fatIndex];

    // make sure that the dir block has space, otherwise make a new dirblock
    if ((blockContainingDir[currentDirIndex].size & 4095) == 0) {  // Trust
        int newFatBlock = this->getEmptyFat();
        if (newFatBlock == -1) return -1;  // No space
        this->fat[fatIndex] = newFatBlock;
        fatIndex = newFatBlock;
    }

    // dir block where our new file should go
    std::array<dir_entry, 64> newFileBlockLocation;
    this->disk.read(fatIndex, (uint8_t*)newFileBlockLocation.data());

    // fatindexes for the new file
    int neededPages = (data.size() / BLOCK_SIZE) + 1;
    std::vector<int> newSpace;
    newSpace.reserve(neededPages);
    for (int i = 0; i < neededPages; i++)
    {
        int newFileLocation = this->getEmptyFat();
        if (newFileLocation == -1) {
            return -1;  // no space
        }
        newSpace.emplace_back(newFileLocation);
    }

    // write data
    int i = 0;
    for (; i < neededPages - 1; i++)
    {
        this->disk.write(newSpace[i], (uint8_t*)(data.front() + i * BLOCK_SIZE));
        this->fat[newSpace[i]] = newSpace[i + 1];
    }
    this->disk.write(newSpace[i], (uint8_t*)(data.front() + i * BLOCK_SIZE));
    this->fat[newSpace[i]] = FAT_EOF;


    // meta data for the new file
    dir_entry newEntry{.file_name = fileName.front(),
                       .size = 0,
                       .first_blk = newSpace[0],
                       .type = TYPE_FILE,
                       .access_rights = WRITE | READ};

    // write the meta data for the new file
    newFileBlockLocation[blockContainingDir[currentDirIndex].size & 4095] =
        newEntry;
    this->disk.write(fatIndex, (uint8_t*)&blockContainingDir);

    // update the meta data for the directory in which the file was put
    blockContainingDir[currentDirIndex].size += BLOCK_SIZE;
    this->disk.write(blockContainingDirFat,
                     (uint8_t*)blockContainingDir.data());

    return 0;
}

// cat <filepath> reads the content of a file and prints it on the screen
int FS::cat(std::string filepath) {
    std::cout << "FS::cat(" << filepath << ")\n";
    return 0;
}

// ls lists the content in the currect directory (files and sub-directories)
int FS::ls() {
    int16_t fatIndex = ROOT_BLOCK;
    std::array<dir_entry, 64> dirBlock;
    std::string print("name     size\n");
    this->disk.read(fatIndex, (uint8_t*)dirBlock.data());
    int dirSize = dirBlock[0].size;
    int i = 1;
    while (dirSize != 0) {
        this->disk.read(fatIndex, (uint8_t*)dirBlock.data());
        print.reserve(print.capacity() + 70);
        for (; i < (dirSize & (BLOCK_SIZE - 1)) / 64; i++) {
            print.append(dirBlock[i].file_name, 56);
            print += " " + std::to_string(dirBlock[i].size);
            print += '\n';
        }
        i = 0;
        dirSize -= (dirSize & (BLOCK_SIZE - 1));
        fatIndex = this->fat[fatIndex];
    }
    std::cout << print;
    return 0;
}

// cp <sourcepath> <destpath> makes an exact copy of the file
// <sourcepath> to a new file <destpath>
int FS::cp(std::string sourcepath, std::string destpath) {
    std::cout << "FS::cp(" << sourcepath << "," << destpath << ")\n";
    return 0;
}

// mv <sourcepath> <destpath> renames the file <sourcepath> to the name
// <destpath>, or moves the file <sourcepath> to the directory <destpath> (if
// dest is a directory)
int FS::mv(std::string sourcepath, std::string destpath) {
    std::cout << "FS::mv(" << sourcepath << "," << destpath << ")\n";
    return 0;
}

// rm <filepath> removes / deletes the file <filepath>
int FS::rm(std::string filepath) {
    std::cout << "FS::rm(" << filepath << ")\n";
    return 0;
}

// append <filepath1> <filepath2> appends the contents of file <filepath1> to
// the end of file <filepath2>. The file <filepath1> is unchanged.
int FS::append(std::string filepath1, std::string filepath2) {
    std::cout << "FS::append(" << filepath1 << "," << filepath2 << ")\n";
    return 0;
}

// mkdir <dirpath> creates a new sub-directory with the name <dirpath>
// in the current directory
int FS::mkdir(std::string dirpath) {
    std::cout << "FS::mkdir(" << dirpath << ")\n";
    return 0;
}

// cd <dirpath> changes the current (working) directory to the directory named
// <dirpath>
int FS::cd(std::string dirpath) {
    std::cout << "FS::cd(" << dirpath << ")\n";
    return 0;
}

// pwd prints the full path, i.e., from the root directory, to the current
// directory, including the currect directory name
int FS::pwd() {
    std::cout << "FS::pwd()\n";
    return 0;
}

// chmod <accessrights> <filepath> changes the access rights for the
// file <filepath> to <accessrights>.
int FS::chmod(std::string accessrights, std::string filepath) {
    std::cout << "FS::chmod(" << accessrights << "," << filepath << ")\n";
    return 0;
}
