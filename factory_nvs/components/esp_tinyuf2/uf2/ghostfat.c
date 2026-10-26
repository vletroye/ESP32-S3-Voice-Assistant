/*
 * The MIT License (MIT)
 *
 * Copyright (c) Microsoft Corporation
 * Copyright (c) Ha Thach for Adafruit Industries
 * Copyright (c) Henry Gabryjelski
 * Copyright (c) Espressif
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <inttypes.h>
#include "compile_date.h"
#include "board_flash.h"
#include "uf2.h"

//--------------------------------------------------------------------+
//
//--------------------------------------------------------------------+

// ota0 partition size
static uint32_t _flash_size;

#define STATIC_ASSERT(_exp) _Static_assert(_exp, "static assert failed")

#define STR0(x) #x
#define STR(x) STR0(x)

#if defined(__GNUC__)
#define UF2_NONSTRING __attribute__((nonstring))
#else
#define UF2_NONSTRING
#endif

#define UF2_ARRAY_SIZE(_arr)    ( sizeof(_arr) / sizeof(_arr[0]) )
#define UF2_DIV_CEIL(_v, _d)    ( ((_v) / (_d)) + ((_v) % (_d) ? 1 : 0) )

typedef struct {
    uint8_t JumpInstruction[3];
    uint8_t OEMInfo[8] UF2_NONSTRING;
    uint16_t SectorSize;
    uint8_t SectorsPerCluster;
    uint16_t ReservedSectors;
    uint8_t FATCopies;
    uint16_t RootDirectoryEntries;
    uint16_t TotalSectors16;
    uint8_t MediaDescriptor;
    uint16_t SectorsPerFAT;
    uint16_t SectorsPerTrack;
    uint16_t Heads;
    uint32_t HiddenSectors;
    uint32_t TotalSectors32;
    uint8_t PhysicalDriveNum;
    uint8_t Reserved;
    uint8_t ExtendedBootSig;
    uint32_t VolumeSerialNumber;
    uint8_t VolumeLabel[11] UF2_NONSTRING;
    uint8_t FilesystemIdentifier[8] UF2_NONSTRING;
} __attribute__((packed)) FAT_BootBlock;

typedef struct {
    char name[8];
    char ext[3];
    uint8_t attrs;
    uint8_t reserved;
    uint8_t createTimeFine;
    uint16_t createTime;
    uint16_t createDate;
    uint16_t lastAccessDate;
    uint16_t highStartCluster;
    uint16_t updateTime;
    uint16_t updateDate;
    uint16_t startCluster;
    uint32_t size;
} __attribute__((packed)) DirEntry;
STATIC_ASSERT(sizeof(DirEntry) == 32);

typedef struct FileContent {
    char name[11] UF2_NONSTRING;    // [Voice assistant] the phrase files get their name at start-up
    void const * content;
    uint32_t size;       // OK to use uint32_T b/c FAT32 limits filesize to (4GiB - 2)
    // [Voice assistant] clusters kept for the file: an edited file can grow in place (else what Windows
    // writes past the clusters of the original content is lost)
    uint32_t reserved;

    // computing fields based on index and size
    uint16_t cluster_start;
    uint16_t cluster_end;
} FileContent_t;

//--------------------------------------------------------------------+
//
//--------------------------------------------------------------------+

#define BPB_SECTOR_SIZE           ( 512)
#define BPB_SECTORS_PER_CLUSTER   (CFG_UF2_SECTORS_PER_CLUSTER)
#define BPB_RESERVED_SECTORS      (   1)
#define BPB_NUMBER_OF_FATS        (   2)
#define BPB_ROOT_DIR_ENTRIES      (  64)
#define BPB_TOTAL_SECTORS         CFG_UF2_NUM_BLOCKS
#define BPB_MEDIA_DESCRIPTOR_BYTE (0xF8)
#define FAT_ENTRY_SIZE            (2)
#define FAT_ENTRIES_PER_SECTOR    (BPB_SECTOR_SIZE / FAT_ENTRY_SIZE)
#define FAT_END_OF_CHAIN          (0xFFFF)

// NOTE: MS specification explicitly allows FAT to be larger than necessary
#define TOTAL_CLUSTERS_ROUND_UP   UF2_DIV_CEIL(BPB_TOTAL_SECTORS, BPB_SECTORS_PER_CLUSTER)
#define BPB_SECTORS_PER_FAT       UF2_DIV_CEIL(TOTAL_CLUSTERS_ROUND_UP, FAT_ENTRIES_PER_SECTOR)
#define DIRENTRIES_PER_SECTOR     (BPB_SECTOR_SIZE/sizeof(DirEntry))
#define ROOT_DIR_SECTOR_COUNT     UF2_DIV_CEIL(BPB_ROOT_DIR_ENTRIES, DIRENTRIES_PER_SECTOR)
#define BPB_BYTES_PER_CLUSTER     (BPB_SECTOR_SIZE * BPB_SECTORS_PER_CLUSTER)

STATIC_ASSERT((BPB_SECTORS_PER_CLUSTER & (BPB_SECTORS_PER_CLUSTER - 1)) == 0); // sectors per cluster must be power of two
STATIC_ASSERT(BPB_SECTOR_SIZE                              ==       512); // GhostFAT does not support other sector sizes (currently)
STATIC_ASSERT(BPB_NUMBER_OF_FATS                           ==         2); // FAT highest compatibility
STATIC_ASSERT(sizeof(DirEntry)                             ==        32); // FAT requirement
STATIC_ASSERT(BPB_SECTOR_SIZE % sizeof(DirEntry)           ==         0); // FAT requirement
STATIC_ASSERT(BPB_ROOT_DIR_ENTRIES % DIRENTRIES_PER_SECTOR ==         0); // FAT requirement
STATIC_ASSERT(BPB_BYTES_PER_CLUSTER                        <= (32 * 1024)); // FAT requirement (64k+ has known compatibility problems)
STATIC_ASSERT(FAT_ENTRIES_PER_SECTOR                       ==       256); // FAT requirement

#define UF2_FIRMWARE_BYTES_PER_SECTOR   256
#define UF2_SECTOR_COUNT                (_flash_size / UF2_FIRMWARE_BYTES_PER_SECTOR)
#define UF2_BYTE_COUNT                  (UF2_SECTOR_COUNT * BPB_SECTOR_SIZE) // always a multiple of sector size, per UF2 spec

#ifdef TINYUF2_FAVICON_HEADER
#include TINYUF2_FAVICON_HEADER
const char autorunFile[] = "[Autorun]\r\nIcon=FAVICON.ICO\r\n";
#endif

// size of CURRENT.UF2:
static FileContent_t info[] = {
    // [Voice assistant] INFO_UF2.TXT and INDEX.HTM of TinyUF2 left out (no use here): unnamed, so not listed
    {.name = "", .content = NULL, .size = 0  },
    {.name = "", .content = NULL, .size = 0  },
    {.name = "CONFIG  INI", .content = NULL, .size = 0  },
    {.name = "PERSONA TXT", .content = NULL, .size = 0  },
    {.name = "MEMORY  TXT", .content = NULL, .size = 0  },
    // [Voice assistant] AGENTS.INI and the phrase files (VOICE_FR.TXT...), named at start-up; not listed while unnamed
    {.name = "", .content = NULL, .size = 0  },
    {.name = "", .content = NULL, .size = 0  },
    {.name = "", .content = NULL, .size = 0  },
    {.name = "", .content = NULL, .size = 0  },
    {.name = "", .content = NULL, .size = 0  },
    {.name = "", .content = NULL, .size = 0  },
#ifdef TINYUF2_FAVICON_HEADER
    {.name = "AUTORUN INF", .content = autorunFile, .size = sizeof(autorunFile) - 1},
    {.name = "FAVICON ICO", .content = favicon_data, .size = favicon_len            },
#endif
    // current.uf2 must be the last element and its content must be NULL. [Voice assistant] Not listed: no firmware
    // is read nor written through the drive (no OTA partition given), it is only the free space after the files
    {.name = "", .content = NULL, .size = 0                       },
};

enum {
    NUM_FILES = sizeof(info) / sizeof(info[0]),
    FID_UF2 = NUM_FILES - 1,
    FID_INI = 2,
    FID_PERSONA = 3,    // FID_PERSONA + UF2_TEXT_xxx: the text files
    FID_TEXT_LAST = FID_PERSONA + UF2_TEXT_COUNT - 1,
    NUM_DIRENTRIES = NUM_FILES + 1 // including volume label as first root directory entry
};

STATIC_ASSERT(NUM_DIRENTRIES < BPB_ROOT_DIR_ENTRIES);  // FAT requirement -- Ensures BPB reserves sufficient entries for all files
STATIC_ASSERT(NUM_DIRENTRIES < DIRENTRIES_PER_SECTOR); // GhostFAT bug workaround -- else, code overflows buffer

#define NUM_SECTORS_IN_DATA_REGION (BPB_TOTAL_SECTORS - BPB_RESERVED_SECTORS - (BPB_NUMBER_OF_FATS * BPB_SECTORS_PER_FAT) - ROOT_DIR_SECTOR_COUNT)
#define CLUSTER_COUNT              (NUM_SECTORS_IN_DATA_REGION / BPB_SECTORS_PER_CLUSTER)

// Ensure cluster count results in a valid FAT16 volume!
STATIC_ASSERT(CLUSTER_COUNT >= 0x0FF5 && CLUSTER_COUNT < 0xFFF5);

// Many existing FAT implementations have small (1-16) off-by-one style errors
// So, avoid being within 32 of those limits for even greater compatibility.
STATIC_ASSERT(CLUSTER_COUNT >= 0x1015 && CLUSTER_COUNT < 0xFFD5);

#define FS_START_FAT0_SECTOR      BPB_RESERVED_SECTORS
#define FS_START_FAT1_SECTOR      (FS_START_FAT0_SECTOR + BPB_SECTORS_PER_FAT)
#define FS_START_ROOTDIR_SECTOR   (FS_START_FAT1_SECTOR + BPB_SECTORS_PER_FAT)
#define FS_START_CLUSTERS_SECTOR  (FS_START_ROOTDIR_SECTOR + ROOT_DIR_SECTOR_COUNT)

//--------------------------------------------------------------------+
//
//--------------------------------------------------------------------+

static FAT_BootBlock const BootBlock = {
    .JumpInstruction      = {0xeb, 0x3c, 0x90},
    .OEMInfo              = "UF2 UF2 ",
    .SectorSize           = BPB_SECTOR_SIZE,
    .SectorsPerCluster    = BPB_SECTORS_PER_CLUSTER,
    .ReservedSectors      = BPB_RESERVED_SECTORS,
    .FATCopies            = BPB_NUMBER_OF_FATS,
    .RootDirectoryEntries = BPB_ROOT_DIR_ENTRIES,
    .TotalSectors16       = (BPB_TOTAL_SECTORS > 0xFFFF) ? 0 : BPB_TOTAL_SECTORS,
    .MediaDescriptor      = BPB_MEDIA_DESCRIPTOR_BYTE,
    .SectorsPerFAT        = BPB_SECTORS_PER_FAT,
    .SectorsPerTrack      = 1,
    .Heads                = 1,
    .TotalSectors32       = (BPB_TOTAL_SECTORS > 0xFFFF) ? BPB_TOTAL_SECTORS : 0,
    .PhysicalDriveNum     = 0x80, // to match MediaDescriptor of 0xF8
    .ExtendedBootSig      = 0x29,
    .VolumeSerialNumber   = 0x00420042,
    .VolumeLabel          = UF2_VOLUME_LABEL,
    .FilesystemIdentifier = "FAT16   ",
};

//--------------------------------------------------------------------+
//
//--------------------------------------------------------------------+

static inline bool is_uf2_block(UF2_Block const *bl)
{
    return (bl->magicStart0 == UF2_MAGIC_START0) &&
           (bl->magicStart1 == UF2_MAGIC_START1) &&
           (bl->magicEnd == UF2_MAGIC_END) &&
           (bl->flags & UF2_FLAG_FAMILYID) &&
           !(bl->flags & UF2_FLAG_NOFLASH);
}

// cache the cluster start offset for each file
// this allows more flexible algorithms w/o O(n) time
static void init_starting_clusters(void)
{
    // +2 because FAT decided first data sector would be in cluster number 2, rather than zero
    uint16_t start_cluster = 2;

    for (uint16_t i = 0; i < NUM_FILES; i++) {
        info[i].cluster_start = start_cluster;
        uint32_t size = info[i].reserved > info[i].size ? info[i].reserved : info[i].size;
        info[i].cluster_end = start_cluster + UF2_DIV_CEIL(size, BPB_SECTOR_SIZE * BPB_SECTORS_PER_CLUSTER) - 1;

        start_cluster = info[i].cluster_end + 1;
    }

    return;
}

// get file index for file that uses the cluster
// if cluster is past last file, returns ( NUM_FILES-1 ).
//
// Caller must still check if a particular *sector*
// contains data from the file's contents, as there
// are often padding sectors, including all the unused
// sectors past the end of the media.
static uint32_t info_index_of(uint32_t cluster)
{
    // default results for invalid requests is the index of the last file (CURRENT.UF2)
    if (cluster >= 0xFFF0) {
        return FID_UF2;
    }

    for (uint32_t i = 0; i < NUM_FILES; i++) {
        if ((info[i].cluster_start <= cluster) && (cluster <= info[i].cluster_end)) {
            return i;
        }
    }

    return FID_UF2;
}

void uf2_init(void)
{
    // TODO maybe limit to application size only if possible board_flash_app_size()
    _flash_size = board_flash_size();

    // update CURRENT.UF2 file size
    info[FID_UF2].size = UF2_BYTE_COUNT;
    if (_ini_file) {
        info[FID_INI].content = _ini_file;
        info[FID_INI].size = strlen(_ini_file);
        info[FID_INI].reserved = CFG_UF2_INI_FILE_SIZE;
    }
    for (int i = 0; i < UF2_TEXT_COUNT; i++) {
        char *content = board_flash_text_content(i);
        if (content) {
            const char *fat_name = board_flash_text_fat_name(i);
            if (fat_name) {
                memcpy(info[FID_PERSONA + i].name, fat_name, 11);
            }
            info[FID_PERSONA + i].content = content;
            info[FID_PERSONA + i].size = strlen(content);
            info[FID_PERSONA + i].reserved = CFG_UF2_TEXT_FILE_SIZE;
        }
    }
    init_starting_clusters();
}

/*------------------------------------------------------------------*/
/* Read CURRENT.UF2
 *------------------------------------------------------------------*/
