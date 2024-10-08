/*
  FUSE ssd: FUSE ioctl example
  Copyright (C) 2008       SUSE Linux Products GmbH
  Copyright (C) 2008       Tejun Heo <teheo@suse.de>
  This program can be distributed under the terms of the GNU GPLv2.
  See the file COPYING.
*/
#define FUSE_USE_VERSION 35
#include <fuse.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include "ssd_fuse_header.h"
#define SSD_NAME       "ssd_file"
#define transform(pca) (pca.block * PAGE_NUM_PER_NAND + pca.page)

enum
{
    SSD_NONE,
    SSD_ROOT,
    SSD_FILE,
};


static size_t physic_size;
static size_t logic_size;
static size_t host_write_size;
static size_t nand_write_size;

typedef union pca_rule PCA_RULE;
union pca_rule
{
    unsigned int pca;
    struct
    {
        unsigned int page : 16;
        unsigned int block: 16;
    } ;
};

typedef struct state_rule STATE_RULE;
struct state_rule
{
    union
    {
        unsigned int state;
        struct
        {
            unsigned int free       : 20;
            unsigned int stale      : 20;
            unsigned int valid_count: 2;
        };
    };
};

PCA_RULE curr_pca;
STATE_RULE* block_state;

PCA_RULE* L2P;
unsigned int* P2L;
unsigned int* free_block_number, gc_blockid;

static int ssd_resize(size_t new_size)
{
    //set logic size to new_size
    if (new_size >= LOGICAL_NAND_NUM * NAND_SIZE_KB * 1024  )
    {
        return -ENOMEM;
    }
    else
    {
        logic_size = new_size;
        return 0;
    }

}

static int gc(void);



// static int ssd_expand(size_t new_size)
// {
//     //logical size must be less than logic limit

//     if (new_size > logic_size)
//     {
//         return ssd_resize(new_size);
//     }

//     return 0;
// }
static int ssd_expand(size_t new_size)
{
    //logic must less logic limit

    if (new_size > logic_size)
    {
        return ssd_resize(new_size);
    }

    return 0;
}

static int nand_read(char* buf, int pca)
{
    char nand_name[100];
    FILE* fptr;

    PCA_RULE my_pca;
    my_pca.pca = pca;
    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, my_pca.block);

    //read from nand
    if ( (fptr = fopen(nand_name, "r") ))
    {
        fseek( fptr, my_pca.page * 512, SEEK_SET );
        fread(buf, 1, 512, fptr);
        fclose(fptr);
    }
    else
    {
        printf("open file fail at nand read pca = %d\n", pca);
        return -EINVAL;
    }
    return 512;
}
static int nand_write(const char* buf, int pca)
{
    char nand_name[100];
    FILE* fptr;

    PCA_RULE my_pca;
    my_pca.pca = pca;
    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, my_pca.block);

    //write to nand
    if ( (fptr = fopen(nand_name, "r+")))
    {
        fseek( fptr, my_pca.page * 512, SEEK_SET );
        fwrite(buf, 1, 512, fptr);
        fclose(fptr);
        physic_size ++;
        block_state[my_pca.block].valid_count++;
        block_state[my_pca.block].free &= ~(1 << my_pca.page); // use & to invalid writen page
    }
    else
    {
        printf("open file fail at nand (%s) write pca = %d, return %d\n", nand_name, pca, -EINVAL);
        return -EINVAL;
    }

    nand_write_size += 512;
    return 512;
}

static int nand_erase(int block)
{
    char nand_name[100];
	int found = 0;
    FILE* fptr;

    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, block);

    //erase nand
    if ( (fptr = fopen(nand_name, "w")))
    {
        fclose(fptr);
        found = 1;
    }
    else
    {
        printf("open file fail at nand (%s) erase nand = %d, return %d\n", nand_name, block, -EINVAL);
        return -EINVAL;
    }


	if (found == 0)
	{
		printf("nand erase not found\n");
		return -EINVAL;
	}
    block_state[block].state = FREE_BLOCK;
    printf("nand erase %d pass\n", block);
    return 1;
}

