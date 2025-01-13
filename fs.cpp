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

inline bool validEntry(const dir_entry& dir) { return dir.file_name[0] != 0; }

bool FS::findDirEntry(dir_entry dir, std::string fileName, dir_entry& result,
                      int16_t& fatIndex, int& blockIndex) {
    fatIndex = dir.first_blk;
    std::array<dir_entry, 64> dirBlock;

    while (fatIndex != FAT_EOF) {
        this->disk.read(fatIndex, (uint8_t*)dirBlock.data());
        for (blockIndex = 0; blockIndex < 64; blockIndex++) {
            if (!validEntry(dirBlock[blockIndex])) return false;
            if (fileName.compare(0, 56, dirBlock[blockIndex].file_name) == 0) {
                result = dirBlock[blockIndex];
                return true;
            }
        }
    }

    return false;
}

bool FS::findDirEntry(dir_entry dir, std::string fileName, dir_entry& result) {
    int16_t fatIndex;
    int blockIndex;
    return findDirEntry(dir, fileName, result, fatIndex, blockIndex);
}

bool FS::__cd(dir_entry& workingDir, const std::string& path, bool createDirs) {
    std::vector<std::string> pathv;
    parsePath(path, pathv);
    return this->__cd(workingDir, pathv, createDirs);
}

bool FS::__cd(dir_entry& workingDir, const std::vector<std::string>& path,
              bool createDirs) {
    if (!this->__cdToFile(workingDir, path, createDirs)) return false;
    if (workingDir.type == TYPE_FILE) return false;
    return true;
}

bool FS::__cdToFile(dir_entry& workingDir, const std::string& path,
                    bool createDirs) {
    std::vector<std::string> pathv;
    parsePath(path, pathv);
    return this->__cdToFile(workingDir, pathv, createDirs);
}

bool FS::__cdToFile(dir_entry& workingDir, const std::vector<std::string>& path,
                    bool createDirs) {
    for (std::string file : path) {
        if (!workingDir.type == TYPE_DIR) return false;
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
    }
    return true;
}

bool FS::__writeData(int16_t startFat, std::string data,
                     int16_t offset) {  // behöver ändras
    int16_t fatAvailable = BLOCK_SIZE - offset;
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
    for (int i = offset; i < BLOCK_SIZE; i++) buffer[i] = 0;

    size_t pos = 0;
    data.copy(&buffer[offset], BLOCK_SIZE - offset, pos);

    pos += BLOCK_SIZE - offset;

    fatIndex = startFat;
    this->disk.write(fatIndex, (uint8_t*)buffer);

    fatIndex = this->fat[fatIndex];
    while (pos < data.length()) {
        data.copy(buffer, BLOCK_SIZE, pos);
        this->disk.write(fatIndex, (uint8_t*)buffer);
        pos += BLOCK_SIZE;
    }
    this->disk.write(1, (uint8_t*)this->fat);
    return true;
}

bool FS::addDirEntry(dir_entry& dir,
                     dir_entry newEntry) {  // Onödig skit i denna
    dir_entry temp;
    if (this->findDirEntry(dir, std::string(newEntry.file_name), temp)) {
        return false;
    }

    int16_t fatIndex = dir.first_blk;

    while (this->fat[fatIndex] != FAT_EOF) {
        fatIndex = this->fat[fatIndex];
    }

    std::array<dir_entry, 64> dirBlock;
    this->disk.read(fatIndex, (uint8_t*)dirBlock.data());
    int dirEntryIndexInBlock;
    for (dirEntryIndexInBlock = 0; dirEntryIndexInBlock < 64;
         dirEntryIndexInBlock++) {
        if (!validEntry(dirBlock[dirEntryIndexInBlock])) break;
    }

    // if we don't have enough space in our directory for the dir_entry
    std::cout << dirEntryIndexInBlock << "\n";
    if (dirEntryIndexInBlock ==
        64) {  // I would say trust but don't the magicc might no be magiccing

        dirEntryIndexInBlock = 0;
        int16_t newDirBlock = this->getEmptyFat();
        if (newDirBlock == -1) {
            return false;
        }
        this->fat[fatIndex] = newDirBlock;
        fatIndex = this->fat[fatIndex];
        this->fat[fatIndex] = FAT_EOF;

        // add metadata for new file
        std::array<dir_entry, 64> freshDirBlock;
        freshDirBlock[dirEntryIndexInBlock] = newEntry;
        this->disk.write(fatIndex, (uint8_t*)freshDirBlock.data());

    } else {
        // add metadata for new file
        dirBlock[dirEntryIndexInBlock] = newEntry;
        this->disk.write(fatIndex, (uint8_t*)dirBlock.data());
    }

    this->disk.write(1, (uint8_t*)this->fat);
    return true;
}