void padded_memcpy(char *dst, char const *src, int len)
{
    for (int i = 0; i < len; ++i) {
        if (*src) {
            *dst = *src++;
        } else {
            *dst = ' ';
        }
        dst++;
    }
}

/*------------------------------------------------------------------*/
/* [Voice assistant] Text files (CONFIG.INI, PERSONA.TXT, MEMORY.TXT) written elsewhere
 *------------------------------------------------------------------*/

/*
 * Saving an edited file, Windows may put it in other clusters than ours (the free space after CURRENT.UF2):
 * only its directory entry, written by the PC too, tells where it now is and its size. Data sectors written
 * outside the known files are kept until then; once the place is known, its sectors go to the file.
 */
#define TEXT_FILE_COUNT     (FID_TEXT_LAST - FID_INI + 1)
#define CACHED_SECTORS      32
#define NO_SECTOR           UINT32_MAX

typedef struct {
    uint32_t sector;            /* data area sector, NO_SECTOR when unused */
    uint8_t data[BPB_SECTOR_SIZE];
} cached_sector_t;

static cached_sector_t *s_cached = NULL;
static unsigned s_cached_next = 0;

/* Where the PC last wrote each text file (from its directory entry) */
static struct {
    uint32_t first_sector;      /* data area sector of its first cluster */
    uint32_t size;
    bool known;
} s_where[TEXT_FILE_COUNT];