static unsigned int get_next_block()
{
    for (int i = 0; i < PHYSICAL_NAND_NUM; i++)
    {
        if (block_state[(curr_pca.block + i) % PHYSICAL_NAND_NUM].state == FREE_BLOCK)
        {
            curr_pca.block = (curr_pca.block + i) % PHYSICAL_NAND_NUM;
            curr_pca.page = 0;
            free_block_number--;
            block_state[curr_pca.block].state = 0;
            block_state[curr_pca.block].free = (1 << PAGE_NUM_PER_NAND) - 1;
            return curr_pca.pca;
        }
    }
    return OUT_OF_BLOCK;
}

static unsigned int get_next_pca()
{
    /*  TODO: seq A, need to change to seq B */
	
    if (curr_pca.pca == INVALID_PCA)
    {
        //init
        curr_pca.pca = 0;
        block_state[0].state = 0;
        block_state[curr_pca.block].free = (1 << PAGE_NUM_PER_NAND) - 1;
        free_block_number--;
        printf("=========================================\n");
        printf("PCA = page %d, nand %d\n", curr_pca.page, curr_pca.block);
        return curr_pca.pca;
    }
    else if (curr_pca.pca == FULL_PCA)
    {
        //ssd is full, no pca can be allocated
        printf("No new PCA\n");
        return FULL_PCA;
    }

    do
    {
        if (block_state[curr_pca.block].free)
        {   
            // get where 1 is, equl to page
            curr_pca.page = ffs(block_state[curr_pca.block].free) - 1;
            printf("=========================================\n");
            printf("PCA = page %d, nand %d\n", curr_pca.page, curr_pca.block);
            return curr_pca.pca;
        }

        if (free_block_number != 0)
        {
            return get_next_block();
        }

        gc();
    } while (1);
    
}

static int ftl_read( char* buf, size_t lba)
{
    PCA_RULE pca;

	pca.pca = L2P[lba].pca;
	if (pca.pca == INVALID_PCA) {
	    //data has not be written, return 0
	    return 0;
	}
	else {
	    return nand_read(buf, pca.pca);
	}
}

static int ftl_write(const char* buf, size_t lba_rnage, size_t lba)
{
    /*  TODO: only basic write case, need to consider other cases */
    PCA_RULE pca, oldpca;
    int ret;

    oldpca.pca = L2P[lba].pca;

    // Set stale
    if (oldpca.pca != INVALID_PCA)
    {
        // stale, so valid -1
        block_state[oldpca.block].valid_count--;
        block_state[oldpca.block].stale |= (1 << oldpca.page);
    }

    pca.pca = get_next_pca();

    if (pca.pca < 0 || pca.pca == OUT_OF_BLOCK)
    {
        return 0;
    }

    ret = nand_write(buf, pca.pca);

    L2P[lba].pca = pca.pca;
    P2L[transform(pca)] = lba;

    return ret;
}

