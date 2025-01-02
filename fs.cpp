#include "fs.h"

#include <stdint.h>

#include <cstring>
#include <iostream>
#include <vector>


size_t parse(std::string path_and_data, std::vector<std::string>& path, std::string& data) {
    size_t index = 0;
    path.clear();
    data.clear();
    if (path_and_data.size() == 0) return 0;

    std::string fileName = "";

    while(index < path_and_data.size()){
        if (path_and_data[index] == '/'){
            path.emplace_back(fileName);
            fileName = "";
        }
        else if (path_and_data[index] == '\n' && (index + 1) < path_and_data.size() && path_and_data[index + 1] == '\n') {
            if (fileName.size() > 0)
                path.emplace_back(fileName);
            data = path_and_data.substr(index + 2);
            return path_and_data.size() - index - 2;
        }    
        else fileName += path_and_data[index];
        index++;
    }
    if (fileName.size() > 0)
        path.emplace_back(fileName);
    return 0;
}

int16_t FS::reserve(size_t size) {
    size_t neededNodes = (size + (BLOCK_SIZE - 1)) / BLOCK_SIZE;
    std::vector<int> fatNodes;
    int16_t firstNode = this->getEmptyFat();
    if (firstNode == -1) return false;
    fatNodes.emplace_back(firstNode);
    this->fat[firstNode] = FAT_EOF;

    int16_t prevNode = firstNode;
    for (int i  = 0; i < neededNodes - 1; i++){
        int16_t newNode = this->getEmptyFat();
        if (newNode != -1){
            this->fat[prevNode] = newNode;
            this->fat[newNode] = FAT_EOF;
            prevNode = newNode;
            fatNodes.emplace_back(newNode);

        }
        else { 
            for (int fatIndex : fatNodes){
                this->fat[fatIndex] = FAT_FREE;
            }
            return -1; 
        }
    }
    return firstNode;
}

