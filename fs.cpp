#include "fs.h"

#include <stdint.h>

#include <cstring>
#include <iostream>
#include <vector>

// Type definitions for clarity and to reduce verbosity
typedef std::array<dir_entry, FS::DIR_BLK_SIZE> dir_block;
typedef std::array<char, BLOCK_SIZE> file_block;

// A mask used in size calculation to calculate rest of BLOCK_SIZE,
// usually shows how much of the last block is used
static const int BLOCK_MASK = BLOCK_SIZE - 1;

// -------------------FILE SYSTEM--------------------

// Reads the FAT block and initilizes the working path
FS::FS() : workingPath(&this->disk, this->fat) { this->readFat(); }

// Default destructor
FS::~FS() {}

// Formats the disk, i.e., creates an empty file system
int FS::format() {
    this->fat[ROOT_BLOCK] = FAT_EOF;
    this->fat[FAT_BLOCK] = FAT_EOF;
    for (int i = 2; i < FS::FAT_SIZE; i++) this->fat[i] = FAT_FREE;

    // Size 64 to make sure we don't go out of scope in disk.write since that
    // the function takes a uint8_t* and then indexes 4096 steps into that
    // root dir metadata
    dir_block directories = {dir_entry{
                                 .file_name = ".",
                                 .size = 0,
                                 .first_blk = 0,
                                 .type = TYPE_DIR,
                                 .access_rights = READ | WRITE,
                             },
                             dir_entry{
                                 .file_name = "..",
                                 .size = 0,
                                 .first_blk = 0,
                                 .type = TYPE_DIR,
                                 .access_rights = READ | WRITE,
                             }};

    this->write(ROOT_BLOCK, directories);
    this->writeFat();
    this->workingPath = Path(&this->disk, this->fat);
    return 0;
}

// Creates a new file on the disk, the data content is
// written on the following rows (ended with an empty row)
int FS::create(std::string filepath) {
    dir_entry currentDir;
    std::string fileName;

    // Finds the directory where the file is supposed to be or return -1 if not found
    if (!this->workingPath.findUpToLast(filepath, currentDir, fileName)) return -1;

    // Validity check
    if (currentDir.type != TYPE_DIR) return -1;
    if (!(currentDir.access_rights & WRITE)) return -1;

    // Creates new file entry
    dir_entry newFile{
        .type = TYPE_FILE,
        .access_rights = READ | WRITE,
    };

    // Making sure the file name is not too large
    if (fileName.size() > 55) return -1;

    // Sets file name by copying the last component in the path
    fileName.copy(newFile.file_name, 56);

    // Gets file data from user
    std::string data, line;
    while (std::getline(std::cin, line) && !line.empty()) {
        data += line + "\n";
    }

    // Creates new file in current directory with metadata and data
    if (!this->__create(currentDir, newFile, data)) return -1;

    return 0;
}

// Reads the content of a file and prints it on the screen
int FS::cat(std::string filepath) {
    dir_entry file;
    if (!this->workingPath.find(filepath, file)) return -1;
    if (file.type != TYPE_FILE) return -1;
    if (!(file.access_rights & READ)) return -1;

    int16_t nextFat = file.first_blk;
    file_block dirBlock;
    std::string print;

    // Go through each full dirBlock
    for (int i = 0; i < (file.size / BLOCK_SIZE); i++) {
        if (nextFat == FAT_EOF) throw std::runtime_error(("1: Reached end of file before expected in cat()!"));

        this->read(nextFat, dirBlock);
        std::cout.write(dirBlock.data(), BLOCK_SIZE);
        nextFat = this->fat[nextFat];
    }

    // Go through the direntries in a non full dirblock
    size_t rest = (file.size & BLOCK_MASK);
    if (rest) {
        if (nextFat == FAT_EOF) throw std::runtime_error("2: Reached end of file before expected in cat()!");

        this->read(nextFat, dirBlock);
        std::cout.write(dirBlock.data(), rest);
    }

    return 0;
}