static size_t text_capacity(uint32_t fid)
{
    return fid == FID_INI ? CFG_UF2_INI_FILE_SIZE : CFG_UF2_TEXT_FILE_SIZE;
}

static char *text_dummy(uint32_t fid)
{
    return fid == FID_INI ? _ini_file_dummy : board_flash_text_dummy(fid - FID_PERSONA);
}

static char *text_content(uint32_t fid)
{
    return fid == FID_INI ? _ini_file : board_flash_text_content(fid - FID_PERSONA);
}

/* A written sector, at its offset in the buffer receiving the file */
static void text_put(uint32_t fid, size_t offset, const uint8_t *data)
{
    size_t capacity = text_capacity(fid);
    char *dummy = text_dummy(fid);
    if (!dummy || offset >= capacity - 1) {
        return;
    }
    size_t n = capacity - 1 - offset;
    memcpy(dummy + offset, data, n < BPB_SECTOR_SIZE ? n : BPB_SECTOR_SIZE);
}

/* The received file is parsed and saved if it changed */
static void text_commit(uint32_t fid)
{
    size_t capacity = text_capacity(fid);
    char *dummy = text_dummy(fid);
    char *content = text_content(fid);
    if (!dummy || !content) {
        return;
    }
    dummy[capacity - 1] = '\0';
    if (memcmp(dummy, content, capacity)) {
        if (fid == FID_INI) {
            memcpy(_ini_file, _ini_file_dummy, capacity);
            board_flash_nvs_update(_ini_file_dummy);
        } else {
            board_flash_text_update(fid - FID_PERSONA);
        }
        info[fid].size = strlen(content);
    }
}