void FS::free(int16_t fatStart){
    int16_t fatIndex = fatStart;
    while ( fatIndex != FAT_EOF ){
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

bool FS::__cd(dir_entry& workingDir, const std::string& path, bool createDirs){
    size_t index = 0;
    std::string nextDir;
    while(index < path.size()){
        if ( path[index] == '/' ){
            if (!this->findDirEntry(workingDir, nextDir, workingDir)){
                if (createDirs){
                    //throw std::runtime_error("Nah");
                    dir_entry newDir {
                        .type = TYPE_DIR,
                        .access_rights = WRITE | READ,
                    };
                    strncpy(newDir.file_name, nextDir.c_str(), 56);
                    if (!this->__create(workingDir, newDir, "")) return false;
                    if (!this->findDirEntry(workingDir, nextDir, workingDir)) 
                        throw std::runtime_error("Thingy that should not possibly have failed failed, How???? Line:" + std::to_string(__LINE__) + " " + "File: " + __FILE__);
                }
                else return false;
            } 
            nextDir = "";
        } else {
            nextDir += path[index];
        }
        index++;
    }
    if (nextDir.size() > 0) {
        if (!this->findDirEntry(workingDir, nextDir, workingDir)) return false;
    }
    return true;
}

bool FS::__cd(dir_entry& workingDir, const std::vector<std::string>& path, bool createDirs){
    for (std::string file : path){
        if(!this->findDirEntry(workingDir, file, workingDir)){
            if (createDirs) {
                //throw std::runtime_error("Nah");
                dir_entry newDir {
                    .type = TYPE_DIR,
                    .access_rights = WRITE | READ,
                };
                strncpy(newDir.file_name, file.c_str(), 56);
                if (!this->__create(workingDir, newDir, "")) return false;
                if (!this->findDirEntry(workingDir, file, workingDir)) 
                    throw std::runtime_error("Thingy that should not possibly have failed failed, How???? Line:" + std::to_string(__LINE__) + " " + "File: " + __FILE__);
            }
            else return false;
        }
    }
    return true;
}

bool FS::__writeData(int16_t startFat, std::string data, int16_t ofset){
    int16_t fatAvailable = BLOCK_SIZE - ofset;
    int fatIndex = startFat;
    while (this->fat[fatIndex] != FAT_EOF){
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
    while(pos < data.length()){
        data.copy(buffer, BLOCK_SIZE, pos);
        this->disk.write(fatIndex, (uint8_t*)buffer);
        pos += BLOCK_SIZE;
    }
    return true;
}

bool FS::__create(dir_entry dir, dir_entry metadata, std::string data){
    int fatIndex = dir.first_blk;
    std::array<dir_entry, 64> dirBlock;

    while (this->fat[fatIndex] != FAT_EOF){
        fatIndex = this->fat[fatIndex];
    }

    int16_t dirEntryIndexInBlock = (dir.size & (BLOCK_SIZE - 1)) / 64;

    // reserve space that the new file needs
    int16_t startfat = this->reserve(data.length());
    if (startfat == -1) return false;

    // set metadata for the file
    metadata.first_blk = startfat;
    metadata.size = data.length();
    if (metadata.type == TYPE_DIR) metadata.size += 128;

    // if we don't have enough space in our directory for the dir_entry
    if (dirEntryIndexInBlock == 0) { // I would say trust but don't the magicc might no be magiccing
        int16_t newDirBlock = this->getEmptyFat();
        if (newDirBlock == -1){
            this->free(startfat);
            return false;
        } 
        this->fat[fatIndex] = newDirBlock;
        fatIndex = this->fat[fatIndex];
        this->fat[fatIndex] = FAT_EOF;
    }

    // add metadata for new file
    this->disk.read(fatIndex, (uint8_t*)dirBlock.data());
    dirBlock[dirEntryIndexInBlock] = metadata;
    this->disk.write(fatIndex, (uint8_t*)dirBlock.data());

    //std::cout << fatIndex << " " << dir.first_blk << " " << dirEntryIndexInBlock << " - - - -\n";

    // update metadata for directory
    this->disk.read(dir.first_blk, (uint8_t*)dirBlock.data());
    dirBlock[0].size += 64;
    this->disk.write(dir.first_blk, (uint8_t*)dirBlock.data());

    // write the data to the new file
    std::string extra;
    if (metadata.type == TYPE_DIR){
        for (int i = 0; i < 128; i++) extra += (char)0;
        extra[0] = '.';
        extra[64] = '.';
        extra[65] = '.';
        for ( int i = 0; i < 64 - 56; i++){
            extra[i + 56] = ((char*)&metadata)[i + 56];
            extra[64 + i + 56] = ((char*)&dir)[i + 56];
        }
    }

    // std::cout << "-------------------\n";
    // for (char c : extra){
    //     std::cout << c;
    // }
    
    // std::cout << "\n----------------\n";

    // std::cout << std::string(extra + data);

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
                                     .size = 64,
                                     .first_blk = 0,
                                     .type = TYPE_DIR,
                                     .access_rights = READ | WRITE,
                                 }};

    this->disk.write(ROOT_BLOCK, (uint8_t*)&directories);
    this->disk.write(FAT_BLOCK, (uint8_t*)&(this->fat));
    this->workingDir = directories[0];
    return 0;
}

// create <filepath> creates a new file on the disk, the data content is
// written on the following rows (ended with an empty row)
int FS::create(std::string filepath) {
    std::cout << "FS::create(" << filepath << ")\n";

    std::string data;
    std::vector<std::string> path;
    size_t dataSize = parse(filepath, path, data);
    dir_entry currentDir = this->workingDir;

    if (path.size() < 1) return false;

    std::vector<std::string> some(path.begin(), path.end() - 1);

    this->__cd(currentDir, std::vector<std::string>(path.begin(), path.end() - 1));

    dir_entry newFile{
        .type = TYPE_FILE,
        .access_rights = READ | WRITE,
    };
    strncpy(newFile.file_name, path.back().c_str(), 56);

    this->__create(workingDir, newFile, data);

    //std::cout << path << "\n" << data << "\n" << dataSize << "\n";
}

// cat <filepath> reads the content of a file and prints it on the screen
int FS::cat(std::string filepath) {
    dir_entry currentDir = this->workingDir;
    this->__cd(currentDir, filepath);
    int16_t fatIndex = currentDir.first_blk;
    // while(this->fat[fatIndex] != FAT_EOF){
    //     char dirBlock[BLOCK_SIZE];
    //     this->disk.read(fatIndex, (uint8_t*)dirBlock);
    //     std::cout << dirBlock;
    //     fatIndex = this->fat[fatIndex];
    // }
    // char dirBlock[BLOCK_SIZE]{NULL};
    // this->disk.read(fatIndex, (uint8_t*)dirBlock);
    // std::cout << dirBlock << "\n";

    while(fatIndex != FAT_EOF){
        char dirBlock[BLOCK_SIZE];
        this->disk.read(fatIndex, (uint8_t*)dirBlock);
        fatIndex = this->fat[fatIndex];
    }
    return 0;
}

// ls lists the content in the currect directory (files and sub-directories)
int FS::ls() {
    int16_t fatIndex = ROOT_BLOCK;
    std::array<dir_entry, 64> dirBlock;
    std::string print("name     size\n");
    this->disk.read(fatIndex, (uint8_t*)dirBlock.data());
    int dirSize = dirBlock[0].size;
    size_t numberOfFatblocks = (dirSize + (BLOCK_SIZE - 1) / BLOCK_SIZE);

    bool firstBlock = true;
    while (this->fat[fatIndex] != FAT_EOF){
        this->disk.read(fatIndex, (uint8_t*)dirBlock.data());
        for (size_t j = firstBlock; j < 64; j++){
            print += dirBlock[j].file_name + std::string(" ") + std::to_string(dirBlock[j].size) + "\n";
        }
        firstBlock = false;
        fatIndex = this->fat[fatIndex];
    }

    this->disk.read(fatIndex, (uint8_t*)dirBlock.data());
    for (size_t i = firstBlock; i < (dirSize & (BLOCK_SIZE - 1)) / 64; i++){
        print += dirBlock[i].file_name + std::string(" ") + std::to_string(dirBlock[i].size) + "\n";
    }

    std::cout << print;


    // int i = 1;
    // while (dirSize != 0) {
    //     this->disk.read(fatIndex, (uint8_t*)dirBlock.data());
    //     print.reserve(print.capacity() + 70);
    //     for (; i < (dirSize & (BLOCK_SIZE - 1)) / 64; i++) {
    //         print.append(dirBlock[i].file_name, 56);
    //         print += " " + std::to_string(dirBlock[i].size);
    //         print += '\n';
    //     }
    //     i = 0;
    //     dirSize -= (dirSize & (BLOCK_SIZE - 1));
    //     fatIndex = this->fat[fatIndex];
    // }
    // std::cout << print;
    // return 0;
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