bool FS::removeDirEntry(dir_entry& dir, std::string fileName) {  // Onödig skit
    int16_t removeBlockIndex;
    int removeDirEntryIndex;
    dir_entry entryToRemove;
    if (!this->findDirEntry(dir, std::string(fileName), entryToRemove,
                            removeBlockIndex, removeDirEntryIndex))
        return false;

    int16_t fatIndex = dir.first_blk;
    while (this->fat[fatIndex] != FAT_EOF) {
        fatIndex = this->fat[fatIndex];
    }

    std::array<dir_entry, 64> dirBlock;
    this->disk.read(fatIndex, (uint8_t*)dirBlock.data());

    int16_t dirEntryIndexInBlock;
    for (dirEntryIndexInBlock = 0; dirEntryIndexInBlock < 64;
         dirEntryIndexInBlock++) {
        if (!validEntry(dirBlock[dirEntryIndexInBlock])) break;
    }
    dirEntryIndexInBlock--;

    // Move last data to space where we remove
    dir_entry entryToMove = dirBlock[dirEntryIndexInBlock];
    dirBlock[dirEntryIndexInBlock].file_name[0] = 0;
    this->disk.write(fatIndex, (uint8_t*)dirBlock.data());

    if (dirEntryIndexInBlock != removeDirEntryIndex ||
        fatIndex != removeBlockIndex) {
        this->disk.read(removeBlockIndex, (uint8_t*)dirBlock.data());
        dirBlock[removeDirEntryIndex] = entryToMove;
        this->disk.write(removeBlockIndex, (uint8_t*)dirBlock.data());
    }

    // Update EOF
    if (dirEntryIndexInBlock == 0) {
        this->fat[fatIndex] = FAT_FREE;
        while (this->fat[this->fat[fatIndex]] != FAT_FREE) {
            fatIndex = this->fat[fatIndex];
        }
        this->fat[fatIndex] = FAT_EOF;
    }

    this->disk.write(1, (uint8_t*)this->fat);
    return true;
}

bool FS::__create(dir_entry dir, dir_entry& metadata, std::string data) {
    // reserve space that the new file needs
    int16_t startfat = this->reserve(data.length());
    if (startfat == -1) return false;

    // set metadata for the file
    metadata.first_blk = startfat;
    metadata.size = data.length();
    if (metadata.type == TYPE_DIR) metadata.size += 128;

    if (!this->addDirEntry(dir, metadata)) return false;

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
    this->disk.write(1, (uint8_t*)this->fat);
    return true;
}

int FS::getEmptyFat() const {
    for (uint16_t i = 2; i < BLOCK_SIZE / 2; i++)
        if (this->fat[i] == FAT_FREE) return i;
    return -1;
}