// ls() lists the content in the current directory (files and sub-directories)
int FS::ls() {
    if (!(this->workingPath.workingDir().access_rights & READ)) return -1;

    int16_t nextFat = this->workingPath.workingDir().first_blk;
    std::cout << "name\t type\t accessrights\t size\n";

    while (nextFat != FAT_EOF) {
        dir_block dirBlock{};
        this->read(nextFat, dirBlock);
        for (size_t i = 0; i < FS::DIR_BLK_SIZE; i++) {
            // Print if not hidden file
            if (dirBlock[i].file_name[0] != '.' && isNotFreeEntry(dirBlock[i])) {
                std::string type = dirBlock[i].type ? "dir" : "file";
                std::string size = dirBlock[i].type ? "-" : std::to_string(dirBlock[i].size);
                std::string rights;
                rights += (dirBlock[i].access_rights & READ) ? 'r' : '-';
                rights += (dirBlock[i].access_rights & WRITE) ? 'w' : '-';
                rights += (dirBlock[i].access_rights & EXECUTE) ? 'x' : '-';
                std::cout << std::string(dirBlock[i].file_name) + "\t " + type + "\t " + rights + "\t\t " + size + "\n";
            }
        }
        nextFat = this->fat[nextFat];
    }

    return 0;
}

// Makes an exact copy of the file to a new file
int FS::cp(std::string sourcepath, std::string destpath) {
    dir_entry src;
    dir_entry dest;

    // Find src
    if (!this->workingPath.find(sourcepath, src)) return -1;
    if (src.type != TYPE_FILE) return -2;
    if (!(src.access_rights & READ)) return -2;

    // Find dest dir
    std::string fileName;
    if (!this->workingPath.findUpToLast(destpath, dest, fileName)) return -3;
    if (dest.type != TYPE_DIR) return -4;

    dir_entry filecpy = src;

    // Check if last is a dir or a new filename, if neither ERROR
    if (!this->workingPath.searchDir(dest, fileName, dest)) {
        for (int i = 0; i < 56; i++) filecpy.file_name[i] = 0;
        fileName.copy(filecpy.file_name, 56);
    } else {
        if (dest.type != TYPE_DIR) return -5;
    }

    // Make sure we are allowed to write
    if (!(dest.access_rights & WRITE)) return -6;

    // Reserve needed space
    int16_t newFats = this->reserve(src.size);
    if (newFats == -1) return -1;
    filecpy.first_blk = newFats;

    // Add entry
    if (!this->addDirEntry(dest, filecpy)) {
        this->free(newFats);
        return -7;
    }

    // Copy data
    int16_t nextSrcFat = src.first_blk;
    int16_t nextTargetFat = filecpy.first_blk;
    while (nextSrcFat != FAT_EOF) {
        file_block block{0};
        this->read(nextSrcFat, block);
        this->write(nextTargetFat, block);

        nextSrcFat = this->fat[nextSrcFat];
        nextTargetFat = this->fat[nextTargetFat];
    }

    // Writes updates to the FAT block
    this->writeFat();

    return 0;
}

// Renames the file or moves the file to the directory (if dest is a directory)
int FS::mv(std::string sourcepath, std::string destpath) {
    dir_entry srcDir;
    dir_entry srcFile;

    // Find src dir
    std::string fileName;
    if (!this->workingPath.findUpToLast(sourcepath, srcDir, fileName)) return -1;

    // Find src
    if (!this->workingPath.searchDir(srcDir, fileName, srcFile)) return -2;

    // Check that we are allowed to read and write
    if (!(srcDir.access_rights & WRITE)) return -1;
    if (!(srcDir.access_rights & READ)) return -1;

    // Find possible target dir
    dir_entry fileCopy = srcFile;
    dir_entry targetDir;
    if (!this->workingPath.findUpToLast(destpath, targetDir, fileName)) return -3;

    // If the last entry is a valid directory, that entry is the target directory
    // If it is a file we cannot move here
    // If there exists no entry with the name fileName the name of our moved entry is changed to fileName
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

    // Validity check
    if (!(targetDir.access_rights & WRITE)) return -1;

    // Moves the directory entry
    if (!this->addDirEntry(targetDir, fileCopy)) return -5;
    if (!this->removeDirEntry(srcDir, srcFile.file_name))
        throw std::runtime_error("removeDirEntry function failed to find dir_entry we have already found");

    // Updates the ".." file in directory
    if (fileCopy.type == TYPE_DIR) {
        dir_block dirBlock{};
        this->read(fileCopy.first_blk, dirBlock);
        dirBlock[1] = targetDir;
        std::string("..").copy(dirBlock[1].file_name, 56);
        this->write(fileCopy.first_blk, dirBlock);
    }

    // Writes updates to the FAT block
    this->writeFat();
    return 0;
}