/* Text file written by the PC at this data sector (its offset in *offset), -1 if none */
static int text_written_at(uint32_t sector, size_t *offset)
{
    for (int i = 0; i < TEXT_FILE_COUNT; i++) {
        uint32_t sectors = UF2_DIV_CEIL(s_where[i].size, BPB_SECTOR_SIZE);
        if (s_where[i].known && sector >= s_where[i].first_sector && sector < s_where[i].first_sector + sectors) {
            *offset = (sector - s_where[i].first_sector) * BPB_SECTOR_SIZE;
            return FID_INI + i;
        }
    }
    return -1;
}

/* A sector written outside the known files: kept in case a text file was put there */
static void cache_sector(uint32_t sector, const uint8_t *data)
{
    if (!s_cached) {
        s_cached = (cached_sector_t *)malloc(CACHED_SECTORS * sizeof(cached_sector_t));
        if (!s_cached) {
            return;
        }
        for (int i = 0; i < CACHED_SECTORS; i++) {
            s_cached[i].sector = NO_SECTOR;
        }
    }
    for (int i = 0; i < CACHED_SECTORS; i++) {
        if (s_cached[i].sector == sector) {
            memcpy(s_cached[i].data, data, BPB_SECTOR_SIZE);
            return;
        }
    }
    s_cached[s_cached_next].sector = sector;
    memcpy(s_cached[s_cached_next].data, data, BPB_SECTOR_SIZE);
    s_cached_next = (s_cached_next + 1) % CACHED_SECTORS;
}

