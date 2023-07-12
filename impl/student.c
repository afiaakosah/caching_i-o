#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <stdarg.h>

#include "../io300.h"



/*
    student.c
    Fill in the following stencils
*/

/*
    When starting, you might want to change this for testing on small files.
*/
#ifndef CACHE_SIZE
#define CACHE_SIZE 4096
#endif

#if(CACHE_SIZE < 4)
#error "internal cache size should not be below 4."
#error "if you changed this during testing, that is fine."
#error "when handing in, make sure it is reset to the provided value"
#error "if this is not done, the autograder will not run"
#endif

/*
   This macro enables/disables the dbg() function. Use it to silence your
   debugging info.
   Use the dbg() function instead of printf debugging if you don't want to
   hunt down 30 printfs when you want to hand in
*/
#define DEBUG_PRINT 0
#define DEBUG_STATISTICS 1

struct io300_file {
    /* read,write,seek all take a file descriptor as a parameter */
    int fd;
    /* this will serve as our cache */
    char *cache;

    /* index of the read/write head in the file */
    int curr_pos;
    /* index of the read/write head in the cache */
    int cache_pos;
    /* index of the file at which the cache starts */
    int cache_start;
    /* number of valid bytes (from the file) in the cache */
    int valid_bytes;
    /* flag to represent if the cache has been written to*/
    int modified;
    /* number of bytes in the file */
    int file_size;
    

    /* Used for debugging, keep track of which io300_file is which */
    char *description;
    /* To tell if we are getting the performance we are expecting */
    struct io300_statistics {
        int read_calls;
        int write_calls;
        int seeks;
    } stats;
};

/*
    Assert the properties that you would like your file to have at all times.
    Call this function frequently (like at the beginning of each function) to
    catch logical errors early on in development.
*/
static void check_invariants(struct io300_file *f) {
    assert(f != NULL);
    assert(f->cache != NULL);
    assert(f->fd >= 0);
}

/*
    Wrapper around printf that provides information about the
    given file. You can silence this function with the DEBUG_PRINT macro.
*/
static void dbg(struct io300_file *f, char *fmt, ...) {
    (void)f; (void)fmt;
    #if(DEBUG_PRINT == 1)
        static char buff[300];
        size_t const size = sizeof(buff);
        int n = snprintf(
            buff,
            size,
            // the fields you want to print when debugging
            "{desc:%s, curr_pos:%d, cache_pos:%d, cache_start:%d, valid_bytes:%d} -- ",
            f->description, f->curr_pos, f->cache_pos, f->cache_start, f->valid_bytes
        );
        int const bytes_left = size - n;
        va_list args;
        va_start(args, fmt);
        vsnprintf(&buff[n], bytes_left, fmt, args);
        va_end(args);
        printf("%s", buff);
    #endif
}

/*
    Fill the cache with data from the file.

    Returns the number of bytes read and copied 
    into the cache or or -1 on failure. 
    Returns 0 if at EOF.
*/
int refill_cache(struct io300_file *const f){

    //check if EOF before calling read? 

    f->cache_start = f->curr_pos;
    int read_return = pread(f->fd, f->cache, CACHE_SIZE, f->curr_pos);
    f->stats.read_calls++;
    if (read_return < 0) return -1;
    
    f->cache_pos = 0;
    f->valid_bytes = read_return;
    f->modified = 0;
    return read_return;
}

struct io300_file *io300_open(const char *const path, char *description) {
    if (path == NULL) {
        fprintf(stderr, "error: null file path\n");
        return NULL;
    }