// Removes / deletes the file
int FS::rm(std::string filepath) {
    dir_entry dir;
    dir_entry file;

    // Find source directory
    std::string fileName;
    if (!this->workingPath.findUpToLast(filepath, dir, fileName)) return -1;

    // Find source
    if (!this->workingPath.searchDir(dir, fileName, file)) return -1;

    // Make sure we have write rights
    if (!(dir.access_rights & WRITE)) return -1;
    if (!(dir.access_rights & READ)) return -1;

    // If the entry is a directory, check that it is empty
    if (file.type == TYPE_DIR) {
        dir_block dirblock;
        this->read(file.first_blk, dirblock);
        if (isNotFreeEntry(dirblock[2])) return -1;
    }

    // Remove the entry and free the FAT
    if (!this->removeDirEntry(dir, file.file_name)) return -1;
    this->free(file.first_blk);

    // Writes updates to the FAT block
    this->writeFat();
    return 0;
}

// Appends the contents of file1 to the end of file2 without changing file1
int FS::append(std::string filepath1, std::string filepath2) {
    // Find source
    dir_entry src;
    if (!this->workingPath.find(filepath1, src)) return -1;
    if (src.type != TYPE_FILE) return -2;
    if (src.size == 0) return 0;

    // Destination Data
    dir_entry dest;
    dir_entry destDir;
    int destBlockIndex;
    int16_t destFatIndex;
    std::string targetFileName;

    // Finds the destination directory
    if (!this->workingPath.findUpToLast(filepath2, destDir, targetFileName)) return -3;

    // Finds the destination entry
    if (!this->workingPath.searchDir(destDir, targetFileName, dest, destFatIndex, destBlockIndex)) return -4;

    // Type check
    if (dest.type != TYPE_FILE) return -5;

    // Checks access rights
    if (!(src.access_rights & READ)) return -6;
    if (!(dest.access_rights & WRITE)) return -7;

    // Goes to the last block in the destination entry
    int16_t destFat = dest.first_blk;
    while (this->fat[destFat] != FAT_EOF) destFat = this->fat[destFat];

    // Place to start adding new data in the last block of the destination entry
    int offset = (int(dest.size) & BLOCK_MASK);

    // Calculate neededSpace
    int neededSpace;
    if (dest.size == 0)
        neededSpace = int(src.size) - BLOCK_SIZE;
    else if (offset == 0)
        neededSpace = int(src.size);
    else
        neededSpace = int(src.size) - BLOCK_SIZE + offset;

    // Reserves necessary space
    if (neededSpace > 0) {
        int neededSpace = src.size - BLOCK_SIZE;
        int16_t extraFatSpace = this->reserve(neededSpace);
        if (extraFatSpace == -1) return -8;
        this->fat[destFat] = extraFatSpace;
    }

    // Incase the current FAT block is already full, go to next FAT block
    if (dest.size > 0 && offset == 0 && neededSpace > 0) {
        destFat = this->fat[destFat];
    }

    int16_t srcFat = src.first_blk;
    file_block srcData{};
    file_block destData{};

    // Copies data from file1 to the end of file2
    this->read(destFat, destData);
    while (srcFat != FAT_EOF) {
        this->read(srcFat, srcData);

        // Copies until the destination block is full
        for (int i = 0; i < BLOCK_SIZE - offset; i++) {
            destData[i + offset] = srcData[i];
        }
        this->write(destFat, destData);

        // Cleans the buffer
        for (int i = 0; i < BLOCK_SIZE; i++) destData[i] = 0;

        // Iterates to the next FAT block for the destination entry
        destFat = this->fat[destFat];

        // Copies until the source block is empty
        for (int i = 0; i < offset; i++) {
            destData[i] = srcData[i - offset + BLOCK_SIZE];
        }
        srcFat = this->fat[srcFat];
    }
    // Writes last data to the destination block
    if (destFat != FAT_EOF) this->write(destFat, destData);

    // Writes updates to the FAT block
    this->writeFat();

    // Updates the dir_entry in the directory
    dir_block dirBlock{};
    this->read(destFatIndex, dirBlock);
    dirBlock[destBlockIndex].size += src.size;
    this->write(destFatIndex, dirBlock);

    return 0;
}

