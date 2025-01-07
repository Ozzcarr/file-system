#include "fs.h"

#include <stdint.h>

#include <cstring>
#include <iostream>
#include <vector>

size_t parsePath(std::string sPath, std::vector<std::string>& path) {
    path.clear();
    if (sPath.size() == 0) return -1;

    std::string fileName = "";

    for (size_t index = 0; index < sPath.size(); index++) {
        if (sPath[index] == '/') {
            path.emplace_back(fileName);
            fileName = "";
        } else if (sPath[index] == '\n') {
            if (fileName.size() > 0) path.emplace_back(fileName);
            return 0;
        } else
            fileName += sPath[index];
    }
    if (fileName.size() > 0) path.emplace_back(fileName);
    return 0;
}

int16_t FS::reserve(size_t size) {
    int neededNodes = (size + (BLOCK_SIZE - 1)) / BLOCK_SIZE;
    std::vector<int> fatNodes;
    int16_t firstNode = this->getEmptyFat();
    if (firstNode == -1) return false;
    fatNodes.emplace_back(firstNode);
    this->fat[firstNode] = FAT_EOF;

    int16_t prevNode = firstNode;
    for (int i = 0; i < (neededNodes - 1); i++) {
        int16_t newNode = this->getEmptyFat();
        if (newNode != -1) {
            this->fat[prevNode] = newNode;
            this->fat[newNode] = FAT_EOF;
            prevNode = newNode;
            fatNodes.emplace_back(newNode);

        } else {
            for (int fatIndex : fatNodes) {
                this->fat[fatIndex] = FAT_FREE;
            }
            return -1;
        }
    }

    return firstNode;
}

void FS::free(int16_t fatStart) {
    int16_t fatIndex = fatStart;
    while (fatIndex != FAT_EOF) {
        int16_t temp = fatIndex;
        fatIndex = this->fat[fatIndex];
        this->fat[temp] = FAT_FREE;
    }
}

bool FS::findDirEntry(dir_entry dir, std::string fileName, dir_entry& result) {
    int fatIndex = dir.first_blk;
    std::array<dir_entry, 64> dirBlock;
    do {
        this->disk.read(fatIndex, (uint8_t*)dirBlock.data());
        for (uint8_t i = 0; i < 64; i++) {
            if (fileName.compare(0, 56, dirBlock[i].file_name) == 0) {
                result = dirBlock[i];
                return true;
            }
        }
        fatIndex = this->fat[fatIndex];
    } while (fatIndex != FAT_EOF);
    return false;
}

bool FS::__cd(dir_entry& workingDir, const std::string& path, bool createDirs) {
    std::vector<std::string> pathv;
    parsePath(path, pathv);
    return this->__cd(workingDir, pathv, createDirs);
}

bool FS::__cd(dir_entry& workingDir, const std::vector<std::string>& path,
              bool createDirs) {
    for (std::string file : path) {
        if (!this->findDirEntry(workingDir, file, workingDir)) {
            if (createDirs) {
                dir_entry newDir{
                    .type = TYPE_DIR,
                    .access_rights = WRITE | READ,
                };
                strncpy(newDir.file_name, file.c_str(), 56);
                if (!this->__create(workingDir, newDir, "")) return false;
                if (!this->findDirEntry(workingDir, file, workingDir))
                    throw std::runtime_error(
                        "Thingy that should not possibly have failed failed, "
                        "How???? Line:" +
                        std::to_string(__LINE__) + " " + "File: " + __FILE__);
            } else
                return false;
        }
        if (!workingDir.type == TYPE_DIR) return false;
    }
    return true;
}

bool FS::__writeData(int16_t startFat, std::string data, int16_t ofset) {
    int16_t fatAvailable = BLOCK_SIZE - ofset;
    int fatIndex = startFat;
    while (this->fat[fatIndex] != FAT_EOF) {
        fatAvailable += BLOCK_SIZE;
        fatIndex = this->fat[fatIndex];
    }

    if (data.length() > fatAvailable) {
        int16_t extraFatStart = this->reserve(data.length() - fatAvailable);
        if (extraFatStart == -1) return false;
        this->fat[fatIndex] = extraFatStart;
    }

    char buffer[4096]{0};
    this->disk.read(startFat, (uint8_t*)buffer);
    for (int i = ofset; i < BLOCK_SIZE; i++) buffer[i] = 0;

    size_t pos = 0;
    data.copy(&buffer[ofset], BLOCK_SIZE - ofset, pos);

    for (int i = 0; i < BLOCK_SIZE; i++) std::cout << buffer[i];
    std::cout << "\n";

    pos += BLOCK_SIZE - ofset;

    fatIndex = startFat;
    this->disk.write(fatIndex, (uint8_t*)buffer);

    fatIndex = this->fat[fatIndex];
    while (pos < data.length()) {
        data.copy(buffer, BLOCK_SIZE, pos);
        this->disk.write(fatIndex, (uint8_t*)buffer);
        pos += BLOCK_SIZE;
    }
    return true;
}