FS::FS() {
    this->disk.read(FAT_BLOCK, (uint8_t*)this->fat);
    this->workingDir = {
                                     .file_name = ".",
                                     .size = 0,
                                     .first_blk = 0,
                                     .type = TYPE_DIR,
                                     .access_rights = READ | WRITE,
                                 };
}

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
                                     .size = 0,
                                     .first_blk = 0,
                                     .type = TYPE_DIR,
                                     .access_rights = READ | WRITE,
                                 },
                                 {
                                     .file_name = "..",
                                     .size = 0,
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

    if (path.back().size() > 55) return -1;
    path.back().copy(newFile.file_name, 56);

    std::string data, line;
    while (std::getline(std::cin, line) && !line.empty()) {
        data += line + "\n";
    }

    if (!this->__create(workingDir, newFile, data)) return -1;

    std::array<dir_entry, 64> dirblock;
    this->disk.read(workingDir.first_blk, (uint8_t*)dirblock.data());
    this->workingDir = dirblock[0];

    return 0;
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

void FS::__processDirBlock(int16_t fatIndex, size_t count,
                           std::string& output) {  // behöver ändras
    std::array<dir_entry, 64> dirBlock;
    this->disk.read(fatIndex, (uint8_t*)dirBlock.data());
    for (size_t i = 0; i < count; i++) {
        // Print if not hidden file
        if (dirBlock[i].file_name[0] != '.' && validEntry(dirBlock[i])) {
            std::string type = dirBlock[i].type ? "dir" : "file";
            output +=
                std::string(dirBlock[i].file_name) + "\t " + type + "\t " +
                (!dirBlock[i].type ? std::to_string(dirBlock[i].size) : "----") +
                "\n";
        }
    }
}

// ls lists the content in the current directory (files and sub-directories)
int FS::ls() {  // behöver ändras
    int16_t nextFat = this->workingDir.first_blk;
    std::string print("name\t type\t size\n");

    while (nextFat != FAT_EOF) {
        this->__processDirBlock(nextFat, 64, print);
        nextFat = this->fat[nextFat];
    }

    std::cout << print;

    return 0;

    // Process each full dirBlock
    for (int i = 0; i < (workingDir.size / BLOCK_SIZE); i++) {
        if (nextFat == FAT_EOF)
            throw std::runtime_error("some shite went wrong");

        this->__processDirBlock(nextFat, 64, print);
        nextFat = this->fat[nextFat];
    }

    // Process the remaining entries in a non-full dirBlock
    size_t rest = (workingDir.size & (BLOCK_SIZE - 1)) / 64;
    if (rest) {
        if (nextFat == FAT_EOF)
            throw std::runtime_error("some shite went wrong");

        this->__processDirBlock(nextFat, rest, print);
    }

    std::cout << print;

    return 0;
}

// cp <sourcepath> <destpath> makes an exact copy of the file
// <sourcepath> to a new file <destpath>
int FS::cp(std::string sourcepath, std::string destpath) {
    std::cout << sourcepath << " " << destpath << "\n";
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

    path.empty();
    parsePath(destpath, path);

    if (path.size() > 1 && !this->__cd(dest, std::vector<std::string>(
                                                 path.begin(), path.end() - 1)))
        return -1;

    dir_entry filecpy = src;

    if (!this->findDirEntry(dest, path.back(), dest)) {
        for (int i = 0; i < 56; i++) filecpy.file_name[i] = 0;
        path.back().copy(filecpy.file_name, 56);
    } else {
        if (dest.type == TYPE_FILE) return -1;
    }

    int16_t newFats = this->reserve(src.size);
    if (newFats == -1) return -1;
    filecpy.first_blk = newFats;

    if (!this->addDirEntry(dest, filecpy)) return -1;

    int16_t nextSrcFat = src.first_blk;
    int16_t nextTargetFat = filecpy.first_blk;
    while (nextSrcFat != FAT_EOF) {
        uint8_t block[BLOCK_SIZE]{0};
        this->disk.read(nextSrcFat, block);
        this->disk.write(nextTargetFat, block);
        nextSrcFat = this->fat[nextSrcFat];
        nextTargetFat = this->fat[nextTargetFat];
    }
    this->disk.write(1, (uint8_t*)this->fat);

    return 0;
}

// mv <sourcepath> <destpath> renames the file <sourcepath> to the name
// <destpath>, or moves the file <sourcepath> to the directory <destpath> (if
// dest is a directory)
int FS::mv(std::string sourcepath, std::string destpath) {
    dir_entry dir = this->workingDir;
    std::vector<std::string> path;
    parsePath(sourcepath, path);
    for (std::string p : path) std::cout << p << "\n";
    dir_entry file;

    // find src dir
    if (path.size() > 1 && !this->__cd(dir, std::vector<std::string>(
                                                path.begin(), path.end() - 1)))
        return -1;
    // find src
    if (!this->findDirEntry(dir, path.back(), file)) return -2;

    dir_entry fileCopy = file;
    path.clear();
    parsePath(destpath, path);
    dir_entry targetDir = this->workingDir;
    if (path.size() > 1 &&
        !this->__cd(targetDir,
                    std::vector<std::string>(path.begin(), path.end() - 1)))
        return -3;

    dir_entry temp;
    if (this->findDirEntry(targetDir, path.back(), temp)) {
        if (temp.type == TYPE_DIR) {
            targetDir = temp;
            std::cout << targetDir.first_blk << "\n";
        } else {
            return -4;
        }
    } else {
        path.back().copy(fileCopy.file_name, 56);
    }

    std::array<dir_entry, 64> dirBlock;
    this->disk.read(fileCopy.first_blk, (uint8_t*)dirBlock.data());
    dirBlock[1] = targetDir;
    std::string("..").copy(dirBlock[1].file_name, 56);
    this->disk.write(fileCopy.first_blk, (uint8_t*)dirBlock.data());

    std::cout << "File Copy File Name: " << fileCopy.file_name << "\n";
    if (!this->addDirEntry(targetDir, fileCopy)) return -5;
    if (dir.first_blk == targetDir.first_blk) dir.size += 64;
    if (!this->removeDirEntry(dir, file.file_name))
        throw std::runtime_error("how??");

    this->disk.write(1, (uint8_t*)this->fat);
    return 0;
}

// rm <filepath> removes / deletes the file <filepath>
int FS::rm(std::string filepath) {
    dir_entry dir = this->workingDir;
    std::vector<std::string> path;
    parsePath(filepath, path);
    dir_entry file;

    // find src dir
    if (path.size() > 1 && !this->__cd(dir, std::vector<std::string>(
                                                path.begin(), path.end() - 1)))
        return -1;
    // find src
    if (!this->findDirEntry(dir, path.back(), file)) return -1;

    if (!this->removeDirEntry(dir, file.file_name)) return -1;
    this->free(file.first_blk);

    this->disk.write(1, (uint8_t*)this->fat);
    return 0;
}

// append <filepath1> <filepath2> appends the contents of file <filepath1> to
// the end of file <filepath2>. The file <filepath1> is unchanged.
int FS::append(std::string filepath1, std::string filepath2) {
    // Work very much not done
    std::vector<std::string> path1;
    std::vector<std::string> path2;
    parsePath(filepath1, path1);
    parsePath(filepath2, path2);

    dir_entry src;
    dir_entry dest;

    this->__cdToFile(src, path1);

    // need to track daddy dir for path2 file aswell to update file size in
    // dir_entry
    this->__cdToFile(dest, path2);

    int16_t offset = dest.size & (BLOCK_SIZE - 1);

    int16_t extraFatSpace = this->reserve((int)src.size - BLOCK_SIZE + offset);
    if (extraFatSpace == -1) return -1;

    int16_t destFat = dest.first_blk;
    while (this->fat[destFat] != FAT_EOF) destFat = this->fat[destFat];

    this->fat[destFat] = extraFatSpace;

    int16_t srcFat = src.first_blk;
    uint8_t srcData[BLOCK_SIZE]{0};
    uint8_t destData[BLOCK_SIZE]{0};
    this->disk.read(destFat, destData);
    while (srcFat != FAT_EOF) {
        this->disk.read(srcFat, srcData);

        // copy until dest is full
        for (int i = 0; i < BLOCK_SIZE - offset; i++) {
            destData[i + offset] = srcData[i];
        }
        this->disk.write(destFat, destData);
        // clean buffer
        for (int i = 0; i < BLOCK_SIZE; i++) destData[i] = 0;
        destFat = this->fat[destFat];
        if (destFat == FAT_EOF) return 0;

        // copy until srcblock is empty
        for (int i = 0; i < offset; i++) {
            destData[i] = srcData[i - offset + BLOCK_SIZE];
        }
    }
    this->disk.write(destFat, destData);
    this->disk.write(FAT_BLOCK, (uint8_t*)this->fat);

    return 0;
}

// mkdir <dirpath> creates a new sub-directory with the name <dirpath>
// in the current directory
int FS::mkdir(std::string dirpath) {
    dir_entry dir2 = this->workingDir;
    if (this->__cd(dir2, dirpath)) return -1;
    dir2 = this->workingDir;
    this->__cd(dir2, dirpath, true);
    return 0;
}

// cd <dirpath> changes the current (working) directory to the directory named
// <dirpath>
int FS::cd(std::string dirpath) {
    dir_entry newWorkingDir = this->workingDir;
    std::vector<std::string> path;
    parsePath(dirpath, path);
    std::vector<dir_entry> newWorkingPath(this->workingPath);
    for (std::string dirName : path) {
        if (!this->findDirEntry(newWorkingDir, dirName, newWorkingDir))
            return -1;
        if (newWorkingDir.type != TYPE_DIR) return -1;
        if (dirName == "..") {
            if (newWorkingPath.size() > 0) {
                newWorkingPath.pop_back();
            }
        } else if (dirName != ".") {
            newWorkingPath.emplace_back(newWorkingDir);
        }
    }

    std::array<dir_entry, 64> dirBlock;
    this->disk.read(newWorkingDir.first_blk, (uint8_t*)dirBlock.data());

    if (dirBlock[0].first_blk != newWorkingDir.first_blk)
        throw std::runtime_error("very bad");

    this->workingDir = dirBlock[0];
    this->workingPath = newWorkingPath;
    return 0;
}

// pwd prints the full path, i.e., from the root directory, to the current
// directory, including the currect directory name
int FS::pwd() {
    std::string path = "/";

    for (dir_entry dir : this->workingPath) {
        path += std::string(dir.file_name) + "/";
    }
    std::cout << path << "\n";
    return 0;
}

// chmod <accessrights> <filepath> changes the access rights for the
// file <filepath> to <accessrights>.
int FS::chmod(std::string accessrights, std::string filepath) {
    if (accessrights.size() > 3) return -1;
    uint8_t accessRightBin = 0;
    if (accessrights.find('r') != accessrights.size()) accessRightBin |= READ;
    if (accessrights.find('w') != accessrights.size()) accessRightBin |= WRITE;
    if (accessrights.find('x') != accessrights.size())
        accessRightBin |= EXECUTE;

    std::vector<std::string> path;
    parsePath(filepath, path);

    dir_entry dir = this->workingDir;
    if (path.size() > 1) {
        this->__cd(dir, std::vector<std::string>(path.begin(), path.end() - 1));
    }

    dir_entry target_dir;
    int16_t fatIndex;
    int blockIndex;
    this->findDirEntry(dir, path.back(), target_dir, fatIndex, blockIndex);

    std::array<dir_entry, 64> dirBlock;
    this->disk.read(fatIndex, (uint8_t*)dirBlock.data());
    dirBlock[blockIndex].access_rights = accessRightBin;
    this->disk.write(fatIndex, (uint8_t*)dirBlock.data());

    this->disk.read(target_dir.first_blk, (uint8_t*)dirBlock.data());
    dirBlock[0].access_rights = accessRightBin;
    this->disk.write(target_dir.first_blk, (uint8_t*)dirBlock.data());

    if (target_dir.first_blk == this->workingDir.first_blk)
        this->workingDir.access_rights = accessRightBin;

    return 0;
}