/* A root directory sector written by the PC: where the text files now are and their size */
static void directory_written(const uint8_t *data)
{
    const DirEntry *d = (const DirEntry *)(const void *)data;
    for (size_t e = 0; e < DIRENTRIES_PER_SECTOR; e++, d++) {
        uint8_t first = (uint8_t)d->name[0];
        if (first == 0x00 || first == 0xE5 || d->attrs == 0x0F || (d->attrs & 0x08) || d->startCluster < 2) {
            continue;   // free, deleted, long name part, volume label, or empty file
        }
        for (uint32_t fid = FID_INI; fid <= FID_TEXT_LAST; fid++) {
            if (memcmp(d->name, info[fid].name, 11) != 0) {
                continue;
            }
            int i = fid - FID_INI;
            uint32_t first_sector = (uint32_t)(d->startCluster - 2) * BPB_SECTORS_PER_CLUSTER;
            if (s_where[i].known && s_where[i].first_sector == first_sector && s_where[i].size == d->size) {
                continue;
            }
            s_where[i].first_sector = first_sector;
            s_where[i].size = d->size;
            s_where[i].known = true;
            // Its sectors written before this entry, then its end at its size
            uint32_t sectors = UF2_DIV_CEIL(d->size, BPB_SECTOR_SIZE);
            for (int c = 0; s_cached && c < CACHED_SECTORS; c++) {
                if (s_cached[c].sector != NO_SECTOR && s_cached[c].sector >= first_sector &&
                        s_cached[c].sector < first_sector + sectors) {
                    text_put(fid, (s_cached[c].sector - first_sector) * BPB_SECTOR_SIZE, s_cached[c].data);
                }
            }
            char *dummy = text_dummy(fid);
            if (dummy && d->size < text_capacity(fid)) {
                dummy[d->size] = '\0';
            }
            text_commit(fid);
        }
    }
}