static int ssd_file_type(const char* path)
{
    if (strcmp(path, "/") == 0)
    {
        return SSD_ROOT;
    }
    if (strcmp(path, "/" SSD_NAME) == 0)
    {
        return SSD_FILE;
    }
    return SSD_NONE;
}
static int ssd_getattr(const char* path, struct stat* stbuf,
                       struct fuse_file_info* fi)
{
    (void) fi;
    stbuf->st_uid = getuid();
    stbuf->st_gid = getgid();
    stbuf->st_atime = stbuf->st_mtime = time(NULL);
    switch (ssd_file_type(path))
    {
        case SSD_ROOT:
            stbuf->st_mode = S_IFDIR | 0755;
            stbuf->st_nlink = 2;
            break;
        case SSD_FILE:
            stbuf->st_mode = S_IFREG | 0644;
            stbuf->st_nlink = 1;
            stbuf->st_size = logic_size;
            break;
        case SSD_NONE:
            return -ENOENT;
    }
    return 0;
}
static int ssd_open(const char* path, struct fuse_file_info* fi)
{
    (void) fi;
    if (ssd_file_type(path) != SSD_NONE)
    {
        return 0;
    }
    return -ENOENT;
}
static int ssd_do_read(char* buf, size_t size, off_t offset)
{
    int tmp_lba;
    int idx, curr_size, remain_size;
    char* tmp_buf;

    //off limit
    if (offset >= logic_size)
    {
        return 0;
    }
    if (size > logic_size - offset)
    {
        //is valid data section
        size = logic_size - offset;
    }

    tmp_lba = offset / PAGESIZE;
    tmp_buf = calloc(PAGESIZE, sizeof(char));

    idx = 0;
    curr_size = 0;
    remain_size = size;
    int cnt = 0
    while (remain_size > 0)
    {
        int ret, rsize;

        ret = ftl_read(tmp_buf, tmp_lba + idx);

        if (ret = 0)
        {
            memset(tmp_buf + cnt * 512, 0, 512);
        }
        else if (rst < 0 )
        {
            free(tmp_buf);
            return ret;
        }

        offset = (offset + curr_size) % PAGESIZE;
        rsize = remain_size > PAGESIZE - offset ? PAGESIZE - offset : remain_size;

        memcpy(&buf[curr_size], &tmp_buf[offset], rsize);

        cnt++;
        idx += 1;
        curr_size += rsize;
        remain_size -= rsize;
    }

    free(tmp_buf);

    return curr_size;
}
static int ssd_read(const char* path, char* buf, size_t size,
                    off_t offset, struct fuse_file_info* fi)
{
    (void) fi;
    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }
    return ssd_do_read(buf, size, offset);
}
static int ssd_do_write(const char* buf, size_t size, off_t offset)
{
    int tmp_lba, tmp_lba_range;
    int idx, curr_size, remain_size;
    
    host_write_size += size;
    if (ssd_expand(offset + size) != 0)
    {
        return -ENOMEM;
    }

    char* tmp_buf;
    int ret;

    tmp_lba = offset / PAGESIZE;
    tmp_lba_range = (offset + size - 1) / PAGESIZE - (tmp_lba) + 1;
    
    tmp_buf = calloc(PAGESIZE, sizeof(char));

    idx = 0;
    curr_size = 0;
    remain_size = size;

    while (remain_size > 0)
    {
        int write_size;

        offset = (offset + curr_size) % PAGESIZE;
        write_size = remain_size > PAGESIZE - offset ? PAGESIZE - offset : remain_size;

        if (write_size != PAGESIZE) 
        {
            //Partial overwrite, read-modify-write
            ret = ftl_read(tmp_buf, tmp_lba + idx);
            if (ret <= 0)
            {
                return ret;
            }
            memcpy(&tmp_buf[offset], &buf[curr_size], write_size);

            //Write which page
            ret = ftl_write(tmp_buf, 1, tmp_lba + idx);
            if (ret <= 0)
            {
                return ret;
            }
        } 
        else
        {
            //Just overwrite whole page
            ret = ftl_write(&buf[curr_size], tmp_lba_range - idx, tmp_lba + idx);

            if (ret <= 0)
            {
                return ret;
            }
        }

        idx += 1;
        curr_size += write_size;
        remain_size -= write_size;
    }

    free(tmp_buf);

    return curr_size;
}
static int ssd_write(const char* path, const char* buf, size_t size,
                     off_t offset, struct fuse_file_info* fi)
{
    (void) fi;
    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }
    return ssd_do_write(buf, size, offset);
}
static int ssd_truncate(const char* path, off_t size,
                        struct fuse_file_info* fi)
{
    (void) fi;
    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }

    return ssd_resize(size);
}
static int ssd_readdir(const char* path, void* buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info* fi,
                       enum fuse_readdir_flags flags)
{
    (void) fi;
    (void) offset;
    (void) flags;
    if (ssd_file_type(path) != SSD_ROOT)
    {
        return -ENOENT;
    }
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    filler(buf, SSD_NAME, NULL, 0, 0);
    return 0;
}
static int ssd_ioctl(const char* path, unsigned int cmd, void* arg,
                     struct fuse_file_info* fi, unsigned int flags, void* data)
{

    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }
    if (flags & FUSE_IOCTL_COMPAT)
    {
        return -ENOSYS;
    }
    switch (cmd)
    {
        case SSD_GET_LOGIC_SIZE:
            *(size_t*)data = logic_size;
            printf(" --> logic size: %ld\n", logic_size);
            return 0;
        case SSD_GET_PHYSIC_SIZE:
            *(size_t*)data = physic_size;
            printf(" --> physic size: %ld\n", physic_size);
            return 0;
        case SSD_GET_WA:
            *(double*)data = (double)nand_write_size / (double)host_write_size;
            return 0;
    }
    return -EINVAL;
}
static const struct fuse_operations ssd_oper =
{
    .getattr        = ssd_getattr,
    .readdir        = ssd_readdir,
    .truncate       = ssd_truncate,
    .open           = ssd_open,
    .read           = ssd_read,
    .write          = ssd_write,
    .ioctl          = ssd_ioctl,
};

