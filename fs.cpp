#include "fs.h"

#include <stdint.h>

#include <cstring>
#include <iostream>
#include <vector>

int16_t FS::reserve(size_t size) {
    int neededNodes = (size + (BLOCK_SIZE - 1)) / BLOCK_SIZE;
    int16_t firstNode = this->getEmptyFat();
    if (firstNode == -1) return false;
    this->fat[firstNode] = FAT_EOF;

    int16_t prevNode = firstNode;
    for (int i = 0; i < (neededNodes - 1); i++) {
        int16_t newNode = this->getEmptyFat();
        if (newNode != -1) {
            this->fat[prevNode] = newNode;
            this->fat[newNode] = FAT_EOF;
            prevNode = newNode;

        } else {
            free(firstNode);
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

bool FS::addDirEntry(const dir_entry& dir, dir_entry newEntry) {
    dir_entry temp;
    if (std::string("").compare(newEntry.file_name) == 0) return false;
    if (this->workingPath.searchDir(dir, std::string(newEntry.file_name),
                                    temp)) {
        return false;
    }

    int16_t fatIndex = dir.first_blk;

    while (this->fat[fatIndex] != FAT_EOF) {
        fatIndex = this->fat[fatIndex];
    }

    std::array<dir_entry, 64> dirBlock{};
    this->disk.read(fatIndex, (uint8_t*)dirBlock.data());

    int dirEntryIndexInBlock;
    for (dirEntryIndexInBlock = 0; dirEntryIndexInBlock < 64;
         dirEntryIndexInBlock++) {
        if (!validEntry(dirBlock[dirEntryIndexInBlock])) break;
    }
    // if we don't have enough space in our directory for the dir_entry
    if (dirEntryIndexInBlock ==
        64) {  // I would say trust but don't the magicc might no be magiccing
        dirEntryIndexInBlock = 0;
        int16_t newDirBlock = this->getEmptyFat();
        if (newDirBlock == -1) {
            return false;
        }
        this->fat[fatIndex] = newDirBlock;
        this->fat[newDirBlock] = FAT_EOF;
        fatIndex = this->fat[fatIndex];

        // add metadata for new file
        std::array<dir_entry, 64> freshDirBlock{};
        freshDirBlock[dirEntryIndexInBlock] = newEntry;
        this->disk.write(fatIndex, (uint8_t*)freshDirBlock.data());

    } else {
        // add metadata for new file
        dirBlock[dirEntryIndexInBlock] = newEntry;
        this->disk.write(fatIndex, (uint8_t*)dirBlock.data());
    }

    this->disk.write(FAT_BLOCK, (uint8_t*)this->fat);
    return true;
}

bool FS::removeDirEntry(dir_entry& dir, std::string fileName) {
    int16_t removeBlockIndex;
    int removeDirEntryIndex;
    dir_entry entryToRemove;

    if (!this->workingPath.searchDir(dir, fileName, entryToRemove,
                                     removeBlockIndex, removeDirEntryIndex)) {
        return false;
    }

    int16_t fatIndex = dir.first_blk;
    while (this->fat[fatIndex] != FAT_EOF) {
        fatIndex = this->fat[fatIndex];
    }

    std::array<dir_entry, 64> dirBlock{};
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

    this->disk.write(FAT_BLOCK, (uint8_t*)this->fat);
    return true;
}

bool FS::__create(const dir_entry& dir, dir_entry& metadata,
                  const std::string& data) {
    // reserve space that the new file needs
    int16_t startfat = this->reserve(data.size());
    if (startfat == -1) return false;

    // set metadata for the file
    metadata.first_blk = startfat;
    metadata.size = data.size();

    if (!this->addDirEntry(dir, metadata)) {
        this->free(startfat);
        return false;
    }

    std::string extra;
    if( metadata.type == TYPE_DIR){
        dir_entry dotanddotdot[] = {metadata, dir};
        std::string extra = std::string((char*)dotanddotdot, 128);
        std::string(".").copy(&extra[0], 2);
        std::string("..").copy(&extra[64], 3);
    }

    // write the data to the new file
    size_t pos = 0;
    int16_t fatIndex = metadata.first_blk;
    while (fatIndex != FAT_EOF) {
        char buffer[4096]{0};
        (extra + data).copy(buffer, BLOCK_SIZE, pos);
        this->disk.write(fatIndex, (uint8_t*)buffer);
        pos += BLOCK_SIZE;
        fatIndex = this->fat[fatIndex];
    }
    this->disk.write(FAT_BLOCK, (uint8_t*)this->fat);

    return true;
}

int FS::getEmptyFat() const {
    for (uint16_t i = 2; i < BLOCK_SIZE / 2; i++)
        if (this->fat[i] == FAT_FREE) return i;
    return -1;
}

FS::FS() : workingPath(&this->disk, this->fat) {
    this->disk.read(FAT_BLOCK, (uint8_t*)this->fat);
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
    this->workingPath = Path(&this->disk, this->fat);
    return 0;
}

// create <filepath> creates a new file on the disk, the data content is
// written on the following rows (ended with an empty row)
int FS::create(std::string filepath) {
    dir_entry currentDir;
    std::string fileName;

    if (!this->workingPath.findUpToLast(filepath, currentDir, fileName))
        return -1;
    if (currentDir.type != TYPE_DIR) return -1;

    if (!(currentDir.access_rights & WRITE)) return -1;

    dir_entry newFile{
        .type = TYPE_FILE,
        .access_rights = READ | WRITE,
    };

    if (fileName.size() > 55) return -1;

    fileName.copy(newFile.file_name, 56);

    std::string data, line;

    while (std::getline(std::cin, line) && !line.empty()) {
        data += line + "\n";
    }

    if (!this->__create(currentDir, newFile, data)) return -1;

    return 0;
}

// cat <filepath> reads the content of a file and prints it on the screen
int FS::cat(std::string filepath) {
    dir_entry file;
    if (!this->workingPath.find(filepath, file)) return -1;
    if (file.type != TYPE_FILE) return -1;
    if (!(file.access_rights & READ)) return -1;

    int16_t nextFat = file.first_blk;
    char dirBlock[BLOCK_SIZE];
    std::string print;

    // Go through each full dirBlock
    for (int i = 0; i < (file.size / BLOCK_SIZE); i++) {
        if (nextFat == FAT_EOF)
            throw std::runtime_error(
                ("some shite went wrong Line: " + __LINE__) +
                std::string(" File: " __FILE__));

        this->disk.read(nextFat, (uint8_t*)dirBlock);
        for (char c : dirBlock) {
            print += c;
        }
        nextFat = this->fat[nextFat];
    }

    // Go through the direntries in a non full dirblock
    size_t rest = (file.size & (BLOCK_SIZE - 1));
    if (rest) {
        if (nextFat == FAT_EOF)
            throw std::runtime_error("some shite went wrong");
        this->disk.read(nextFat, (uint8_t*)dirBlock);
        for (int i = 0; i < rest; i++) {
            print += dirBlock[i];
        }
    }

    std::cout << print;

    return 0;
}

// ls lists the content in the current directory (files and sub-directories)
int FS::ls() {
    if (!(this->workingPath.workingDir().access_rights & READ)) return -1;

    int16_t nextFat = this->workingPath.workingDir().first_blk;
    std::cout << "name\t type\t accessrights\t size\n";

    while (nextFat != FAT_EOF) {
        std::array<dir_entry, 64> dirBlock{};
        this->disk.read(nextFat, (uint8_t*)dirBlock.data());
        for (size_t i = 0; i < 64; i++) {
            // Print if not hidden file
            if (dirBlock[i].file_name[0] != '.' && validEntry(dirBlock[i])) {
                std::string type = dirBlock[i].type ? "dir" : "file";
                std::string size =
                    dirBlock[i].type ? "-" : std::to_string(dirBlock[i].size);
                std::string rights;
                rights += (dirBlock[i].access_rights & READ) ? 'r' : '-';
                rights += (dirBlock[i].access_rights & WRITE) ? 'w' : '-';
                rights += (dirBlock[i].access_rights & EXECUTE) ? 'x' : '-';
                std::cout << std::string(dirBlock[i].file_name) + "\t " + type +
                                 "\t " + rights + "\t\t " + size + "\n";
            }
        }
        nextFat = this->fat[nextFat];
    }

    return 0;
}

// cp <sourcepath> <destpath> makes an exact copy of the file
// <sourcepath> to a new file <destpath>
int FS::cp(std::string sourcepath, std::string destpath) {
    dir_entry src;
    dir_entry dest;

    // find src
    if (!this->workingPath.find(sourcepath, src)) return -1;
    if (src.type != TYPE_FILE) return -2;
    if (!(src.access_rights & READ)) return -2;

    // find dest dir
    std::string fileName;
    if (!this->workingPath.findUpToLast(destpath, dest, fileName)) return -3;
    if (dest.type != TYPE_DIR) return -4;

    dir_entry filecpy = src;

    // check if last is a dir or a new filename, if neither ERROR
    if (!this->workingPath.searchDir(dest, fileName, dest)) {
        for (int i = 0; i < 56; i++) filecpy.file_name[i] = 0;
        fileName.copy(filecpy.file_name, 56);
    } else {
        if (dest.type != TYPE_DIR) return -5;
    }

    // Make sure we are allowed to write
    if (!(dest.access_rights & WRITE)) return -6;

    // reserve needed space
    int16_t newFats = this->reserve(src.size);
    if (newFats == -1) return -1;
    filecpy.first_blk = newFats;

    // add entry
    if (!this->addDirEntry(dest, filecpy)) return -7;

    // copy data
    int16_t nextSrcFat = src.first_blk;
    int16_t nextTargetFat = filecpy.first_blk;
    while (nextSrcFat != FAT_EOF) {
        uint8_t block[BLOCK_SIZE]{0};
        this->disk.read(nextSrcFat, block);
        this->disk.write(nextTargetFat, block);
        nextSrcFat = this->fat[nextSrcFat];
        nextTargetFat = this->fat[nextTargetFat];
    }

    // update FAT block
    this->disk.write(FAT_BLOCK, (uint8_t*)this->fat);

    return 0;
}

// mv <sourcepath> <destpath> renames the file <sourcepath> to the name
// <destpath>, or moves the file <sourcepath> to the directory <destpath> (if
// dest is a directory)
int FS::mv(std::string sourcepath, std::string destpath) {
    dir_entry srcDir;
    dir_entry srcFile;

    // find src dir
    std::string fileName;
    if (!this->workingPath.findUpToLast(sourcepath, srcDir, fileName))
        return -1;

    // find src
    if (!this->workingPath.searchDir(srcDir, fileName, srcFile)) return -2;

    // check that we are allowed to read and write
    if (!(srcDir.access_rights & WRITE)) return -1;
    if (!(srcDir.access_rights & READ)) return -1;

    // find possible target dir
    dir_entry fileCopy = srcFile;
    dir_entry targetDir;
    if (!this->workingPath.findUpToLast(destpath, targetDir, fileName))
        return -3;

    // check if last entry is a valid dir, if it is that is the target dir, if
    // it is a file we cannot move here, if there exists no entry with the name
    // fileName, the name of our moved entry is changed to fileName
    dir_entry temp;
    if (this->workingPath.searchDir(targetDir, fileName, temp)) {
        if (temp.type == TYPE_DIR) {
            targetDir = temp;
        } else {
            return -4;
        }
    } else {
        fileName.copy(fileCopy.file_name, 56);
    }

    // check that we are allowed to write in target dir
    if (!(targetDir.access_rights & WRITE)) return -1;

    // move the directory entry
    if (!this->addDirEntry(targetDir, fileCopy)) return -5;
    if (srcDir.first_blk == targetDir.first_blk) srcDir.size += 64;
    if (!this->removeDirEntry(srcDir, srcFile.file_name))
        throw std::runtime_error("how??");

    // update ".." file in directory
    if (fileCopy.type == TYPE_DIR) {
        std::array<dir_entry, 64> dirBlock{};
        this->disk.read(fileCopy.first_blk, (uint8_t*)dirBlock.data());
        dirBlock[1] = targetDir;
        std::string("..").copy(dirBlock[1].file_name, 56);
        this->disk.write(fileCopy.first_blk, (uint8_t*)dirBlock.data());
    }

    // update FAT
    this->disk.write(FAT_BLOCK, (uint8_t*)this->fat);
    return 0;
}

// rm <filepath> removes / deletes the file <filepath>
int FS::rm(std::string filepath) {
    dir_entry dir;
    dir_entry file;

    // find src dir
    std::string fileName;
    if (!this->workingPath.findUpToLast(filepath, dir, fileName)) return -1;

    // find src
    if (!this->workingPath.searchDir(dir, fileName, file)) return -1;

    // Make sure we have write rights
    if (!(dir.access_rights & WRITE)) return -1;
    if (!(dir.access_rights & READ)) return -1;

    // remove the entry and free the fat
    if (!this->removeDirEntry(dir, file.file_name)) return -1;
    this->free(file.first_blk);

    // update fat
    this->disk.write(FAT_BLOCK, (uint8_t*)this->fat);
    return 0;
}

// append <filepath1> <filepath2> appends the contents of file <filepath1> to
// the end of file <filepath2>. The file <filepath1> is unchanged.
int FS::append(std::string filepath1, std::string filepath2) {
    // find src
    dir_entry src;
    if (!this->workingPath.find(filepath1, src)) return -1;
    if (src.type != TYPE_FILE) return -2;

    // Destination Data
    dir_entry dest;
    dir_entry destDir;
    int destBlockIndex;
    int16_t destFatIndex;
    std::string targetFileName;

    // find dest Dir
    if (!this->workingPath.findUpToLast(filepath2, destDir, targetFileName))
        return -3;

    // find dest
    if (!this->workingPath.searchDir(destDir, targetFileName, dest,
                                     destFatIndex, destBlockIndex))
        return -4;

    if (dest.type != TYPE_FILE) return -5;

    // check access rights
    if (!(src.access_rights & READ)) return -6;
    if (!(dest.access_rights & WRITE)) return -7;

    // go to last block in dest
    int16_t destFat = dest.first_blk;
    while (this->fat[destFat] != FAT_EOF) destFat = this->fat[destFat];

    // place to start adding new data in last block of dest
    int16_t offset = (dest.size & (BLOCK_SIZE - 1));

    // reserve neccesary space
    if (dest.size == 0 && src.size > BLOCK_SIZE) {
        int neededSpace = src.size - BLOCK_SIZE;
        int16_t extraFatSpace = this->reserve(neededSpace);
        if (extraFatSpace == -1) return -8;
        this->fat[destFat] = extraFatSpace;
    }
    // (BLOCK_SIZE - ((dest.size - 1) & (BLOCK_SIZE - 1)) - 1) *This is how much
    // of the last block is unused
    else if ((BLOCK_SIZE - ((dest.size - 1) & (BLOCK_SIZE - 1)) - 1) <
             src.size) {
        int neededSpace =
            src.size - (BLOCK_SIZE - ((dest.size - 1) & (BLOCK_SIZE - 1)) - 1);
        if (neededSpace < 0) throw std::runtime_error("sometink bad happend");
        int16_t extraFatSpace = this->reserve(neededSpace);
        if (extraFatSpace == -1) return -9;
        this->fat[destFat] = extraFatSpace;
    }

    // incase current fatblock is already full
    if (dest.size > 0 && offset == 0) {
        destFat = this->fat[destFat];
    }

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

        // iterate to next destFat
        destFat = this->fat[destFat];

        // copy until srcblock is empty
        for (int i = 0; i < offset; i++) {
            destData[i] = srcData[i - offset + BLOCK_SIZE];
        }
        srcFat = this->fat[srcFat];
    }
    if (destFat != FAT_EOF) this->disk.write(destFat, destData);

    this->disk.write(FAT_BLOCK, (uint8_t*)this->fat);

    // Update dir_entry in directory
    std::array<dir_entry, 64> dirBlock{};
    this->disk.read(destFatIndex, (uint8_t*)dirBlock.data());
    dirBlock[destBlockIndex].size += src.size;
    this->disk.write(destFatIndex, (uint8_t*)dirBlock.data());

    return 0;
}

// mkdir <dirpath> creates a new sub-directory with the name <dirpath>
// in the current directory
int FS::mkdir(std::string dirpath) {
    // Parse path
    std::vector<std::string> path;
    if (!Path::parsePath(dirpath, path)) return -1;

    // find the last valid dir_entry
    dir_entry currentDir = this->workingPath.workingDir();
    auto it = path.begin();
    for (; it < path.end(); it++) {
        if (!(currentDir.access_rights & READ)) return -1;
        if (!this->workingPath.searchDir(currentDir, *it, currentDir))
            break;
    }

    // if the last valid direntry is a file or we have reached the end of the
    // path we cannot create any directories
    if (it == path.end() || currentDir.type == TYPE_FILE ||
        !(currentDir.access_rights & WRITE))
        return -1;

    // create directories from rest of path
    for (; it < path.end(); it++) {
        dir_entry newDir{
            .type = TYPE_DIR,
            .access_rights = READ | WRITE,
        };
        it->copy(newDir.file_name, 56);
        if (!this->__create(currentDir, newDir, "")) return -1;
        currentDir = newDir;
    }
    return 0;
}

// cd <dirpath> changes the current (working) directory to the directory named
// <dirpath>
int FS::cd(std::string dirpath) {
    if (!this->workingPath.cd(dirpath)) return -1;
    return 0;
}

// pwd prints the full path, i.e., from the root directory, to the current
// directory, including the currect directory name
int FS::pwd() {
    std::cout << this->workingPath.pwd() + "\n";
    return 0;
}

// chmod <accessrights> <filepath> changes the access rights for the
// file <filepath> to <accessrights>.
int FS::chmod(std::string accessrights, std::string filepath) {
    // Parse access rights
    if (accessrights.size() > 1) return -1;
    if (!std::isdigit(accessrights[0])) return -1;
    uint8_t accessRightBin = std::stoi(accessrights) & (READ | WRITE | EXECUTE);

    // Find directory that target lies in
    dir_entry dir;
    std::string fileName;
    if (!this->workingPath.findUpToLast(filepath, dir, fileName)) return -1;

    // find target
    dir_entry target;
    int16_t fatIndex;
    int blockIndex;
    if (!this->workingPath.searchDir(dir, fileName, target, fatIndex,
                                     blockIndex))
        return -1;

    // update dir_entry in directory
    std::array<dir_entry, 64> dirBlock{};
    this->disk.read(fatIndex, (uint8_t*)dirBlock.data());
    dirBlock[blockIndex].access_rights = accessRightBin;
    this->disk.write(fatIndex, (uint8_t*)dirBlock.data());

    // update "." entry in directory
    if (target.type == TYPE_DIR) {
        this->disk.read(target.first_blk, (uint8_t*)dirBlock.data());
        dirBlock[0].access_rights = accessRightBin;
        this->disk.write(target.first_blk, (uint8_t*)dirBlock.data());
    }

    this->workingPath.updatePathEntry(target, target);

    return 0;
}

FS::Path::Path(Disk* disk, int16_t* const fat) : disk(disk), fat(fat) {
    // Add root dir
    dir_entry root{
        .first_blk = 0,
        .type = TYPE_DIR,
        .access_rights = READ | WRITE,
    };
    this->path.emplace_back(root);
}

bool FS::Path::find(const std::string& path, dir_entry& result) const {
    std::vector<std::string> pathv;
    if (!this->parsePath(path, pathv)) return false;
    result = this->workingDir();

    for (std::string file : pathv) {
        if (!result.type == TYPE_DIR) return false;
        if (!this->searchDir(result, file, result)) return false;
    }
    return true;
}

bool FS::Path::findUpToLast(const std::string& path, dir_entry& result,
                            std::string& last) const {
    std::vector<std::string> pathv;
    if (!this->parsePath(path, pathv)) return false;
    result = this->workingDir();

    for (auto it = pathv.begin(); it < pathv.end() - 1; it++) {
        if (!result.type == TYPE_DIR) return false;
        if (!this->searchDir(result, *it, result)) return false;
    }
    last = pathv.back();
    return true;
}

inline bool FS::Path::searchDir(const dir_entry& dir,
                                const std::string& fileName,
                                dir_entry& result) const {
    int16_t fatIndex;
    int blockIndex;
    return this->searchDir(dir, fileName, result, fatIndex, blockIndex);
}

bool FS::Path::searchDir(const dir_entry& dir, const std::string& fileName,
                         dir_entry& result, int16_t& fatIndex,
                         int& blockIndex) const {
    if (fileName == "/") {
        result = this->path[0];
        return true;
    }
    fatIndex = dir.first_blk;
    std::array<dir_entry, 64> dirBlock{};
    if (!(dir.access_rights & READ)) return false;

    while (fatIndex != FAT_EOF) {
        this->disk->read(fatIndex, (uint8_t*)dirBlock.data());

        for (blockIndex = 0; blockIndex < 64; blockIndex++) {
            if (!validEntry(dirBlock[blockIndex])) return false;

            if (fileName.compare(0, 56, dirBlock[blockIndex].file_name) == 0) {
                result = dirBlock[blockIndex];
                return true;
            }
        }
        fatIndex = this->fat[fatIndex];
    }

    return false;
}

bool FS::Path::cd(const std::string& path) {
    // make copy of working path
    std::vector<dir_entry> newPath(this->path);

    // parse path
    std::vector<std::string> pathv;
    if (!this->parsePath(path, pathv)) return false;
    dir_entry dir = this->workingDir();

    // check for absolute path
    auto it = pathv.begin();
    if (*it == "/") {
        newPath.clear();
        dir = this->rootDir();
        newPath.emplace_back(dir);
        it++;
    }

    // iterate to end of path
    for (; it < pathv.end(); it++) {
        if (*it == "..") {
            if (newPath.size() > 1) {
                newPath.pop_back();
                dir = newPath.back();
            }
        } else if (*it != ".") {
            if (!(dir.access_rights & READ)) return false;
            if (this->searchDir(dir, *it, dir)) {
                if (dir.type != TYPE_DIR) return false;
                newPath.emplace_back(dir);
            } else {
                return false;
            }
        }
    }

    // apply the newPath
    this->path = newPath;
    return true;
}

std::string FS::Path::pwd() const {
    std::string path = "/";
    auto it = this->path.begin() + 1;
    for (; it < this->path.end() - 1; it++)
        path += std::string(it->file_name) + "/";
    if (this->path.size() > 1) path += std::string(it->file_name);
    return path;
}

bool FS::Path::parsePath(const std::string& paths,
                         std::vector<std::string>& pathv) {
    pathv.clear();
    if (paths.size() == 0) return false;

    // check for absolute path
    size_t index;
    if (paths[0] == '/') {
        pathv.emplace_back("/");
        index = 1;
    } else {
        index = 0;
    }

    // parse every fileName into a vector
    std::string fileName = "";
    for (; index < paths.size(); index++) {
        if (paths[index] == '/' || paths[index] == '\n') {
            // check that fileName is valid
            if (fileName.size() == 0 || fileName[0] == 0) return false;
            // add fileName to vector
            pathv.emplace_back(fileName);
            if (paths[index] == '\n') return true;
            fileName = "";
        } else {
            fileName += paths[index];
        }
    }
    // if leftover check that fileName is valid and add
    if (fileName.size() > 0) {
        if (fileName[0] == 0) return false;
        pathv.emplace_back(fileName);
    }

    return true;
}

void FS::Path::updatePathEntry(const dir_entry& entry, dir_entry newData) {
    // Find correct entry and change it
    for (dir_entry& dir : this->path) {
        if (dir.first_blk == entry.first_blk) {
            dir = newData;
            return;
        }
    }
}