void uf2_read_block(uint32_t block_no, uint8_t *data)
{
    memset(data, 0, BPB_SECTOR_SIZE);
    uint32_t sectionRelativeSector = block_no;

    if (block_no == 0) {
        // Request was for the Boot block
        memcpy(data, &BootBlock, sizeof(BootBlock));
        data[510] = 0x55;    // Always at offsets 510/511, even when BPB_SECTOR_SIZE is larger
        data[511] = 0xaa;    // Always at offsets 510/511, even when BPB_SECTOR_SIZE is larger
    } else if (block_no < FS_START_ROOTDIR_SECTOR) {
        // Request was for a FAT table sector
        sectionRelativeSector -= FS_START_FAT0_SECTOR;

        // second FAT is same as the first... use sectionRelativeSector to write data
        if (sectionRelativeSector >= BPB_SECTORS_PER_FAT) {
            sectionRelativeSector -= BPB_SECTORS_PER_FAT;
        }

        uint16_t* data16 = (uint16_t*)(void*) data;

        uint32_t sectorFirstCluster = sectionRelativeSector * FAT_ENTRIES_PER_SECTOR;
        uint32_t firstUnusedCluster = info[FID_UF2].cluster_end + 1;

        // OPTIMIZATION:
        // Because all files are contiguous, the FAT CHAIN entries
        // are all set to (cluster+1) to point to the next cluster.
        // All clusters past the last used cluster of the last file
        // are set to zero.
        //
        // EXCEPTIONS:
        // 1. Clusters 0 and 1 require special handling
        // 2. Final cluster of each file must be set to END_OF_CHAIN
        //

        // Set default FAT values first.
        for (uint16_t i = 0; i < FAT_ENTRIES_PER_SECTOR; i++) {
            uint32_t cluster = i + sectorFirstCluster;
            if (cluster >= firstUnusedCluster) {
                data16[i] = 0;
            } else {
                data16[i] = cluster + 1;
            }
        }

        // Exception #1: clusters 0 and 1 need special handling
        if (sectionRelativeSector == 0) {
            data[0] = BPB_MEDIA_DESCRIPTOR_BYTE;
            data[1] = 0xff;
            data16[1] = FAT_END_OF_CHAIN; // cluster 1 is reserved
        }

        // Exception #2: the final cluster of each file must be set to END_OF_CHAIN
        for (uint32_t i = 0; i < NUM_FILES; i++) {
            uint32_t lastClusterOfFile = info[i].cluster_end;
            if (lastClusterOfFile >= sectorFirstCluster) {
                uint32_t idx = lastClusterOfFile - sectorFirstCluster;
                if (idx < FAT_ENTRIES_PER_SECTOR) {
                    // that last cluster of the file is in this sector
                    data16[idx] = FAT_END_OF_CHAIN;
                }
            }
        }
    } else if (block_no < FS_START_CLUSTERS_SECTOR) {
        // Request was for a (root) directory sector .. root because not supporting subdirectories (yet)
        sectionRelativeSector -= FS_START_ROOTDIR_SECTOR;

        DirEntry *d = (void*) data;                   // pointer to next free DirEntry this sector
        int remainingEntries = DIRENTRIES_PER_SECTOR; // remaining count of DirEntries this sector

        uint32_t startingFileIndex;

        if (sectionRelativeSector == 0) {
            // volume label is first directory entry
            padded_memcpy(d->name, (char const*) BootBlock.VolumeLabel, 11);
            d->attrs = 0x28;
            d++;
            remainingEntries--;

            startingFileIndex = 0;
        } else {
            // -1 to account for volume label in first sector
            startingFileIndex = DIRENTRIES_PER_SECTOR * sectionRelativeSector - 1;
        }

        for (uint32_t fileIndex = startingFileIndex;
                remainingEntries > 0 && fileIndex < NUM_FILES; // while space remains in buffer and more files to add...
                fileIndex++, d++) {
            // WARNING -- code presumes all files take exactly one directory entry (no long file names!)
            uint32_t const startCluster = info[fileIndex].cluster_start;
            if (!info[fileIndex].name[0]) {
                d--;        // [Voice assistant] unused phrase file slot: not listed (all entries fit in sector 0)
                continue;
            }

            FileContent_t const *inf = &info[fileIndex];
            padded_memcpy(d->name, inf->name, 11);
            d->createTimeFine   = COMPILE_SECONDS_INT % 2 * 100;
            d->createTime       = COMPILE_DOS_TIME;
            d->createDate       = COMPILE_DOS_DATE;
            d->lastAccessDate   = COMPILE_DOS_DATE;
            d->highStartCluster = startCluster >> 16;
            d->updateTime       = COMPILE_DOS_TIME;
            d->updateDate       = COMPILE_DOS_DATE;
            d->startCluster     = startCluster & 0xFFFF;
            d->size             = (inf->content ? inf->size : (fileIndex == FID_UF2 ? UF2_BYTE_COUNT : 0));
        }
    } else if (block_no < BPB_TOTAL_SECTORS) {
        // Request was to read from the data area (files, unused space, ...)
        sectionRelativeSector -= FS_START_CLUSTERS_SECTOR;

        // [Voice assistant] A text file the PC wrote elsewhere is read back from there
        size_t textOffset;
        int textFid = text_written_at(sectionRelativeSector, &textOffset);
        if (textFid >= 0) {
            FileContent_t const *t = &info[textFid];
            if (t->content && t->size > textOffset) {
                size_t n = t->size - textOffset;
                memcpy(data, (const uint8_t *)t->content + textOffset, n < BPB_SECTOR_SIZE ? n : BPB_SECTOR_SIZE);
            }
            return;
        }

        // plus 2 for first data cluster offset
        uint32_t fid = info_index_of(2 + sectionRelativeSector / BPB_SECTORS_PER_CLUSTER);
        FileContent_t const * inf = &info[fid];

        uint32_t fileRelativeSector = sectionRelativeSector - (info[fid].cluster_start - 2) * BPB_SECTORS_PER_CLUSTER;

        if (fid != FID_UF2) {
            // Handle all files other than CURRENT.UF2
            size_t fileContentStartOffset = fileRelativeSector * BPB_SECTOR_SIZE;
            size_t fileContentLength = inf->size;

            // nothing to copy if already past the end of the file (only when >1 sector per cluster)
            if (fileContentLength > fileContentStartOffset) {
                // obviously, 2nd and later sectors should not copy data from the start
                const void * dataStart = (inf->content) + fileContentStartOffset;
                // limit number of bytes of data to be copied to remaining valid bytes
                size_t bytesToCopy = fileContentLength - fileContentStartOffset;
                // and further limit that to a single sector at a time
                if (bytesToCopy > BPB_SECTOR_SIZE) {
                    bytesToCopy = BPB_SECTOR_SIZE;
                }
                memcpy(data, dataStart, bytesToCopy);
            }
        } else {
            // CURRENT.UF2: generate data on-the-fly
            uint32_t addr = BOARD_FLASH_APP_START + (fileRelativeSector * UF2_FIRMWARE_BYTES_PER_SECTOR);
            if (addr < _flash_size) { // TODO abstract this out
                UF2_Block *bl = (void*) data;
                bl->magicStart0 = UF2_MAGIC_START0;
                bl->magicStart1 = UF2_MAGIC_START1;
                bl->magicEnd = UF2_MAGIC_END;
                bl->blockNo = fileRelativeSector;
                bl->numBlocks = UF2_SECTOR_COUNT;
                bl->targetAddr = addr;
                bl->payloadSize = UF2_FIRMWARE_BYTES_PER_SECTOR;
                bl->flags = UF2_FLAG_FAMILYID;
                bl->familyID = BOARD_UF2_FAMILY_ID;

                board_flash_read(addr, bl->data, bl->payloadSize);
            }
        }
    }
}

