#ifndef EXP6_OUTPUT_FILE_H
#define EXP6_OUTPUT_FILE_H

/* 最終名と同じディレクトリで受信し、保存成功時だけrenameする。 */
static int output_fd = -1;
static char *output_temporary;
static const char *output_final;

static void output_discard(void)
{
    if(output_fd >= 0)
        close(output_fd);
    if(output_temporary)
        unlink(output_temporary);
    free(output_temporary);
    output_temporary = NULL;
    output_fd = -1;
}

static int output_open(const char *filename)
{
    size_t length = strlen(filename) + sizeof(".part.XXXXXX");
    output_temporary = malloc(length);
    if(output_temporary == NULL)
        return -1;
    snprintf(output_temporary, length, "%s.part.XXXXXX", filename);
    output_final = filename;
    output_fd = mkstemp(output_temporary);
    if(output_fd < 0) {
        free(output_temporary);
        output_temporary = NULL;
        return -1;
    }
    if(atexit(output_discard) != 0) {
        output_discard();
        errno = ENOMEM;
        return -1;
    }
    return output_fd;
}

static int output_commit(void)
{
    int result = close(output_fd);
    output_fd = -1;
    if(result < 0 || rename(output_temporary, output_final) < 0)
        return -1;
    free(output_temporary);
    output_temporary = NULL;
    return 0;
}

#endif
