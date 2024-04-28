#include <stdio.h>
#include <string.h>

#include <wafel/ios/svc.h>
#include <wafel/utils.h>
#include <wafel/services/fsa.h>


#define CROSS_PROCESS_HEAP_ID 0xcaff
#define COPY_BUFFER_SIZE 1024
#define MAX_PATH_LENGHT 0x27F
#define MAX_DIRECTORY_DEPTH 60

    
static int write_log(int fsaHandle, int logHandle, const char *operation, const char *path, int res, void *dataBuffer){
    snprintf(dataBuffer, COPY_BUFFER_SIZE, "%s;%s;-%08X\n", operation, path, res);
    res = FSA_WriteFile(fsaHandle, dataBuffer, strnlen(dataBuffer, COPY_BUFFER_SIZE), 1, logHandle, 0);
    if(res == 1)
        res = FSA_FlushFile(fsaHandle, logHandle);
    if(res<0){
      debug_printf("Error writing log: -%08X", -res);
      return -1;
    }
    return 0;
}


static int log_error(int fsaHandle, int logHandle, const char *operation, const char *path, int error, void *dataBuffer){
    error_count++;
    debug_printf("Error %s %s: -%08X", operation, path, error);
    return write_log(fsaHandle, logHandle, operation, path, -error, dataBuffer);
}

static int open_dir_e(int fsaHandle, const char *path, int *dir_handle, int logHandle, void *dataBuffer, int *in_out_error_cnt){
    int res = FSA_OpenDir(fsaHandle, path, dir_handle);
    if (res < 0) {
        log_error(fsaHandle, logHandle, "OpenDir", path, res, dataBuffer);
        return 1;
    }
    return 0;
}

static void* malloc_e(size_t sz){
    void *buff = iosAlloc(LOCAL_PROCESS_HEAP_ID, sz);
    if(!buff)
        debug_printf("Out of memory!\n");
    return buff;
}

int copy_file(int fsaFd, const char* src, const char* dst, void* dataBuffer)
{
    int readHandle;
    int res = FSA_OpenFile(fsaFd, src, "r", &readHandle);
    if (res < 0) {
        return res;
    }

    int writeHandle;
    res = FSA_OpenFile(fsaFd, dst, "w", &writeHandle);
    if (res < 0) {
        FSA_CloseFile(fsaFd, readHandle);
        return res;
    }

    while ((res = FSA_ReadFile(fsaFd, dataBuffer, 1, COPY_BUFFER_SIZE, readHandle, 0)) > 0) {
        if ((res = FSA_WriteFile(fsaFd, dataBuffer, 1, res, writeHandle, 0)) < 0) {
            break;
        }
    }

    FSA_CloseFile(fsaFd, writeHandle);
    FSA_CloseFile(fsaFd, readHandle);

    return (res > 0) ? 0 : res;
}

int copy_recursive(int fsaHandle, const char* src, const char* dst, int logHandle){
    void* dataBuffer = NULL;
    char *path = NULL;
    char *dst_path = NULL;
    int ret = -1;
    error_count = 0;


    dataBuffer = iosAllocAligned(CROSS_PROCESS_HEAP_ID, COPY_BUFFER_SIZE, 0x40);
    if (!dataBuffer) {
        debug_printf("Out of IO memory!\n");
        log_error(fsaHandle, logHandle, "AllocDataBuffer", path, res, dataBuffer);
        goto error;
    }

    path = (char*) malloc_e(MAX_PATH_LENGHT + 1);
    if (!path) {
        goto error;
    }
    strncpy(path, src, MAX_DIRECTORY_DEPTH);

    dst_path = (char*) malloc_e(MAX_PATH_LENGHT + 1);
    if (!dst_path) {
        goto error;
    }
    strncpy(dst_path, src, MAX_DIRECTORY_DEPTH);

    int depth = -1;
    int dir_stack[MAX_DIRECTORY_DEPTH] = { 0 };
    int res=open_dir_e(fsaHandle, path, dir_stack, logHandle, dataBuffer, &ret);
    if(res)
        goto error;
    depth = 0;
    uint32_t src_path_len = strnlen(path, MAX_PATH_LENGHT);
    uint32_t dst_path_len = strnlen(dst_path, MAX_PATH_LENGHT);
    dst_path[dst_path_len++] = path[src_path_len++] = '/';
    dst_path[dst_path_len] = path[src_path_len] = '\0';

    ret = 0;
    while(depth >= 0){
        FSDirectoryEntry dir_entry;
        res = FSA_ReadDir(fsaHandle, dir_stack[depth], &dir_entry);
        if(res < 0){
            if(res != END_OF_DIR){
                ret++;
                log_error(fsaHandle, logHandle, "ReadDir", path, res, dataBuffer);
            }
            FSA_CloseDir(fsaHandle, dir_stack[depth]);
            dir_stack[depth] = 0;
            depth--;
            do {
                src_path_len--;
                dst_path_len--;
            } while((src_path_len > 0) && (path[src_path_len - 1] != '/'));
            path[dst_path_len] = path[src_path_len] = '\0';
            continue;
        }
        if(dir_entry.stat.flags & DIR_ENTRY_IS_LINK){
            write_log(fsaHandle, logHandle, "SkipSymlink", path, 0);
            continue; //skip symlinks
        }
        strncpy(path + src_path_len, dir_entry.name, MAX_PATH_LENGHT - (src_path_len + 1));
        strncpy(dst_path + dst_path_len, dir_entry.name, MAX_PATH_LENGHT - (dst_path_len + 1));
        debug_printf("Dumping: %s to %s\n", path, dst_path);
        if(!(dir_entry.stat.flags & DIR_ENTRY_IS_DIRECTORY)){
            res = copy_file(fsaHandle, path, dst_path, dataBuffer);
            if(res < 0){
                log_error(fsaHandle, logHandle, "Copy", path, res, dataBuffer);
                ret = res;
            }
        } else { // Directory
            if(depth >= MAX_DIRECTORY_DEPTH){
                write_log(fsaHandle, logHandle, "ExceedDepth", path, depth, dataBuffer);
                continue;
            }
            res = open_dir_e(fsaHandle, path, dir_stack + depth + 1, logHandle, dataBuffer, &ret);
            if(res < 0){
                ret = res;
                break;
            }
            if(res){
                path[src_path_len] = '\0';
                continue;
            }
            depth++;
            src_path_len = strnlen(path, MAX_PATH_LENGHT);
            path[src_path_len] = '/';
            src_path_len++;
            path[src_path_len] = '\0';
        }
    }

error:
    //TODO: close remaining directories in error case
    for(; depth >= 0; depth--){
        if(dir_stack[depth])
            FSA_CloseDir(fsaHandle, dir_stack[depth]);
    }
    write_log(fsaHandle, logHandle, "finished", base_path, error_count, dataBuffer);

    if(path)
        IOS_HeapFree(LOCAL_PROCESS_HEAP_ID, path);
    if(dataBuffer)
        IOS_HeapFree(CROSS_PROCESS_HEAP_ID, dataBuffer);
    return ret?ret:error_count;
}