    int const fd = open(path, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
    if (fd == -1) {
        fprintf(stderr, "error: could not open file: `%s`: %s\n", path, strerror(errno));
        return NULL;
    }

    struct io300_file *const ret = malloc(sizeof(*ret));
    if (ret == NULL) {
        fprintf(stderr, "error: could not allocate io300_file\n");
        return NULL;
    }

    ret->fd = fd;
    ret->cache = malloc(CACHE_SIZE);

    if (ret->cache == NULL) {
        fprintf(stderr, "error: could not allocate file cache\n");
        close(ret->fd);
        free(ret);
        return NULL;
    }

    ret->description = description;

    //initialize cache 
    ret->curr_pos = 0; //filehead 
    ret->cache_start = 0; //start of cache is at the first byte of file
    ret->cache_pos = 0; //we are the the beginning of the cache
    int read_return = read(fd, ret->cache, CACHE_SIZE);
    ret->stats.read_calls++;
    ret->valid_bytes = read_return;
    ret->file_size = io300_filesize(ret);
    ret->modified = 0;

    check_invariants(ret);
    dbg(ret, "Just finished initializing file from path: %s\n", path);
    return ret;
}

int io300_seek(struct io300_file *const f, off_t const pos) {
    check_invariants(f);

    off_t new_curr_pos = lseek(f->fd, pos, SEEK_SET);
    if(new_curr_pos == -1) return -1;

    int old_curr = f->curr_pos;
    f->curr_pos = new_curr_pos;

    f->cache_pos += new_curr_pos - old_curr;

    f->stats.seeks++;
    return new_curr_pos;
}

int io300_close(struct io300_file *const f) {
    check_invariants(f);

    #if(DEBUG_STATISTICS == 1)
        printf("stats: {desc: %s, read_calls: %d, write_calls: %d, seeks: %d}\n",
                f->description, f->stats.read_calls, f->stats.write_calls, f->stats.seeks);
    #endif

    io300_flush(f);

    int close_ret = close(f->fd);
    if (close_ret == -1) {
        free(f->cache);
        free(f);
        return -1;
    } else 
    free(f->cache);
    free(f);
    return 0;
}

off_t io300_filesize(struct io300_file *const f) {
    check_invariants(f);
    struct stat s;
    int const r = fstat(f->fd, &s);
    if (r >= 0 && S_ISREG(s.st_mode)) {
        return s.st_size;
    } else {
        return -1;
    }
}


/*
    readc() reads a single byte from the file and returns it.
    Return -1 on failure or if the end of the file has been reached.
*/
int io300_readc(struct io300_file *const f) {
    check_invariants(f);
    unsigned char c;
    char* cache = f->cache;

    //at EOF or invalid pos
    if(f->curr_pos >= f->file_size || f->curr_pos < 0){
        if(f->modified){
            io300_flush(f);
        }
        return -1;
    }

    //if we have seeked beyond the cache
    if(f->curr_pos < f->cache_start || f->curr_pos >= f->cache_start + f->valid_bytes){
        io300_flush(f);
        refill_cache(f);
    }

    //if exhausted the valid bytes in cache
    //refill the cache with data from the file.
    if (f->cache_pos >= f->valid_bytes) {

        //flush (if necessary)
        if(f->modified){
            io300_flush(f);
        } 

        if (refill_cache(f) <= 0) {
            return -1; // end of file or error
        }

    }
    
    c = (unsigned char) cache[f->cache_pos];
    //increment metadata
    f->curr_pos++;
    f->cache_pos++;
    return c;
}

/*
    writes a single byte to the file.
    Returns the byte that was written upon success and -1 on failure.
*/
int io300_writec(struct io300_file *f, int ch) {
    check_invariants(f);

    char const c = (char)ch;
    int cache_pos = f->cache_pos;
    char* cache = f->cache;
    int valid_bytes = f->valid_bytes; 

    //if exhausted the valid bytes in cache
    if(cache_pos == CACHE_SIZE){         
        //flush (if necessary)
        if(f->modified){
            io300_flush(f);
        } 
        refill_cache(f); 
        cache_pos = f->cache_pos;
        valid_bytes = f->valid_bytes;
    }

    cache[cache_pos] = (unsigned char) c;
    if (cache_pos == valid_bytes){
        f->valid_bytes++;
    }
    f->modified = 1;
    f->curr_pos++;
    f->cache_pos++;
    return 1;
}

/*
    Read `nbytes` from the file into `buff`. Assume that the buffer is large enough.
    On failure, return -1. On success, return the number of bytes that were
    placed into the provided buffer.

    Returns 0 at EOF.
*/
ssize_t io300_read(struct io300_file *const f, char *const buff, size_t const sz) {
    check_invariants(f);

    char* cache = f->cache;

    if(f->curr_pos >= f->file_size){
        //EOF
        if(f->modified){
            io300_flush(f);
        }
        return 0;
    }

    //if exhausted the valid bytes in cache (f->cache_pos >= CACHE_SIZE)
    if(f->cache_pos >= f->valid_bytes || f->cache_pos < 0 ){         
        //flush (if necessary) and refill cache
        if(f->modified){
            io300_flush(f);
        } 
        refill_cache(f);
    }

    int read_return = 0;

    if(f->cache_pos < f->valid_bytes){

        int valid_reads = f->valid_bytes - f->cache_pos;
        char* cache_offset = cache + f->cache_pos;

        //if the buff is smaller than the cache 
        if(sz <= (size_t) valid_reads){
            memcpy(buff, cache_offset, sz);
            f->curr_pos += sz;
            f->cache_pos += sz;
            read_return = sz;
        } else {
            int read_ret = pread(f->fd,buff, sz, f->curr_pos);
            read_return = read_ret;
            f->curr_pos += read_ret;
            
            refill_cache(f);
        }

    }

    return read_return;    
}

/*
    Write `nbytes` from the start of `buff` into the file. Assume that the buffer
    is large enough.
    On failure, return -1. On success, return the number of bytes that were
    written to the file.
*/
ssize_t io300_write(struct io300_file *const f, const char *buff, size_t const sz) {
    check_invariants(f);
    // return write(f->fd, buff, sz);

    char* cache = f->cache;

    //if we are not in the cache
    if(f->cache_pos >= CACHE_SIZE || f->cache_pos < 0 ){         
        //flush (if necessary) and refill cache
        if(f->modified){
            io300_flush(f);
        } 
        refill_cache(f);
    }

    int write_return = 0;

    //if the buff is smaller than the cache
    if(sz <= (size_t) CACHE_SIZE - f->cache_pos){  
        char* cache_offset = cache + f->cache_pos;
        memcpy(cache_offset, buff, sz);

        //edit metadata
        if (f->cache_pos == f->valid_bytes){
            f->valid_bytes += sz;
        }

        if(f->curr_pos == f->file_size){
            f->file_size =+ sz;
        }

        f->curr_pos += sz;
        f->cache_pos += sz;
        write_return = sz;
        f->modified = 1;

    } else {
        int write_ret = pwrite(f->fd, buff, sz, f->curr_pos);
        write_return = write_ret;
        f->curr_pos += write_ret;
        f->cache_pos += write_ret;

        if(f->curr_pos > f->file_size){
            f->file_size = f->curr_pos;
        }

    }

    return write_return;  

}



/*
    Flush any in-RAM data (caches) to disk.

    Returns the CACHE_SIZE (number of bytes written)
    or 0 if the cache was not modified.
*/
int io300_flush(struct io300_file *const f) {
    check_invariants(f);
    
    if(f->modified){ //if the cache was modified, write to disk
        int write_return = pwrite(f->fd, f->cache, f->valid_bytes, f->cache_start);
        f->stats.write_calls++;
        return write_return;
    }

    return 0;
}