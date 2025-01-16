#include <array>
#include <cstdint>
#include <iostream>
#include <vector>

#include "disk.h"

#ifndef __FS_H__
#define __FS_H__

#define ROOT_BLOCK 0
#define FAT_BLOCK 1
#define FAT_FREE 0
#define FAT_EOF -1

#define TYPE_FILE 0
#define TYPE_DIR 1
#define READ 0x04
#define WRITE 0x02
#define EXECUTE 0x01

struct dir_entry {
    char file_name[56];     // name of the file / sub-directory
    uint32_t size;          // size of the file in bytes
    uint16_t first_blk;     // index in the FAT for the first block of the file
    uint8_t type;           // directory (1) or file (0)
    uint8_t access_rights;  // read (0x04), write (0x02), execute (0x01)
};

class FS {
   public:
    FS();
    ~FS();
    // formats the disk, i.e., creates an empty file system
    int format();
    // create <filepath> creates a new file on the disk, the data content is
    // written on the following rows (ended with an empty row)
    int create(std::string filepath);
    // cat <filepath> reads the content of a file and prints it on the screen
    int cat(std::string filepath);
    // ls lists the content in the current directory (files and sub-directories)
    int ls();

    // cp <sourcepath> <destpath> makes an exact copy of the file
    // <sourcepath> to a new file <destpath>
    int cp(std::string sourcepath, std::string destpath);
    // mv <sourcepath> <destpath> renames the file <sourcepath> to the name
    // <destpath>, or moves the file <sourcepath> to the directory <destpath>
    // (if dest is a directory)
    int mv(std::string sourcepath, std::string destpath);
    // rm <filepath> removes / deletes the file <filepath>
    int rm(std::string filepath);
    // append <filepath1> <filepath2> appends the contents of file <filepath1>
    // to the end of file <filepath2>. The file <filepath1> is unchanged.
    int append(std::string filepath1, std::string filepath2);

    // mkdir <dirpath> creates a new sub-directory with the name <dirpath>
    // in the current directory
    int mkdir(std::string dirpath);
    // cd <dirpath> changes the current (working) directory to the directory
    // named <dirpath>
    int cd(std::string dirpath);
    // pwd prints the full path, i.e., from the root directory, to the current
    // directory, including the current directory name
    int pwd();

    // chmod <accessrights> <filepath> changes the access rights for the
    // file <filepath> to <accessrights>.
    int chmod(std::string accessrights, std::string filepath);

   private:
    /// @brief Helper class to handle paths for the file system.
    class Path {
       public:
        /// @brief Binds path to disk and FAT and initilizes working directory.
        /// @param disk The disk.
        /// @param fat The FAT.
        Path(Disk* disk, int16_t* fat);

        Path(const Path& other) = default;

        Path& operator=(const Path& other) = default;

        /// @brief Finds the directory entry for the given path.
        /// @param path The path to the directory entry.
        /// @param result The directory entry the path points to.
        /// @return True if found else false.
        bool find(const std::string& path, dir_entry& result) const;

        /// @brief Finds the directory entry for the given path up to before the last component.
        /// @param path The path to parse.
        /// @param result Directory entry before the last component in the path.
        /// @param last Last component in the path.
        /// @return True if found else false.
        bool findUpToLast(const std::string& path, dir_entry& result, std::string& last) const;

        /// @brief Searches for a specified file in the directory.
        /// @param dir The directory to search.
        /// @param fileName The file name to be searched for.
        /// @param result The directory entry to the found file/directory.
        /// @return True if found else false.
        inline bool searchDir(const dir_entry& dir, const std::string& fileName, dir_entry& result) const;

        /// @brief Searches for a specified file in the directory.
        /// @param dir The directory to search.
        /// @param fileName The file name to be searched for.
        /// @param result The directory entry to the found file/directory.
        /// @param fatIndex The index to the FAT block that @result was found in.
        /// @param blockIndex The index in the FAT block that points to @result.
        /// @return True if found else false.
        bool searchDir(const dir_entry& dir, const std::string& fileName, dir_entry& result, int16_t& fatIndex,
                       int& blockIndex) const;

        /// @brief Adds path to current path.
        /// @param path The path to add.
        /// @return True if succeeded else false.
        bool cd(const std::string& path);

        /// @return Entry for working directory.
        inline const dir_entry& workingDir() const { return this->path.back(); }

        /// @return Root entry.
        inline const dir_entry& rootDir() const { return this->path.front(); }

        /// @brief Formats the working path to a string.
        /// @return Formatted path.
        std::string pwd() const;

        /// @brief Parses every file name into a vector.
        /// @param paths String path.
        /// @param pathv Parsed path.
        /// @return True if succeeded else false.
        static bool parsePath(const std::string& paths, std::vector<std::string>& pathv);

        /// @brief Finds entry in path and changes it to new data.
        /// @param entry The wanted entry.
        /// @param newData The new data to be set.
        void updatePathEntry(const dir_entry& entry, dir_entry newData);

       private:
        Disk* disk;
        int16_t* fat;
        std::vector<dir_entry> path;
    };

    Disk disk;
    int16_t fat[BLOCK_SIZE / 2];
    Path workingPath;

    /// @brief Returns whether dir entry is free or not by checking if file_name starts with NULL terminator.
    /// @param dir The directory entry to check.
    /// @return True if not free else false.
    inline static bool isNotFreeEntry(const dir_entry& dir);

    /// @brief Return the Fat index to an empty file slot.
    /// @return Index or -1 if no empty slots.
    int getEmptyFat() const;

    /// @brief Reserves enough FAT blocks to fit size bytes.
    /// @param size Amount of bytes the FAT link should reserve.
    /// @return -1 if failed else first node index in FAT.
    int16_t reserve(size_t size);

    /// @brief Frees the linked lists FAT entries by setting them to FAT_FREE.
    /// @param fatStart The start of the FAT linked list.
    void free(int16_t fatStart);

    /// @brief Takes in a directory and a new entry and then sets the directory there.
    /// @param dir The directory to add an entry to.
    /// @param newEntry The new entry to be added.
    /// @return True if succeeded else false.
    bool addDirEntry(const dir_entry& dir, dir_entry newEntry);

    /// @brief Removes entry by searching through the directory and then replaces it with the last entry.
    /// @param dir The directory to remove an entry in.
    /// @param fileName The file name of the file to be removed.
    /// @return True if succeeded else false.
    bool removeDirEntry(dir_entry& dir, std::string fileName);

    /// @brief Adds directory entry and data.
    /// @param dir The directory to add an entry in.
    /// @param filedata The metadata of the entry.
    /// @param data The data of the entry.
    /// @return
    bool __create(const dir_entry& dir, dir_entry& filedata, const std::string& data);
};

#endif  // __FS_H__