/*------------------------------------------------------------------*/
/* Write UF2
 *------------------------------------------------------------------*/

/**
 * Write an uf2 block wrapped by 512 sector.
 * @return number of bytes processed, only 3 following values
 *  -1 : if not an uf2 block
 * 512 : write is successful (BPB_SECTOR_SIZE == 512)
 *   0 : is busy with flashing, tinyusb stack will call write_block again with the same parameters later on
 */
int uf2_write_block(uint32_t block_no, uint8_t *data, WriteState *state)
{
    (void) block_no;
    UF2_Block *bl = (void*) data;

    if (!is_uf2_block(bl)) {
        if (block_no >= FS_START_ROOTDIR_SECTOR && block_no < FS_START_CLUSTERS_SECTOR) {
            // [Voice assistant] The directory tells where the PC put an edited text file
            directory_written(data);
        } else if (block_no >= FS_START_CLUSTERS_SECTOR && block_no < BPB_TOTAL_SECTORS) {
            uint32_t sectionRelativeSector = block_no - FS_START_CLUSTERS_SECTOR;
            // [Voice assistant] Each written sector of a text file is copied at its offset in the file buffer,
            // which is then parsed whole: where the PC put the file if known, else in the file's own clusters
            size_t offset = 0;
            int fid = text_written_at(sectionRelativeSector, &offset);
            if (fid < 0) {
                // plus 2 for first data cluster offset
                uint32_t own = info_index_of(2 + sectionRelativeSector / BPB_SECTORS_PER_CLUSTER);
                if (own >= FID_INI && own <= FID_TEXT_LAST) {
                    fid = own;
                    offset = (sectionRelativeSector - (info[own].cluster_start - 2) * BPB_SECTORS_PER_CLUSTER) *
                             BPB_SECTOR_SIZE;
                } else if (own == FID_UF2) {
                    // Elsewhere: kept until the directory entry says which file it belongs to
                    cache_sector(sectionRelativeSector, data);
                }
            }
            if (fid >= 0) {
                text_put(fid, offset, data);
                text_commit(fid);
            }
        }
        return -1;
    };

    if (bl->familyID == BOARD_UF2_FAMILY_ID) {
        // generic family ID
        board_flash_write(bl->targetAddr, bl->data, bl->payloadSize);
    } else {
        // TODO family matches VID/PID
        return -1;
    }

    //------------- Update written blocks -------------//
    if (bl->numBlocks) {
        // Update state num blocks if needed
        if (state->numBlocks != bl->numBlocks) {
            if (bl->numBlocks >= MAX_BLOCKS || state->numBlocks) {
                state->numBlocks = 0xffffffff;
            } else {
                state->numBlocks = bl->numBlocks;
            }
        }

        if (bl->blockNo < MAX_BLOCKS) {
            uint8_t const mask = 1 << (bl->blockNo % 8);
            uint32_t const pos = bl->blockNo / 8;

            // only increase written number with new write (possibly prevent overwriting from OS)
            if (!(state->writtenMask[pos] & mask)) {
                state->writtenMask[pos] |= mask;
                state->numWritten++;
            }

            // flush last blocks
            // TODO numWritten can be smaller than numBlocks if return early
            if (state->numWritten >= state->numBlocks) {
                board_flash_flush();
            }
        }
    }

    return BPB_SECTOR_SIZE;
}