// Creates a new sub-directory in specified path
int FS::mkdir(std::string dirpath) {
    // Parses path
    std::vector<std::string> path;
    if (!Path::parsePath(dirpath, path)) return -1;

    // Finds the last valid dir_entry
    dir_entry currentDir = this->workingPath.workingDir();
    auto it = path.begin();
    for (; it < path.end(); it++) {
        if (!(currentDir.access_rights & READ)) return -1;
        if (!this->workingPath.searchDir(currentDir, *it, currentDir)) break;
    }

    // If the last valid dir_entry is a file or we have reached the end of the path we cannot create any directories
    if (it == path.end() || currentDir.type == TYPE_FILE || !(currentDir.access_rights & WRITE)) return -1;

    // Creates directories from the rest of the path
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

// Changes the current working directory to the specified path
int FS::cd(std::string dirpath) {
    if (!this->workingPath.cd(dirpath)) return -1;
    return 0;
}

// Prints the full path, i.e., from the root directory to the current directory, including the current directory name
int FS::pwd() {
    std::cout << this->workingPath.pwd() + "\n";
    return 0;
}

// Changes the access rights for the file to the specified access rights
int FS::chmod(std::string accessrights, std::string filepath) {
    // Parses the access rights
    if (accessrights.size() > 1) return -1;
    if (!std::isdigit(accessrights[0])) return -1;
    uint8_t accessRightBin = std::stoi(accessrights) & (READ | WRITE | EXECUTE);

    // Finds the directory that the target lies in
    dir_entry dir;
    std::string fileName;
    if (!this->workingPath.findUpToLast(filepath, dir, fileName)) return -1;

    // Finds the target
    dir_entry target;
    int16_t fatIndex;
    int blockIndex;
    if (!this->workingPath.searchDir(dir, fileName, target, fatIndex, blockIndex)) return -1;

    // Updates dir_entry in the directory
    dir_block dirBlock{};
    this->read(fatIndex, dirBlock);
    dirBlock[blockIndex].access_rights = accessRightBin;
    this->write(fatIndex, dirBlock);

    // Updates the "." entry in directory
    if (target.type == TYPE_DIR) {
        this->read(target.first_blk, dirBlock);
        dirBlock[0].access_rights = accessRightBin;
        this->write(target.first_blk, dirBlock);
    }

    // Updates the entry in the current path with the new data provided
    this->workingPath.updatePathEntry(target, target);

    return 0;
}

// ----------------PATH HELPER CLASS-----------------

// Constructor for the FS::Path class
FS::Path::Path(Disk* disk, int16_t* const fat) : disk(disk), fat(fat) {
    // Initializes the root directory entry
    dir_entry root{
        .first_blk = 0,
        .type = TYPE_DIR,
        .access_rights = READ | WRITE,
    };

    // Add the root directory entry to the path
    this->path.emplace_back(root);
}

// Finds the directory entry for the given to the last component
bool FS::Path::find(const std::string& path, dir_entry& result) const {
    std::vector<std::string> pathv;

    // Parses the given path into components
    if (!this->parsePath(path, pathv)) return false;

    // Starts from the working directory
    result = this->workingDir();

    // Traverses each component in the parsed path while checking if it's a directory,
    // and searches for the next component in the current directory
    for (std::string file : pathv) {
        if (!result.type == TYPE_DIR) return false;
        if (!this->searchDir(result, file, result)) return false;
    }
    return true;
}

// Finds the directory entry for the given path up to before the last component
// The variable last becomes the last component of the path
bool FS::Path::findUpToLast(const std::string& path, dir_entry& result, std::string& last) const {
    std::vector<std::string> pathv;

    // Parses the given path into components
    if (!this->parsePath(path, pathv)) return false;

    // Starts from the working directory
    result = this->workingDir();

    // Traverses each component in the parsed path up to the last while checking if it's a directory,
    // and searches for the next component in the current directory
    for (auto it = pathv.begin(); it < pathv.end() - 1; it++) {
        if (!result.type == TYPE_DIR) return false;
        if (!this->searchDir(result, *it, result)) return false;
    }
    last = pathv.back();
    return true;
}

// Wrapper for searchDir() that doesn't require the fatIndex and blockIndex return parameters
inline bool FS::Path::searchDir(const dir_entry& dir, const std::string& fileName, dir_entry& result) const {
    int16_t fatIndex;
    int blockIndex;
    return this->searchDir(dir, fileName, result, fatIndex, blockIndex);
}

// Searches for a specified file in the directory and returns the FAT block's index and the index where result exists
bool FS::Path::searchDir(const dir_entry& dir, const std::string& fileName, dir_entry& result, int16_t& fatIndex,
                         int& blockIndex) const {
    // Checks if the file is an absolute path
    if (fileName == "/") {
        result = this->path[0];
        return true;
    }

    // Creates container for a directory block
    dir_block dirBlock{};

    // Validity check
    if (!(dir.access_rights & READ)) return false;

    // Goes through each block in the directory
    fatIndex = dir.first_blk;
    while (fatIndex != FAT_EOF) {
        this->disk->read(fatIndex, (uint8_t*)dirBlock.data());

        // Goes through each directory entry in the block
        for (blockIndex = 0; blockIndex < FS::DIR_BLK_SIZE; blockIndex++) {
            // Checks if we are at the end of the directory
            if (!isNotFreeEntry(dirBlock[blockIndex])) return false;

            // Checks if the entry has been found
            if (fileName.compare(0, 56, dirBlock[blockIndex].file_name) == 0) {
                result = dirBlock[blockIndex];
                return true;
            }
        }
        fatIndex = this->fat[fatIndex];
    }

    return false;
}

// Adds path to current path
bool FS::Path::cd(const std::string& path) {
    // Makes copy of working path
    std::vector<dir_entry> newPath(this->path);

    // Parses path
    std::vector<std::string> pathv;
    if (!this->parsePath(path, pathv)) return false;
    dir_entry dir = this->workingDir();

    // Checks for absolute path
    auto it = pathv.begin();
    if (*it == "/") {
        newPath.clear();
        dir = this->rootDir();
        newPath.emplace_back(dir);
        it++;
    }

    // Iterates to end of path
    for (; it < pathv.end(); it++) {
        if (*it == "..") {
            // If not in root directory, go to parent directory
            if (newPath.size() > 1) {
                newPath.pop_back();
                dir = newPath.back();
            }
        } else if (*it != ".") {
            // Validity check
            if (!(dir.access_rights & READ)) return false;

            // Searches for the directory and adds it to the newPath
            if (this->searchDir(dir, *it, dir)) {
                if (dir.type != TYPE_DIR) return false;
                newPath.emplace_back(dir);
            } else {
                return false;
            }
        }
    }

    // Applies the newPath
    this->path = newPath;
    return true;
}

// Formats the working path to a string
std::string FS::Path::pwd() const {
    std::string path = "/";

    // Skips root in the iterator
    auto it = this->path.begin() + 1;

    // Goes through the path except the root and last directory entries and formats them to path strings
    for (; it < this->path.end() - 1; it++) path += std::string(it->file_name) + "/";

    // If the last component is not the root it formats to a path string without the "/" at the end
    if (this->path.size() > 1) path += std::string(it->file_name);
    return path;
}

// Parses every file name into a vector
bool FS::Path::parsePath(const std::string& paths, std::vector<std::string>& pathv) {
    pathv.clear();
    if (paths.size() == 0) return false;

    // Checks for absolute path
    size_t index;
    if (paths[0] == '/') {
        pathv.emplace_back("/");
        index = 1;
    } else {
        index = 0;
    }

    // Parses every fileName into a vector
    std::string fileName = "";
    for (; index < paths.size(); index++) {
        if (paths[index] == '/' || paths[index] == '\n') {
            // Checks that fileName is valid
            if (fileName.size() == 0 || fileName[0] == 0) return false;
            // Adds fileName to vector
            pathv.emplace_back(fileName);
            if (paths[index] == '\n') return true;
            fileName = "";
        } else {
            fileName += paths[index];
        }
    }
    // If leftover check that fileName is valid and add
    if (fileName.size() > 0) {
        if (fileName[0] == 0) return false;
        pathv.emplace_back(fileName);
    }

    return true;
}

// Finds entry in path and changes it to new data
void FS::Path::updatePathEntry(const dir_entry& entry, dir_entry newData) {
    // Finds correct entry and changes it
    for (dir_entry& dir : this->path) {
        if (dir.first_blk == entry.first_blk) {
            dir = newData;
            return;
        }
    }
}

// -----------------HELPER FUNCTIONS-----------------

// Wrapper for disk.read() to make read operations safer and less verbose
inline void FS::read(const int16_t block, dir_block& dirBlock) { this->disk.read(block, (uint8_t*)dirBlock.data()); }

// Wrapper for disk.read() to make read operations safer and less verbose
inline void FS::read(const int16_t block, std::array<char, BLOCK_SIZE>& fileBlock) {
    this->disk.read(block, (uint8_t*)fileBlock.data());
}

// Wrapper for disk.read() for reading fat to memory
inline void FS::readFat() { this->disk.read(FAT_BLOCK, (uint8_t*)this->fat); }

// Wrapper for disk.write() to make write operations safer and less verbose
inline void FS::write(const int16_t block, const dir_block& dirBlock) {
    this->disk.write(block, (uint8_t*)dirBlock.data());
}

// Wrapper for disk.write() to make write operations safer and less verbose
inline void FS::write(const int16_t block, const std::array<char, BLOCK_SIZE>& fileBlock) {
    this->disk.write(block, (uint8_t*)fileBlock.data());
}

// Wrapper for disk.write() for writing fat to memory
inline void FS::writeFat() { this->disk.write(FAT_BLOCK, (uint8_t*)this->fat); }

// Returns whether dir entry is free or not by checking if file_name starts with NULL terminator
inline bool FS::isNotFreeEntry(const dir_entry& dir) { return dir.file_name[0] != 0; }

// Returns index of FAT_FREE slot or -1 if there is none
int FS::getEmptyFat() const {
    for (uint16_t i = 2; i < FS::FAT_SIZE; i++)
        if (this->fat[i] == FAT_FREE) return i;
    return -1;
}

// Reserves enough FAT blocks to fit size bytes
int16_t FS::reserve(size_t size) {
    // Calculates amount of needed nodes and sets the first node as occupied in the FAT table
    int neededNodes = (size + (BLOCK_SIZE - 1)) / BLOCK_SIZE;
    int16_t firstNode = this->getEmptyFat();
    if (firstNode == -1) return false;
    this->fat[firstNode] = FAT_EOF;

    // Sets up FAT linked list
    int16_t prevNode = firstNode;
    for (int i = 0; i < (neededNodes - 1); i++) {
        int16_t newNode = this->getEmptyFat();

        // Links previous node to new node
        if (newNode != -1) {
            this->fat[prevNode] = newNode;
            this->fat[newNode] = FAT_EOF;
            prevNode = newNode;

            // Breaks the connection if there is no space for the new node
        } else {
            free(firstNode);
            return -1;
        }
    }

    return firstNode;
}

// Frees the linked lists FAT entries by setting them to FAT_FREE
void FS::free(int16_t fatStart) {
    int16_t fatIndex = fatStart;
    while (fatIndex != FAT_EOF) {
        int16_t temp = fatIndex;
        fatIndex = this->fat[fatIndex];
        this->fat[temp] = FAT_FREE;
    }
}

// Takes in a directory and a new entry and then sets the directory there
bool FS::addDirEntry(const dir_entry& dir, dir_entry newEntry) {
    // Validity checks
    dir_entry temp;
    if (std::string("").compare(newEntry.file_name) == 0) return false;
    if (this->workingPath.searchDir(dir, std::string(newEntry.file_name), temp)) {
        return false;
    }

    // Goes through the FAT until it reaches the FAT_EOF
    int16_t fatIndex = dir.first_blk;
    while (this->fat[fatIndex] != FAT_EOF) {
        fatIndex = this->fat[fatIndex];
    }

    // Reads in the last block in the directory
    dir_block dirBlock{};
    this->read(fatIndex, dirBlock);

    // Looks for the first free slot in the block
    int dirEntryIndexInBlock;
    for (dirEntryIndexInBlock = 0; dirEntryIndexInBlock < FS::DIR_BLK_SIZE; dirEntryIndexInBlock++) {
        if (!isNotFreeEntry(dirBlock[dirEntryIndexInBlock])) break;
    }

    // Adds a new FAT block if we don't have enough space in the directory for the dir_entry
    if (dirEntryIndexInBlock == FS::DIR_BLK_SIZE) {
        dirEntryIndexInBlock = 0;
        int16_t newDirBlock = this->getEmptyFat();
        if (newDirBlock == -1) {
            return false;
        }
        this->fat[fatIndex] = newDirBlock;
        this->fat[newDirBlock] = FAT_EOF;
        fatIndex = this->fat[fatIndex];

        // Add metadata for new file
        dir_block freshDirBlock{};
        freshDirBlock[dirEntryIndexInBlock] = newEntry;
        this->write(fatIndex, freshDirBlock);

        // If there is enough space in the directory
    } else {
        // Add metadata for new file
        dirBlock[dirEntryIndexInBlock] = newEntry;
        this->write(fatIndex, dirBlock);
    }

    // Writes updates to the FAT block
    this->writeFat();
    return true;
}

// Removes entry by searching through the directory and then replaces it with the last entry
bool FS::removeDirEntry(dir_entry& dir, std::string fileName) {
    int16_t entryFatIndex;
    int removeDirEntryIndex;
    dir_entry entryToRemove;

    // Looks for the entry and saves the FAT table index, the entry and its index in the FAT block
    if (!this->workingPath.searchDir(dir, fileName, entryToRemove, entryFatIndex, removeDirEntryIndex)) {
        return false;
    }

    // Goes through the FAT until it reaches the FAT_EOF
    int16_t fatIndex = dir.first_blk;
    while (this->fat[fatIndex] != FAT_EOF) {
        fatIndex = this->fat[fatIndex];
    }

    // Reads in the last block in the directory
    dir_block dirBlock{};
    this->read(fatIndex, dirBlock);

    // Looks for the last non free slot in the block
    int16_t dirEntryIndexInBlock;
    for (dirEntryIndexInBlock = 0; dirEntryIndexInBlock < FS::DIR_BLK_SIZE; dirEntryIndexInBlock++) {
        if (!isNotFreeEntry(dirBlock[dirEntryIndexInBlock])) break;
    }
    dirEntryIndexInBlock--;

    // Marks last free slot
    dir_entry entryToMove = dirBlock[dirEntryIndexInBlock];
    dirBlock[dirEntryIndexInBlock].file_name[0] = 0;
    this->write(fatIndex, dirBlock);

    // Move last data to space where the entry was removed
    if (dirEntryIndexInBlock != removeDirEntryIndex || fatIndex != entryFatIndex) {
        this->read(entryFatIndex, dirBlock);
        dirBlock[removeDirEntryIndex] = entryToMove;
        this->write(entryFatIndex, dirBlock);
    }

    // Updates the FAT_EOF
    if (dirEntryIndexInBlock == 0) {
        this->fat[fatIndex] = FAT_FREE;
        while (this->fat[this->fat[fatIndex]] != FAT_FREE) {
            fatIndex = this->fat[fatIndex];
        }
        this->fat[fatIndex] = FAT_EOF;
    }

    // Writes updates to the FAT block
    this->writeFat();
    return true;
}

// Adds directory entry and data
bool FS::__create(const dir_entry& dir, dir_entry& metadata, const std::string& data) {
    // Reserves space that the new file needs
    int16_t startfat = this->reserve(data.size());
    if (startfat == -1) return false;

    // Sets metadata for the file
    metadata.first_blk = startfat;
    metadata.size = data.size();

    // Frees the entry if it fails
    if (!this->addDirEntry(dir, metadata)) {
        this->free(startfat);
        return false;
    }

    // If the entry is a directory the . and .. entries get added to data
    std::string totalData;
    if (metadata.type == TYPE_DIR) {
        dir_entry dotAndDotDot[] = {metadata, dir};
        totalData = std::string((char*)dotAndDotDot, sizeof(dotAndDotDot));
        for (int i = 0; i < sizeof("."); i++) totalData[i] = "."[i];
        for (int i = 0; i < sizeof(".."); i++) totalData[sizeof(dir_entry) + i] = ".."[i];
    }
    totalData += data;

    // Writes the data to the new entry
    size_t pos = 0;
    int16_t fatIndex = metadata.first_blk;
    while (fatIndex != FAT_EOF) {
        file_block buffer{};
        totalData.copy(buffer.data(), BLOCK_SIZE, pos);
        this->write(fatIndex, buffer);
        pos += BLOCK_SIZE;
        fatIndex = this->fat[fatIndex];
    }
    this->writeFat();

    return true;
}