static int gc(void)
{
    int blockid, minvalid, valid;
    char* buf;
    PCA_RULE target_pca;

    blockid = -1;
    minvalid = PAGE_NUM_PER_NAND + 1;

    // find min valid page block
    for (int i = 0; i < PHYSICAL_NAND_NUM; ++i)
    {
        if (i == gc_blockid)
        {
            continue;
        }

        if (block_state[i].valid_count < minvalid)
        {
            minvalid = block_state[i].valid_count;
            blockid = i;
        }
    }

    if (minvalid == PAGE_NUM_PER_NAND)
    {
        return -1;
    }

    buf = calloc(PAGESIZE, sizeof(char));

    target_pca.block = blockid;

    // last FREE_BLOCK
    curr_pca.block = gc_blockid;
    curr_pca.page = 0;
    block_state[gc_blockid].state = 0;
    block_state[gc_blockid].free = (1 << PAGE_NUM_PER_NAND) - 1;

    // get page be occupied
    valid = ~block_state[blockid].free & ((1 << PAGE_NUM_PER_NAND) - 1);
    // get page should be kept
    valid &= ~block_state[blockid].stale;

    // move valid page to new block
    while (valid)
    {
        int page;
        int lba;

        page = ffs(valid) - 1;

        if (page == -1) {
            break;
        }

        target_pca.page = page;

        nand_read(buf, target_pca.pca);
        // last free page
        nand_write(buf, curr_pca.pca);

        lba = P2L[transform(target_pca)];
        
        // change pointer target to last free page
        L2P[lba].pca = curr_pca.pca;

        P2L[transform(curr_pca)] = lba;
        P2L[transform(target_pca)] = INVALID_LBA;
        
        curr_pca.page++;

        // remove the page already moved
        valid &= ~(1 << page);
    }

    free(buf);

    nand_erase(blockid);

    // update last free block
    gc_blockid = blockid;

    return 0;
}



int main(int argc, char* argv[])
{
    int idx;
    char nand_name[100];
    physic_size = 0;
    logic_size = 0;
	nand_write_size = 0;
	host_write_size = 0;
    free_block_number = PHYSICAL_NAND_NUM;
    curr_pca.pca = INVALID_PCA;
    L2P = malloc(LOGICAL_NAND_NUM * PAGE_NUM_PER_NAND * sizeof(int));
    memset(L2P, INVALID_PCA, sizeof(int) * LOGICAL_NAND_NUM * PAGE_NUM_PER_NAND);
    P2L = malloc(PHYSICAL_NAND_NUM * PAGE_NUM_PER_NAND * sizeof(int));
    memset(P2L, INVALID_LBA, sizeof(int) * PHYSICAL_NAND_NUM * PAGE_NUM_PER_NAND);
    block_state = malloc(PHYSICAL_NAND_NUM * sizeof(STATE_RULE));
    memset(block_state, FREE_BLOCK, sizeof(STATE_RULE) * PHYSICAL_NAND_NUM);

    //create nand file
    for (idx = 0; idx < PHYSICAL_NAND_NUM; idx++)
    {
        FILE* fptr;
        snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, idx);
        fptr = fopen(nand_name, "w");
        if (fptr == NULL)
        {
            printf("open fail");
        }
        fclose(fptr);
    }
    return fuse_main(argc, argv, &ssd_oper, NULL);
}