bool FS::addDirEntry(dir_entry dir, dir_entry newEntry) {
    dir_entry temp;
    if (this->findDirEntry(dir, std::string(newEntry.file_name), temp))
        return false;

    int16_t fatIndex = dir.first_blk;
    std::array<dir_entry, 64> dirBlock;

    while (this->fat[fatIndex] != FAT_EOF) {
        fatIndex = this->fat[fatIndex];
    }

    int16_t dirEntryIndexInBlock = (dir.size & (BLOCK_SIZE - 1)) / 64;

    // if we don't have enough space in our directory for the dir_entry
    if (dirEntryIndexInBlock ==
        0) {  // I would say trust but don't the magicc might no be magiccing
        int16_t newDirBlock = this->getEmptyFat();
        if (newDirBlock == -1) {
            return false;
        }
        this->fat[fatIndex] = newDirBlock;
        fatIndex = this->fat[fatIndex];
        this->fat[fatIndex] = FAT_EOF;
    }

    // add metadata for new file
    this->disk.read(fatIndex, (uint8_t*)dirBlock.data());
    dirBlock[dirEntryIndexInBlock] = newEntry;
    this->disk.write(fatIndex, (uint8_t*)dirBlock.data());

    // std::cout << fatIndex << " " << dir.first_blk << " " <<
    // dirEntryIndexInBlock << " - - - -\n";

    // update metadata for directory
    this->disk.read(dir.first_blk, (uint8_t*)dirBlock.data());
    dirBlock[0].size += 64;
    if (this->workingDir.first_blk == dirBlock[0].first_blk)
        this->workingDir = dirBlock[0];
    this->disk.write(dir.first_blk, (uint8_t*)dirBlock.data());

    return true;
}

bool FS::__create(const dir_entry dir, dir_entry& metadata, std::string data) {
    // reserve space that the new file needs
    int16_t startfat = this->reserve(data.length());
    if (startfat == -1) return false;

    // set metadata for the file
    metadata.first_blk = startfat;
    metadata.size = data.length();
    if (metadata.type == TYPE_DIR) metadata.size += 128;

    if( !this->addDirEntry(dir, metadata) ) return -1;

    // write the data to the new file
    std::string extra;
    if (metadata.type == TYPE_DIR) {
        for (int i = 0; i < 128; i++) extra += (char)0;
        extra[0] = '.';
        extra[64] = '.';
        extra[65] = '.';
        for (int i = 0; i < 64 - 56; i++) {
            extra[i + 56] = ((char*)&metadata)[i + 56];
            extra[64 + i + 56] = ((char*)&dir)[i + 56];
        }
    }

    this->__writeData(startfat, std::string(extra + data), 0);

    return true;
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
                                     .size = 128,
                                     .first_blk = 0,
                                     .type = TYPE_DIR,
                                     .access_rights = READ | WRITE,
                                 },
                                 {
                                     .file_name = "..",
                                     .size = 128,
                                     .first_blk = 0,
                                     .type = TYPE_DIR,
                                     .access_rights = READ | WRITE,
                                 }};

    this->disk.write(ROOT_BLOCK, (uint8_t*)directories);
    this->disk.write(FAT_BLOCK, (uint8_t*)(this->fat));
    this->workingDir = directories[0];
    return 0;
}

// create <filepath> creates a new file on the disk, the data content is
// written on the following rows (ended with an empty row)
int FS::create(std::string filepath) {
    std::cout << "FS::create(" << filepath << ")\n";

    std::vector<std::string> path;
    size_t dataSize = parsePath(filepath, path);
    dir_entry currentDir = this->workingDir;

    if (path.size() == 0) return -1;

    std::vector<std::string> some(path.begin(), path.end() - 1);

    this->__cd(currentDir,
               std::vector<std::string>(path.begin(), path.end() - 1));

    dir_entry newFile{
        .type = TYPE_FILE,
        .access_rights = READ | WRITE,
    };

    strncpy(newFile.file_name, path.back().c_str(), 56);

    std::string data;
    std::getline(std::cin, data);

    if (!this->__create(workingDir, newFile, data)) return -1;

    std::array<dir_entry, 64> dirblock;
    this->disk.read(workingDir.first_blk, (uint8_t*)dirblock.data());
    this->workingDir = dirblock[0];

    return 0;

    // std::cout << path << "\n" << data << "\n" << dataSize << "\n";
}

// cat <filepath> reads the content of a file and prints it on the screen
int FS::cat(std::string filepath) {
    dir_entry dir = workingDir;
    std::vector<std::string> path;
    parsePath(filepath, path);
    if (path.size() == 0) return -1;
    if (path.size() > 1) {
        if (!__cd(dir, std::vector<std::string>(path.begin(), path.end() - 1)))
            return -1;
    }
    dir_entry file;
    if (!this->findDirEntry(dir, path.back(), file)) return -1;
    int16_t nextFat = file.first_blk;
    char dirBlock[BLOCK_SIZE];
    std::string print;

    // Go through each full dirBlock

    for (int i = 0; i < (workingDir.size / BLOCK_SIZE); i++) {
        if (nextFat == FAT_EOF)
            throw std::runtime_error("some shite went wrong");

        this->disk.read(nextFat, (uint8_t*)dirBlock);
        for (char c : dirBlock) {
            print += c;
        }
        nextFat = this->fat[nextFat];
    }

    // Go through the direntries in a non full dirblock
    size_t rest = (workingDir.size & (BLOCK_SIZE - 1));
    if (rest) {
        if (nextFat == FAT_EOF)
            throw std::runtime_error("some shite went wrong");
        this->disk.read(nextFat, (uint8_t*)dirBlock);
        for (int i = 0; i < rest; i++) {
            print += dirBlock[i];
        }
    }
    print += "\n";

    std::cout << print;

    return 0;
}


void FS::__processDirBlock(int16_t fatIndex, size_t count, std::string& output) {
    std::array<dir_entry, 64> dirBlock;
    this->disk.read(fatIndex, (uint8_t*)dirBlock.data());
    for (size_t i = 0; i < count; i++) {
        // Print if not hidden file
        if (dirBlock[i].file_name[0] != '.')
            output += std::string(dirBlock[i].file_name) + "  " +
                    std::to_string(dirBlock[i].size) + "\n";
    }
}

// ls lists the content in the current directory (files and sub-directories)
int FS::ls() {
    int16_t nextFat = this->workingDir.first_blk;
    std::string print("name\t size\n");

    // Process each full dirBlock
    for (int i = 0; i < (workingDir.size / BLOCK_SIZE); i++) {
        if (nextFat == FAT_EOF)
            throw std::runtime_error("some shite went wrong");

        FS::__processDirBlock(nextFat, 64, print);
        nextFat = this->fat[nextFat];
    }

    // Process the remaining entries in a non-full dirBlock
    size_t rest = (workingDir.size & (BLOCK_SIZE - 1)) / 64;
    if (rest) {
        if (nextFat == FAT_EOF)
            throw std::runtime_error("some shite went wrong");

        FS:__processDirBlock(nextFat, rest, print);
    }

    std::cout << print;

    return 0;
}

// cp <sourcepath> <destpath> makes an exact copy of the file
// <sourcepath> to a new file <destpath>
int FS::cp(std::string sourcepath, std::string destpath) {
    // work in progress
    dir_entry src = this->workingDir;
    dir_entry dest = this->workingDir;
    std::vector<std::string> path;
    parsePath(sourcepath, path);
    if (path.size() == 0) return -1;

    // find src dir
    if (path.size() > 1 && !this->__cd(src, std::vector<std::string>(
                                                path.begin(), path.end() - 1)))
        return -1;
    // find src
    if (!this->findDirEntry(src, path.back(), src)) return -1;

    path.clear();
    parsePath(destpath, path);

    if (path.size() > 1 && !this->__cd(dest, std::vector<std::string>(
                                            path.begin(), path.end() - 1)))
    return -1;

    dir_entry filecpy = src;

    if(!this->findDirEntry(dest, path.back(), dest)) {
        path.back().copy(filecpy.file_name, 56);
    }
    else {
        if (dest.type == TYPE_FILE) return -1;
    }

    int16_t newFats = this->reserve(src.size);
    if(newFats == -1) return -1;
    filecpy.first_blk = newFats;

    if(!this->addDirEntry(dest, filecpy)) return -1;

    int16_t nextSrcFat = src.first_blk;
    int16_t nextTargetFat = filecpy.first_blk;
    while (nextSrcFat != FAT_EOF) {
        uint8_t block[BLOCK_SIZE]{0};
        this->disk.read(nextSrcFat, block);
        this->disk.write(nextTargetFat, block);
        nextSrcFat = this->fat[nextSrcFat];
        nextTargetFat = this->fat[nextTargetFat];
    }

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
    dir_entry newWorkingDir = this->workingDir;
    if (!this->__cd(newWorkingDir, dirpath)) return -1;
    this->workingDir = newWorkingDir;
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
